// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/VideoDecoder.h>

#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <spdlog/spdlog.h>

#include <vector>

namespace ge {

struct VideoDecoder::M {
    FrameCallback callback;
    VTDecompressionSessionRef session = nullptr;
    CMVideoFormatDescriptionRef formatDesc = nullptr;

    ~M() {
        if (session) {
            VTDecompressionSessionWaitForAsynchronousFrames(session);
            VTDecompressionSessionInvalidate(session);
            CFRelease(session);
        }
        if (formatDesc) {
            CFRelease(formatDesc);
        }
    }

    void createSession();

    static void outputCallback(void* decompressionOutputRefCon,
                                void* sourceFrameRefCon,
                                OSStatus status,
                                VTDecodeInfoFlags infoFlags,
                                CVImageBufferRef imageBuffer,
                                CMTime presentationTimeStamp,
                                CMTime presentationDuration);
};

void VideoDecoder::M::createSession() {
    if (!formatDesc) {
        SPDLOG_ERROR("VideoDecoder: no format description, cannot create session");
        return;
    }

    // Clean up any existing session
    if (session) {
        VTDecompressionSessionWaitForAsynchronousFrames(session);
        VTDecompressionSessionInvalidate(session);
        CFRelease(session);
        session = nullptr;
    }

    // Request BGRA output
    NSDictionary* destImageBufferAttrs = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
    };

    VTDecompressionOutputCallbackRecord callbackRecord{};
    callbackRecord.decompressionOutputCallback = &M::outputCallback;
    callbackRecord.decompressionOutputRefCon = this;

    OSStatus err = VTDecompressionSessionCreate(
        nullptr,                                          // allocator
        formatDesc,                                       // videoFormatDescription
        nullptr,                                          // videoDecoderSpecification
        (__bridge CFDictionaryRef)destImageBufferAttrs,    // destinationImageBufferAttributes
        &callbackRecord,                                  // outputCallback
        &session
    );

    if (err != noErr) {
        SPDLOG_ERROR("VTDecompressionSessionCreate failed: {}", static_cast<int>(err));
        session = nullptr;
        return;
    }

    SPDLOG_INFO("VideoDecoder: decompression session created");
}

void VideoDecoder::M::outputCallback(void* decompressionOutputRefCon,
                                      void* /*sourceFrameRefCon*/,
                                      OSStatus status,
                                      VTDecodeInfoFlags /*infoFlags*/,
                                      CVImageBufferRef imageBuffer,
                                      CMTime /*presentationTimeStamp*/,
                                      CMTime /*presentationDuration*/) {
    if (status != noErr) {
        // Rate-limit: multi-tile path can spam this under transient bad AUs.
        static thread_local int errBudget = 0;
        if (errBudget++ < 8 || (errBudget % 200) == 0) {
            SPDLOG_ERROR("VideoDecoder callback error: {} (n={})",
                         static_cast<int>(status), errBudget);
        }
        return;
    }
    if (!imageBuffer) return;

    auto* self = static_cast<M*>(decompressionOutputRefCon);

    CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);

    int width = static_cast<int>(CVPixelBufferGetWidth(imageBuffer));
    int height = static_cast<int>(CVPixelBufferGetHeight(imageBuffer));
    size_t bytesPerRow = CVPixelBufferGetBytesPerRow(imageBuffer);
    const uint8_t* baseAddr = static_cast<const uint8_t*>(
        CVPixelBufferGetBaseAddress(imageBuffer));

    if (baseAddr) {
        VideoFrame f;
        f.format = VideoFrame::Format::BGRA;
        f.width = width;
        f.height = height;
        f.planes[0] = baseAddr;
        f.strides[0] = static_cast<int>(bytesPerRow);
        self->callback(f);
    }

    CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
}

VideoDecoder::VideoDecoder(FrameCallback onFrame)
    : m(std::make_unique<M>()) {
    m->callback = std::move(onFrame);
}

VideoDecoder::~VideoDecoder() = default;

void VideoDecoder::setParameterSets(const uint8_t* sps, size_t spsSize,
                                     const uint8_t* pps, size_t ppsSize) {
    // Release old format description
    if (m->formatDesc) {
        CFRelease(m->formatDesc);
        m->formatDesc = nullptr;
    }

    const uint8_t* paramSets[2] = {sps, pps};
    size_t paramSizes[2] = {spsSize, ppsSize};

    OSStatus err = CMVideoFormatDescriptionCreateFromH264ParameterSets(
        nullptr,       // allocator
        2,             // parameterSetCount
        paramSets,     // parameterSetPointers
        paramSizes,    // parameterSetSizes
        4,             // NALUnitHeaderLength (AVCC uses 4-byte length prefix)
        &m->formatDesc
    );

    if (err != noErr) {
        SPDLOG_ERROR("CMVideoFormatDescriptionCreateFromH264ParameterSets failed: {}",
                     static_cast<int>(err));
        m->formatDesc = nullptr;
        return;
    }

    CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(m->formatDesc);
    SPDLOG_INFO("VideoDecoder: SPS/PPS set, {}x{}", dims.width, dims.height);

    m->createSession();
}

