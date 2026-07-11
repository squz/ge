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
#include <chrono>
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
    SPDLOG_DEBUG("VideoEncoder: created {}x{} @ {} fps, {} bps H.264 High, key every {}",
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
    // Prefer in-session update — recreating 60+ VT sessions per pass-2 is death.
    if (m->session) {
        int32_t bitrate = bps;
        CFNumberRef bitrateRef = CFNumberCreate(nullptr, kCFNumberSInt32Type, &bitrate);
        VTSessionSetProperty(m->session, kVTCompressionPropertyKey_AverageBitRate,
                             bitrateRef);
        CFRelease(bitrateRef);
    } else {
        m->createSession();
    }
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
        std::lock_guard lock(m->mu);
        m->encodeDone = true;
        return;
    }
    // Drain so the callback has run before the caller continues (needed for
    // MTU size checks on the tiled path).
    VTCompressionSessionCompleteFrames(m->session, kCMTimeInvalid);
}

bool VideoEncoder::encodeSync(const uint8_t* rgbaPixels, size_t bytesPerRow,
                              std::vector<uint8_t>& out, bool& isKey) {
    out.clear();
    isKey = false;
    if (!m->session || !rgbaPixels) return false;
    encode(rgbaPixels, bytesPerRow);
    std::unique_lock lock(m->mu);
    // CompleteFrames is synchronous for the VT callback on this thread in
    // practice; still wait briefly in case the callback is deferred.
    m->cv.wait_for(lock, std::chrono::milliseconds(50),
                   [&] { return m->encodeDone; });
    if (m->lastData.empty()) return false;
    out = m->lastData;
    isKey = m->lastKey;
    return true;
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
    int baseBps = 50'000;
    std::vector<std::unique_ptr<VideoEncoder>> encoders;
    std::vector<uint8_t> sawKey;  // first IDR delivered for tile
    std::vector<uint8_t> tileRgba;

    void rebuildGrid();
};

