// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/VideoEncoder.h>

#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <spdlog/spdlog.h>

#include <vector>

namespace ge {

struct VideoEncoder::M {
    VTCompressionSessionRef session = nullptr;
    FrameCallback callback;
    int width;
    int height;
    int fps;
    int64_t frameCount = 0;
    std::vector<uint8_t> bgra;  // reused RGBA→BGRA scratch (see encode())

    void createSession();
    static void outputCallback(void* ctx, void* sourceFrameRefCon,
                                OSStatus status, VTEncodeInfoFlags flags,
                                CMSampleBufferRef sampleBuffer);
};

void VideoEncoder::M::createSession() {
    NSDictionary* pixelBufferAttrs = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (NSString*)kCVPixelBufferWidthKey: @(width),
        (NSString*)kCVPixelBufferHeightKey: @(height),
    };

    OSStatus err = VTCompressionSessionCreate(
        nullptr, width, height,
        kCMVideoCodecType_H264,
        nullptr,
        (__bridge CFDictionaryRef)pixelBufferAttrs,
        nullptr,
        &M::outputCallback,
        this,
        &session
    );

    if (err != noErr) {
        SPDLOG_ERROR("VTCompressionSessionCreate failed: {}", static_cast<int>(err));
        return;
    }

    VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_ProfileLevel,
                         kVTProfileLevel_H264_High_AutoLevel);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AllowFrameReordering,
                         kCFBooleanFalse);

    // Zero-frame encoder delay for lowest latency
    int32_t maxDelay = 0;
    CFNumberRef delayRef = CFNumberCreate(nullptr, kCFNumberSInt32Type, &maxDelay);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxFrameDelayCount, delayRef);
    CFRelease(delayRef);

    // High bitrate for crisp quality on localhost — 16 Mbps
    int32_t bitrate = 16000000;
    CFNumberRef bitrateRef = CFNumberCreate(nullptr, kCFNumberSInt32Type, &bitrate);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, bitrateRef);
    CFRelease(bitrateRef);

    // Keyframe every 1s (less frequent = better P-frame quality)
    int32_t maxKeyFrameInterval = fps;
    CFNumberRef keyFrameRef = CFNumberCreate(nullptr, kCFNumberSInt32Type, &maxKeyFrameInterval);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval, keyFrameRef);
    CFRelease(keyFrameRef);

    VTCompressionSessionPrepareToEncodeFrames(session);
    SPDLOG_INFO("VideoEncoder: created {}x{} @ {} fps, 16 Mbps H.264 High, keyframe every {}",
                width, height, fps, maxKeyFrameInterval);
}

void VideoEncoder::M::outputCallback(void* ctx, void* /*sourceFrameRefCon*/,
                                      OSStatus status, VTEncodeInfoFlags /*flags*/,
                                      CMSampleBufferRef sampleBuffer) {
    if (status != noErr || !sampleBuffer) {
        SPDLOG_ERROR("VideoEncoder output callback error: {}", static_cast<int>(status));
        return;
    }

    auto* self = static_cast<M*>(ctx);

    // Get the encoded data — this is the complete AVCC-formatted frame
    // including parameter sets for keyframes.
    CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!blockBuffer) return;

    size_t totalLength = 0;
    char* dataPointer = nullptr;
    OSStatus blockErr = CMBlockBufferGetDataPointer(blockBuffer, 0, nullptr,
                                                     &totalLength, &dataPointer);
    if (blockErr != noErr || !dataPointer || totalLength == 0) return;

    // Check if keyframe
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    bool isKeyframe = false;
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFBooleanRef notSync = (CFBooleanRef)CFDictionaryGetValue(dict,
            kCMSampleAttachmentKey_NotSync);
        isKeyframe = !notSync || !CFBooleanGetValue(notSync);
    }

    // For keyframes, prepend SPS and PPS from the format description
    // so the decoder can initialize from any keyframe without out-of-band data.
    std::vector<uint8_t> frameData;
    if (isKeyframe) {
        CMFormatDescriptionRef formatDesc = CMSampleBufferGetFormatDescription(sampleBuffer);
        if (formatDesc) {
            size_t paramCount = 0;
            CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                formatDesc, 0, nullptr, nullptr, &paramCount, nullptr);

            for (size_t i = 0; i < paramCount; i++) {
                const uint8_t* paramData = nullptr;
                size_t paramSize = 0;
                CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                    formatDesc, i, &paramData, &paramSize, nullptr, nullptr);
                if (paramData && paramSize > 0) {
                    // AVCC length prefix
                    uint32_t len = static_cast<uint32_t>(paramSize);
                    uint8_t lenBytes[4] = {
                        uint8_t(len >> 24), uint8_t(len >> 16),
                        uint8_t(len >> 8), uint8_t(len)
                    };
                    frameData.insert(frameData.end(), lenBytes, lenBytes + 4);
                    frameData.insert(frameData.end(), paramData, paramData + paramSize);
                }
            }
        }
    }

    // Append the encoded frame data (already AVCC-formatted with length prefixes)
    frameData.insert(frameData.end(),
                     reinterpret_cast<const uint8_t*>(dataPointer),
                     reinterpret_cast<const uint8_t*>(dataPointer) + totalLength);

    Frame frame{frameData.data(), frameData.size(), isKeyframe};
    self->callback(frame);
}