void VideoDecoder::decode(const uint8_t* nalData, size_t nalSize) {
    if (!m->session || !m->formatDesc || !nalData || nalSize == 0) return;

    // Input is Annex B (start-code delimited NAL units, one or more). Convert
    // the full access unit to AVCC (repeated 4-byte length + NAL body) so
    // multi-slice IDRs land in a single CMSampleBuffer.
    std::vector<uint8_t> avccData;
    avccData.reserve(nalSize + 8);

    auto emitNal = [&](const uint8_t* body, size_t bodySize) {
        if (!body || bodySize == 0) return;
        uint32_t nalLen = static_cast<uint32_t>(bodySize);
        avccData.push_back(static_cast<uint8_t>((nalLen >> 24) & 0xFF));
        avccData.push_back(static_cast<uint8_t>((nalLen >> 16) & 0xFF));
        avccData.push_back(static_cast<uint8_t>((nalLen >> 8) & 0xFF));
        avccData.push_back(static_cast<uint8_t>(nalLen & 0xFF));
        avccData.insert(avccData.end(), body, body + bodySize);
    };

    // Scan Annex-B start codes. Also accept a single raw NAL (no start code).
    size_t i = 0;
    auto isStart4 = [&](size_t p) {
        return p + 3 < nalSize && nalData[p] == 0 && nalData[p + 1] == 0 &&
               nalData[p + 2] == 0 && nalData[p + 3] == 1;
    };
    auto isStart3 = [&](size_t p) {
        return p + 2 < nalSize && nalData[p] == 0 && nalData[p + 1] == 0 &&
               nalData[p + 2] == 1;
    };
    if (!isStart4(0) && !isStart3(0)) {
        emitNal(nalData, nalSize);
    } else {
        while (i < nalSize) {
            size_t sc = 0;
            if (isStart4(i)) sc = 4;
            else if (isStart3(i)) sc = 3;
            else { ++i; continue; }
            size_t body = i + sc;
            size_t next = body;
            while (next < nalSize && !isStart4(next) && !isStart3(next)) ++next;
            emitNal(nalData + body, next - body);
            i = next;
        }
    }
    if (avccData.empty()) return;
    const size_t avccSize = avccData.size();

    // Create CMBlockBuffer from the AVCC data
    CMBlockBufferRef blockBuffer = nullptr;
    OSStatus err = CMBlockBufferCreateWithMemoryBlock(
        nullptr,                      // allocator
        nullptr,                      // memoryBlock (nullptr = allocate)
        avccSize,                     // blockLength
        kCFAllocatorDefault,          // blockAllocator
        nullptr,                      // customBlockSource
        0,                            // offsetToData
        avccSize,                     // dataLength
        0,                            // flags
        &blockBuffer
    );

    if (err != noErr || !blockBuffer) {
        SPDLOG_ERROR("CMBlockBufferCreateWithMemoryBlock failed: {}", static_cast<int>(err));
        return;
    }

    err = CMBlockBufferReplaceDataBytes(avccData.data(), blockBuffer, 0, avccSize);
    if (err != noErr) {
        SPDLOG_ERROR("CMBlockBufferReplaceDataBytes failed: {}", static_cast<int>(err));
        CFRelease(blockBuffer);
        return;
    }

    // Create CMSampleBuffer
    CMSampleBufferRef sampleBuffer = nullptr;
    const size_t sampleSizeArray[1] = {avccSize};

    err = CMSampleBufferCreateReady(
        nullptr,                  // allocator
        blockBuffer,              // dataBuffer
        m->formatDesc,            // formatDescription
        1,                        // numSamples
        0,                        // numSampleTimingEntries
        nullptr,                  // sampleTimingArray
        1,                        // numSampleSizeEntries
        sampleSizeArray,          // sampleSizeArray
        &sampleBuffer
    );

    CFRelease(blockBuffer);

    if (err != noErr || !sampleBuffer) {
        SPDLOG_ERROR("CMSampleBufferCreateReady failed: {}", static_cast<int>(err));
        return;
    }

    // Decode
    VTDecodeInfoFlags flagsOut = 0;
    err = VTDecompressionSessionDecodeFrame(
        m->session,
        sampleBuffer,
        0,              // decodeFlags (synchronous)
        nullptr,        // sourceFrameRefCon
        &flagsOut
    );

    CFRelease(sampleBuffer);

    if (err != noErr) {
        SPDLOG_ERROR("VTDecompressionSessionDecodeFrame failed: {}", static_cast<int>(err));
    }
}

void VideoDecoder::flush() {
    if (m->session) {
        VTDecompressionSessionWaitForAsynchronousFrames(m->session);
    }
}

} // namespace ge
