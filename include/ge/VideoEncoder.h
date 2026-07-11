// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <ge/Linalg.h>

#include <cstdint>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

// Forward declare CoreVideo type to avoid importing ObjC headers
typedef struct __CVBuffer* CVPixelBufferRef;

namespace ge {

// H.264 video encoder using VideoToolbox (macOS/iOS).
// Encodes RGBA frames to H.264 (RGBA is ge's frame-capture contract — see
// SokolContext's readback normalization; the encode(pixels) path swaps R↔B to
// VideoToolbox's BGRA internally). Output is complete encoded frames (not
// individual NAL units) — the callback receives the full CMSampleBuffer data.
class VideoEncoder {
public:
    struct Config {
        int width = 0;
        int height = 0;
        int fps = 60;
        // AverageBitRate for the session. Default matches the pre-T151
        // "crisp LAN" path; tiled mode uses a lower total split across tiles.
        int averageBitRate = 16'000'000;
        // Max keyframe interval in frames (default 1s at fps).
        int maxKeyFrameInterval = 0;  // 0 → fps
    };

    struct Frame {
        const uint8_t* data;
        size_t size;
        bool isKeyframe;
    };
    using FrameCallback = std::function<void(Frame)>;

    VideoEncoder(int width, int height, int fps, FrameCallback onFrame);
    VideoEncoder(Config cfg, FrameCallback onFrame);
    ~VideoEncoder();

    // Encode from raw CPU pixels (copies data into CVPixelBuffer).
    // Input is RGBA (ge capture contract).
    void encode(const uint8_t* rgbaPixels, size_t bytesPerRow);

    // Encode from CVPixelBuffer directly — zero-copy when backed by IOSurface.
    void encode(CVPixelBufferRef pixelBuffer);

    // Synchronous encode for the tiled path: drains VT, copies the AU into
    // `out`, sets `isKey`. Returns false if encode produced no data.
    bool encodeSync(const uint8_t* rgbaPixels, size_t bytesPerRow,
                    std::vector<uint8_t>& out, bool& isKey);

    // Force the next encode to be a keyframe (e.g. pass-2 retry).
    void forceNextKeyframe();

    // Recreate the session at a new average bitrate (pass-2 / quality ladder).
    void setAverageBitRate(int bps);

    void flush();
    void resize(int width, int height);

    int width() const;
    int height() const;
    int averageBitRate() const;

private:
    struct M;
    std::unique_ptr<M> m;
};

// 🎯T151: independent-tile H.264 encoder with a hard per-tile AU budget.
// Crops the framebuffer into a grid, encodes each cell with its own VT session,
// re-encodes oversize tiles harder, blanks if still over budget.
class TiledVideoEncoder {
public:
    struct Config {
        int width = 0;
        int height = 0;
        int fps = 60;
        int preferredTileEdge = 64;   // CRF-36 lab default
        int mtuBudget = 1000;         // wire::kVideoTileMtuBudget
        int totalAverageBitRate = 6'000'000;  // softer than legacy 16 Mbps
        int maxTiles = 64;            // VT session cap — grow tile edge if needed
    };

    struct TileFrame {
        uint32_t frameSeq = 0;
        uint16_t tileId = 0;
        uint8_t cols = 0;
        uint8_t rows = 0;
        uint16_t frameW = 0;
        uint16_t frameH = 0;
        uint16_t tileEdge = 0;
        bool isKeyframe = false;
        bool blank = false;
        const uint8_t* data = nullptr;
        size_t size = 0;
    };
    using TileCallback = std::function<void(TileFrame)>;

    TiledVideoEncoder(Config cfg, TileCallback onTile);
    ~TiledVideoEncoder();

    void encode(const uint8_t* rgbaPixels, size_t bytesPerRow);
    void flush();
    void resize(int width, int height);

    int tileEdge() const;
    int cols() const;
    int rows() const;

private:
    struct M;
    std::unique_ptr<M> m;
};

} // namespace ge