VideoEncoder::VideoEncoder(int width, int height, int fps, FrameCallback onFrame)
    : m(std::make_unique<M>()) {
    m->width = width;
    m->height = height;
    m->fps = fps;
    m->callback = std::move(onFrame);
    m->createSession();
}

VideoEncoder::~VideoEncoder() {
    if (m->session) {
        VTCompressionSessionInvalidate(m->session);
        CFRelease(m->session);
    }
}

void VideoEncoder::encode(const uint8_t* rgbaPixels, size_t bytesPerRow) {
    if (!m->session) return;

    // ge's frame-capture contract delivers RGBA (SokolContext normalizes every
    // backend's readback to RGBA); VideoToolbox wants BGRA. Swap R↔B into a
    // reused scratch. (On Metal this undoes the capture's BGRA→RGBA swap — a
    // native-BGRA stream-capture path could elide both, tracked separately.)
    const size_t rows = static_cast<size_t>(m->height);
    m->bgra.resize(bytesPerRow * rows);
    for (size_t y = 0; y < rows; ++y) {
        const uint8_t* s = rgbaPixels + y * bytesPerRow;
        uint8_t* d = m->bgra.data() + y * bytesPerRow;
        for (size_t x = 0; x + 4 <= bytesPerRow; x += 4) {
            d[x + 0] = s[x + 2];  // B ← R
            d[x + 1] = s[x + 1];  // G
            d[x + 2] = s[x + 0];  // R ← B
            d[x + 3] = s[x + 3];  // A
        }
    }

    CVPixelBufferRef pixelBuffer = nullptr;
    OSStatus err = CVPixelBufferCreateWithBytes(
        nullptr, m->width, m->height,
        kCVPixelFormatType_32BGRA,
        m->bgra.data(),
        bytesPerRow,
        nullptr, nullptr, nullptr,
        &pixelBuffer
    );

    if (err != noErr || !pixelBuffer) {
        SPDLOG_ERROR("CVPixelBufferCreateWithBytes failed: {}", static_cast<int>(err));
        return;
    }

    CMTime pts = CMTimeMake(m->frameCount++, m->fps);
    err = VTCompressionSessionEncodeFrame(
        m->session, pixelBuffer, pts,
        kCMTimeInvalid, nullptr, nullptr, nullptr
    );

    CVPixelBufferRelease(pixelBuffer);

    if (err != noErr) {
        SPDLOG_ERROR("VTCompressionSessionEncodeFrame failed: {}", static_cast<int>(err));
    }
}

void VideoEncoder::encode(CVPixelBufferRef pixelBuffer) {
    if (!m->session || !pixelBuffer) return;

    CMTime pts = CMTimeMake(m->frameCount++, m->fps);
    OSStatus err = VTCompressionSessionEncodeFrame(
        m->session, pixelBuffer, pts,
        kCMTimeInvalid, nullptr, nullptr, nullptr
    );

    if (err != noErr) {
        SPDLOG_ERROR("VTCompressionSessionEncodeFrame (CVPixelBuffer) failed: {}",
                     static_cast<int>(err));
    }
}

void VideoEncoder::flush() {
    if (m->session) {
        VTCompressionSessionCompleteFrames(m->session, kCMTimeInvalid);
    }
}

void VideoEncoder::resize(int width, int height) {
    if (m->session) {
        VTCompressionSessionInvalidate(m->session);
        CFRelease(m->session);
        m->session = nullptr;
    }
    m->width = width;
    m->height = height;
    m->frameCount = 0;
    m->createSession();
}

} // namespace ge