void TiledVideoEncoder::M::rebuildGrid() {
    encoders.clear();
    // Prefer a tile edge that *divides both* width and height so every tile
    // is the same size. Uneven edge tiles (e.g. 128×96) produce flaky VT
    // encode/decode and leave permanent black regions on the player mosaic.
    tileEdge = 0;
    const int prefer = std::max(16, cfg.preferredTileEdge);
    // Search descending from a generous cap so we keep tiles large when possible.
    for (int e = 512; e >= 16; e -= 16) {
        if (cfg.width % e != 0 || cfg.height % e != 0) continue;
        const int c = cfg.width / e;
        const int r = cfg.height / e;
        if (c < 1 || r < 1 || c > 255 || r > 255) continue;
        if (c * r > cfg.maxTiles) continue;
        // Prefer edges near the configured preference; among equals, larger.
        if (tileEdge == 0 ||
            std::abs(e - prefer) < std::abs(tileEdge - prefer) ||
            (std::abs(e - prefer) == std::abs(tileEdge - prefer) && e > tileEdge)) {
            tileEdge = e;
        }
    }
    if (tileEdge == 0) {
        // No exact divisor under maxTiles — grow from preferred until session
        // count fits (edge tiles will be smaller; padded encode below).
        tileEdge = prefer;
        for (;;) {
            cols = (cfg.width + tileEdge - 1) / tileEdge;
            rows = (cfg.height + tileEdge - 1) / tileEdge;
            if (cols * rows <= cfg.maxTiles || tileEdge >= 512) break;
            tileEdge += 16;
        }
    }
    cols = std::max(1, (cfg.width + tileEdge - 1) / tileEdge);
    rows = std::max(1, (cfg.height + tileEdge - 1) / tileEdge);
    if (cols > 255) {
        tileEdge = (cfg.width + 254) / 255;
        cols = (cfg.width + tileEdge - 1) / tileEdge;
    }
    if (rows > 255) {
        tileEdge = std::max(tileEdge, (cfg.height + 254) / 255);
        rows = (cfg.height + tileEdge - 1) / tileEdge;
    }

    const int n = cols * rows;
    // Equal share per tile — scene complexity is often uniform; quality
    // banding by row was from encode order (always top→bottom under VT load)
    // and all-intra, not from content-weighted rate.
    baseBps = std::max(80'000, cfg.totalAverageBitRate / std::max(1, n));
    encoders.resize(static_cast<size_t>(n));
    sawKey.assign(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        // Every session is full tileEdge×tileEdge. Content that doesn't fill
        // the cell is black-padded so SPS/AU dimensions stay uniform.
        const int ew = tileEdge & ~1;
        const int eh = tileEdge & ~1;
        if (ew < 2 || eh < 2) {
            encoders[static_cast<size_t>(i)].reset();
            continue;
        }
        VideoEncoder::Config ec;
        ec.width = ew;
        ec.height = eh;
        ec.fps = cfg.fps;
        ec.averageBitRate = baseBps;
        // Inter frames (GOP ≈ 1s). Stagger forced IDRs by tile id so keys are
        // not synchronized across the grid (avoids a global bitrate spike).
        ec.maxKeyFrameInterval = std::max(1, cfg.fps);
        encoders[static_cast<size_t>(i)] =
            std::make_unique<VideoEncoder>(ec, VideoEncoder::FrameCallback{});
    }
    SPDLOG_INFO(
        "TiledVideoEncoder: {}x{} tile={} grid={}x{} ({} sessions) "
        "perTileBps={} totalBps={} mtu={} gop={}",
        cfg.width, cfg.height, tileEdge, cols, rows, n, baseBps,
        cfg.totalAverageBitRate, cfg.mtuBudget, std::max(1, cfg.fps));
}

TiledVideoEncoder::TiledVideoEncoder(Config cfg, TileCallback onTile)
    : m(std::make_unique<M>()) {
    m->cfg = cfg;
    if (m->cfg.mtuBudget <= 0) m->cfg.mtuBudget = int(wire::kVideoTileMtuBudget);
    if (m->cfg.preferredTileEdge <= 0) m->cfg.preferredTileEdge = 64;
    if (m->cfg.maxTiles <= 0) m->cfg.maxTiles = 64;
    // Slightly above the all-intra experiment default — P-frames make this
    // land as quality, not as fat keys.
    if (m->cfg.totalAverageBitRate <= 0) m->cfg.totalAverageBitRate = 8'000'000;
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
    const int budget = m->cfg.mtuBudget;
    const int gop = std::max(1, m->cfg.fps);
    const int n = m->cols * m->rows;
    const int cell = m->tileEdge & ~1;
    if (cell < 2) return;

    // Round-robin starting tile so VT load does not always hit the same rows
    // last (top→bottom order made the bottom third systematically noisier
    // under sequential multi-session encode, even on uniform content).
    const int start = int(frameSeq % uint32_t(std::max(1, n)));

    for (int k = 0; k < n; ++k) {
        const int tileId = (start + k) % n;
        auto& enc = m->encoders[static_cast<size_t>(tileId)];
        if (!enc) continue;

        const int col = tileId % m->cols;
        const int row = tileId / m->cols;
        const int x0 = col * m->tileEdge;
        const int y0 = row * m->tileEdge;
        const int contentW = std::min(cell, m->cfg.width - x0);
        const int contentH = std::min(cell, m->cfg.height - y0);
        if (contentW < 1 || contentH < 1) continue;

        m->tileRgba.assign(static_cast<size_t>(cell) * cell * 4, 0);
        for (int y = 0; y < contentH; ++y) {
            const uint8_t* src =
                rgbaPixels + static_cast<size_t>(y0 + y) * bytesPerRow +
                static_cast<size_t>(x0) * 4;
            uint8_t* dst = m->tileRgba.data() + static_cast<size_t>(y) * cell * 4;
            std::memcpy(dst, src, static_cast<size_t>(contentW) * 4);
        }

        std::vector<uint8_t> au;
        bool isKey = false;
        if (enc->averageBitRate() != m->baseBps)
            enc->setAverageBitRate(m->baseBps);

        // First AU for a tile, or staggered GOP tick → force IDR.
        const bool wantKey =
            !m->sawKey[static_cast<size_t>(tileId)] ||
            ((frameSeq + uint32_t(tileId)) % uint32_t(gop)) == 0;
        if (wantKey) enc->forceNextKeyframe();

        bool ok = enc->encodeSync(m->tileRgba.data(),
                                  static_cast<size_t>(cell) * 4, au, isKey);

        // Pass-2: ratchet bitrate on the same session until under MTU.
        int bps = m->baseBps;
        int attempts = 0;
        while (ok && au.size() > static_cast<size_t>(budget) && attempts < 4) {
            bps = std::max(40'000, bps / 2);
            enc->setAverageBitRate(bps);
            enc->forceNextKeyframe();
            ok = enc->encodeSync(m->tileRgba.data(),
                                 static_cast<size_t>(cell) * 4, au, isKey);
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
        if (isKey) m->sawKey[static_cast<size_t>(tileId)] = 1;
        tf.blank = false;
        tf.data = au.data();
        tf.size = au.size();
        if (m->onTile) m->onTile(tf);
    }
}

} // namespace ge
