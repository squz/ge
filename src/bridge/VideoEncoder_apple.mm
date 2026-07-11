// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/VideoEncoder.h>
#include <ge/Protocol.h>

#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace ge {

// ── single-session VideoEncoder ─────────────────────────────────────────────

struct VideoEncoder::M {
    VTCompressionSessionRef session = nullptr;
    FrameCallback callback;
    Config cfg;
    int64_t frameCount = 0;
    std::vector<uint8_t> bgra;
    bool forceKey = false;

    // Sync path for TiledVideoEncoder pass-2: capture the latest encoded AU.
    std::mutex mu;
    std::condition_variable cv;
    bool encodeDone = false;
    std::vector<uint8_t> lastData;
    bool lastKey = false;

    void createSession();
    void destroySession();
    static void outputCallback(void* ctx, void* sourceFrameRefCon,
                                OSStatus status, VTEncodeInfoFlags flags,
                                CMSampleBufferRef sampleBuffer);
};

void VideoEncoder::M::destroySession() {
    if (session) {
        VTCompressionSessionInvalidate(session);
        CFRelease(session);
        session = nullptr;
    }
}

void VideoEncoder::M::createSession() {
    destroySession();
    NSDictionary* pixelBufferAttrs = @{
        (NSString*)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
        (NSString*)kCVPixelBufferWidthKey: @(cfg.width),
        (NSString*)kCVPixelBufferHeightKey: @(cfg.height),
    };

    OSStatus err = VTCompressionSessionCreate(
        nullptr, cfg.width, cfg.height,
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

    int32_t maxDelay = 0;
    CFNumberRef delayRef = CFNumberCreate(nullptr, kCFNumberSInt32Type, &maxDelay);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxFrameDelayCount, delayRef);
    CFRelease(delayRef);

    int32_t bitrate = cfg.averageBitRate;
    CFNumberRef bitrateRef = CFNumberCreate(nullptr, kCFNumberSInt32Type, &bitrate);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, bitrateRef);
    CFRelease(bitrateRef);

    int32_t maxKeyFrameInterval =
        cfg.maxKeyFrameInterval > 0 ? cfg.maxKeyFrameInterval : cfg.fps;
    CFNumberRef keyFrameRef =
        CFNumberCreate(nullptr, kCFNumberSInt32Type, &maxKeyFrameInterval);
    VTSessionSetProperty(session, kVTCompressionPropertyKey_MaxKeyFrameInterval,
                         keyFrameRef);
    CFRelease(keyFrameRef);

    VTCompressionSessionPrepareToEncodeFrames(session);
    SPDLOG_INFO("VideoEncoder: created {}x{} @ {} fps, {} bps H.264 High, key every {}",
                cfg.width, cfg.height, cfg.fps, cfg.averageBitRate, maxKeyFrameInterval);
}

void VideoEncoder::M::outputCallback(void* ctx, void* /*sourceFrameRefCon*/,
                                      OSStatus status, VTEncodeInfoFlags /*flags*/,
                                      CMSampleBufferRef sampleBuffer) {
    auto* self = static_cast<M*>(ctx);
    if (status != noErr || !sampleBuffer) {
        SPDLOG_ERROR("VideoEncoder output callback error: {}", static_cast<int>(status));
        std::lock_guard lock(self->mu);
        self->encodeDone = true;
        self->cv.notify_all();
        return;
    }

    CMBlockBufferRef blockBuffer = CMSampleBufferGetDataBuffer(sampleBuffer);
    if (!blockBuffer) {
        std::lock_guard lock(self->mu);
        self->encodeDone = true;
        self->cv.notify_all();
        return;
    }

    size_t totalLength = 0;
    char* dataPointer = nullptr;
    OSStatus blockErr = CMBlockBufferGetDataPointer(blockBuffer, 0, nullptr,
                                                     &totalLength, &dataPointer);
    if (blockErr != noErr || !dataPointer || totalLength == 0) {
        std::lock_guard lock(self->mu);
        self->encodeDone = true;
        self->cv.notify_all();
        return;
    }

    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    bool isKeyframe = false;
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef dict = (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
        CFBooleanRef notSync = (CFBooleanRef)CFDictionaryGetValue(
            dict, kCMSampleAttachmentKey_NotSync);
        isKeyframe = !notSync || !CFBooleanGetValue(notSync);
    }

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

    frameData.insert(frameData.end(),
                     reinterpret_cast<const uint8_t*>(dataPointer),
                     reinterpret_cast<const uint8_t*>(dataPointer) + totalLength);

    {
        std::lock_guard lock(self->mu);
        self->lastData = std::move(frameData);
        self->lastKey = isKeyframe;
        self->encodeDone = true;
    }
    self->cv.notify_all();

    // Callback outside the lock — callers may re-enter encoder or take other mutexes.
    if (self->callback) {
        Frame frame{self->lastData.data(), self->lastData.size(), self->lastKey};
        self->callback(frame);
    }
}

VideoEncoder::VideoEncoder(int width, int height, int fps, FrameCallback onFrame)
    : VideoEncoder(Config{.width = width, .height = height, .fps = fps},
                   std::move(onFrame)) {}

VideoEncoder::VideoEncoder(Config cfg, FrameCallback onFrame)
    : m(std::make_unique<M>()) {
    m->cfg = cfg;
    if (m->cfg.maxKeyFrameInterval <= 0) m->cfg.maxKeyFrameInterval = m->cfg.fps;
    m->callback = std::move(onFrame);
    m->createSession();
}

VideoEncoder::~VideoEncoder() {
    m->destroySession();
}

void VideoEncoder::forceNextKeyframe() { m->forceKey = true; }

void VideoEncoder::setAverageBitRate(int bps) {
    if (bps <= 0 || bps == m->cfg.averageBitRate) return;
    m->cfg.averageBitRate = bps;
    m->createSession();
}

int VideoEncoder::width() const { return m->cfg.width; }
int VideoEncoder::height() const { return m->cfg.height; }
int VideoEncoder::averageBitRate() const { return m->cfg.averageBitRate; }

void VideoEncoder::encode(const uint8_t* rgbaPixels, size_t bytesPerRow) {
    if (!m->session) return;

    const size_t rows = static_cast<size_t>(m->cfg.height);
    m->bgra.resize(bytesPerRow * rows);
    for (size_t y = 0; y < rows; ++y) {
        const uint8_t* s = rgbaPixels + y * bytesPerRow;
        uint8_t* d = m->bgra.data() + y * bytesPerRow;
        for (size_t x = 0; x + 4 <= bytesPerRow; x += 4) {
            d[x + 0] = s[x + 2];
            d[x + 1] = s[x + 1];
            d[x + 2] = s[x + 0];
            d[x + 3] = s[x + 3];
        }
    }

    CVPixelBufferRef pixelBuffer = nullptr;
    OSStatus err = CVPixelBufferCreateWithBytes(
        nullptr, m->cfg.width, m->cfg.height,
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
    encode(pixelBuffer);
    CVPixelBufferRelease(pixelBuffer);
}

void VideoEncoder::encode(CVPixelBufferRef pixelBuffer) {
    if (!m->session || !pixelBuffer) return;

    {
        std::lock_guard lock(m->mu);
        m->encodeDone = false;
        m->lastData.clear();
    }

    CFMutableDictionaryRef frameProps = nullptr;
    if (m->forceKey) {
        frameProps = CFDictionaryCreateMutable(nullptr, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(frameProps, kVTEncodeFrameOptionKey_ForceKeyFrame,
                             kCFBooleanTrue);
        m->forceKey = false;
    }

    CMTime pts = CMTimeMake(m->frameCount++, m->cfg.fps);
    OSStatus err = VTCompressionSessionEncodeFrame(
        m->session, pixelBuffer, pts,
        kCMTimeInvalid, frameProps, nullptr, nullptr
    );
    if (frameProps) CFRelease(frameProps);

    if (err != noErr) {
        SPDLOG_ERROR("VTCompressionSessionEncodeFrame failed: {}", static_cast<int>(err));
        return;
    }
    // Drain so the callback has run before the caller continues (needed for
    // MTU size checks on the tiled path).
    VTCompressionSessionCompleteFrames(m->session, kCMTimeInvalid);
}

void VideoEncoder::flush() {
    if (m->session) {
        VTCompressionSessionCompleteFrames(m->session, kCMTimeInvalid);
    }
}

void VideoEncoder::resize(int width, int height) {
    m->cfg.width = width;
    m->cfg.height = height;
    m->frameCount = 0;
    m->createSession();
}

// ── TiledVideoEncoder ───────────────────────────────────────────────────────

struct TiledVideoEncoder::M {
    Config cfg;
    TileCallback onTile;
    int tileEdge = 64;
    int cols = 0;
    int rows = 0;
    uint32_t frameSeq = 0;
    std::vector<std::unique_ptr<VideoEncoder>> encoders;
    std::vector<uint8_t> tileRgba;
    // Last encoded AU captured without going through VideoEncoder callback fan-out.
    std::vector<uint8_t> lastAu;
    bool lastKey = false;

    void rebuildGrid();
    bool encodeTileSync(int tileId, const uint8_t* rgba, size_t stride,
                        int tileW, int tileH, int bps, bool forceKey,
                        std::vector<uint8_t>& out, bool& outKey);
};

void TiledVideoEncoder::M::rebuildGrid() {
    encoders.clear();
    tileEdge = std::max(16, cfg.preferredTileEdge);
    // Grow tile edge until session count is practical.
    for (;;) {
        cols = (cfg.width + tileEdge - 1) / tileEdge;
        rows = (cfg.height + tileEdge - 1) / tileEdge;
        if (cols * rows <= cfg.maxTiles || tileEdge >= 512) break;
        tileEdge += 16;
    }
    cols = std::max(1, cols);
    rows = std::max(1, rows);
    // Wire uses u8 cols/rows — clamp.
    if (cols > 255) { tileEdge = (cfg.width + 254) / 255; cols = (cfg.width + tileEdge - 1) / tileEdge; }
    if (rows > 255) { tileEdge = std::max(tileEdge, (cfg.height + 254) / 255); rows = (cfg.height + tileEdge - 1) / tileEdge; }

    const int n = cols * rows;
    const int perTileBps = std::max(50'000, cfg.totalAverageBitRate / std::max(1, n));
    encoders.resize(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        const int col = i % cols;
        const int row = i / cols;
        const int x0 = col * tileEdge;
        const int y0 = row * tileEdge;
        const int tw = std::min(tileEdge, cfg.width - x0);
        const int th = std::min(tileEdge, cfg.height - y0);
        // VT wants even dimensions for yuv420.
        const int ew = tw & ~1;
        const int eh = th & ~1;
        if (ew < 2 || eh < 2) {
            encoders[static_cast<size_t>(i)].reset();
            continue;
        }
        VideoEncoder::Config ec;
        ec.width = ew;
        ec.height = eh;
        ec.fps = cfg.fps;
        ec.averageBitRate = perTileBps;
        ec.maxKeyFrameInterval = cfg.fps;
        // Capture into shared lastAu under lock via lambda — each encoder has its own.
        encoders[static_cast<size_t>(i)] = std::make_unique<VideoEncoder>(
            ec, [](VideoEncoder::Frame) {});
    }
    SPDLOG_INFO(
        "TiledVideoEncoder: {}x{} tile={} grid={}x{} ({} sessions) totalBps={} mtu={}",
        cfg.width, cfg.height, tileEdge, cols, rows, n, cfg.totalAverageBitRate,
        cfg.mtuBudget);
}

bool TiledVideoEncoder::M::encodeTileSync(int tileId, const uint8_t* rgba,
                                          size_t stride, int tileW, int tileH,
                                          int bps, bool forceKey,
                                          std::vector<uint8_t>& out, bool& outKey) {
    auto& enc = encoders[static_cast<size_t>(tileId)];
    if (!enc) return false;
    if (enc->averageBitRate() != bps) enc->setAverageBitRate(bps);
    if (forceKey) enc->forceNextKeyframe();

    // Capture result via temporary callback replacement is not available —
    // use CompleteFrames and read from a one-shot wrapper. Instead, re-create
    // a local encoder for pass-2 when needed. For the happy path, replace
    // encoder callback by encoding into a scratch VideoEncoder.

    // Simpler: each call builds a one-shot encode using the persistent session
    // by swapping in a capturing callback via a dedicated helper encoder.
    // Persistent encoders already complete frames synchronously; we need the
    // AU bytes. Re-bind by encoding with a fresh VideoEncoder when capturing:

    VideoEncoder::Config ec;
    ec.width = tileW & ~1;
    ec.height = tileH & ~1;
    if (ec.width < 2 || ec.height < 2) return false;
    ec.fps = cfg.fps;
    ec.averageBitRate = bps;
    ec.maxKeyFrameInterval = cfg.fps;

    std::vector<uint8_t> captured;
    bool capturedKey = false;
    VideoEncoder oneShot(ec, [&](VideoEncoder::Frame f) {
        captured.assign(f.data, f.data + f.size);
        capturedKey = f.isKeyframe;
    });
    if (forceKey) oneShot.forceNextKeyframe();
    oneShot.encode(rgba, stride);
    oneShot.flush();
    if (captured.empty()) return false;
    out = std::move(captured);
    outKey = capturedKey;
    (void)enc;  // persistent slots reserved for future warm sessions
    return true;
}

TiledVideoEncoder::TiledVideoEncoder(Config cfg, TileCallback onTile)
    : m(std::make_unique<M>()) {
    m->cfg = cfg;
    if (m->cfg.mtuBudget <= 0) m->cfg.mtuBudget = int(wire::kVideoTileMtuBudget);
    if (m->cfg.preferredTileEdge <= 0) m->cfg.preferredTileEdge = 64;
    if (m->cfg.maxTiles <= 0) m->cfg.maxTiles = 64;
    if (m->cfg.totalAverageBitRate <= 0) m->cfg.totalAverageBitRate = 6'000'000;
    m->onTile = std::move(onTile);
    m->rebuildGrid();
}

TiledVideoEncoder::~TiledVideoEncoder() = default;

int TiledVideoEncoder::tileEdge() const { return m->tileEdge; }
int TiledVideoEncoder::cols() const { return m->cols; }
int TiledVideoEncoder::rows() const { return m->rows; }

void TiledVideoEncoder::resize(int width, int height) {
    if (width == m->cfg.width && height == m->cfg.height) return;
    m->cfg.width = width;
    m->cfg.height = height;
    m->rebuildGrid();
}

void TiledVideoEncoder::flush() {
    for (auto& e : m->encoders)
        if (e) e->flush();
}

void TiledVideoEncoder::encode(const uint8_t* rgbaPixels, size_t bytesPerRow) {
    if (!rgbaPixels || m->cols <= 0 || m->rows <= 0) return;
    const uint32_t frameSeq = m->frameSeq++;
    const int n = m->cols * m->rows;
    const int baseBps = std::max(50'000, m->cfg.totalAverageBitRate / std::max(1, n));
    const int budget = m->cfg.mtuBudget;

    for (int row = 0; row < m->rows; ++row) {
        for (int col = 0; col < m->cols; ++col) {
            const int tileId = row * m->cols + col;
            const int x0 = col * m->tileEdge;
            const int y0 = row * m->tileEdge;
            int tw = std::min(m->tileEdge, m->cfg.width - x0);
            int th = std::min(m->tileEdge, m->cfg.height - y0);
            tw &= ~1;
            th &= ~1;
            if (tw < 2 || th < 2) continue;

            // Crop tile into contiguous RGBA buffer.
            m->tileRgba.resize(static_cast<size_t>(tw) * th * 4);
            for (int y = 0; y < th; ++y) {
                const uint8_t* src =
                    rgbaPixels + static_cast<size_t>(y0 + y) * bytesPerRow +
                    static_cast<size_t>(x0) * 4;
                uint8_t* dst = m->tileRgba.data() + static_cast<size_t>(y) * tw * 4;
                std::memcpy(dst, src, static_cast<size_t>(tw) * 4);
            }

            std::vector<uint8_t> au;
            bool isKey = false;
            int bps = baseBps;
            bool ok = m->encodeTileSync(tileId, m->tileRgba.data(),
                                        static_cast<size_t>(tw) * 4, tw, th, bps,
                                        /*forceKey=*/false, au, isKey);

            // Pass-2: ratchet bitrate down until under budget or floor.
            int attempts = 0;
            while (ok && au.size() > static_cast<size_t>(budget) && attempts < 4) {
                bps = std::max(20'000, bps / 2);
                ok = m->encodeTileSync(tileId, m->tileRgba.data(),
                                       static_cast<size_t>(tw) * 4, tw, th, bps,
                                       /*forceKey=*/true, au, isKey);
                ++attempts;
            }

            TileFrame tf;
            tf.frameSeq = frameSeq;
            tf.tileId = static_cast<uint16_t>(tileId);
            tf.cols = static_cast<uint8_t>(m->cols);
            tf.rows = static_cast<uint8_t>(m->rows);
            tf.frameW = static_cast<uint16_t>(m->cfg.width);
            tf.frameH = static_cast<uint16_t>(m->cfg.height);
            tf.tileEdge = static_cast<uint16_t>(m->tileEdge);
            tf.isKeyframe = isKey;

            if (!ok || au.size() > static_cast<size_t>(budget)) {
                tf.blank = true;
                tf.data = nullptr;
                tf.size = 0;
                if (m->onTile) m->onTile(tf);
                continue;
            }
            tf.blank = false;
            tf.data = au.data();
            tf.size = au.size();
            if (m->onTile) m->onTile(tf);
        }
    }
}

} // namespace ge
