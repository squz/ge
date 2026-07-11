// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/PlayerWireBridge.h>
#include <ge/Protocol.h>
#include <ge/VideoDecoder.h>
#include <ge/WebSocketClient.h>

#include "wire_input.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ge {

namespace {

// Parse AVCC-format NAL units from encoded frame data.
// AVCC format: [4-byte big-endian length][NAL body] repeated.
// For keyframes the encoder prepends SPS and PPS NAL units.
// NAL type is (first byte of NAL body) & 0x1F:
//   7 = SPS, 8 = PPS, 5 = IDR (keyframe), 1 = non-IDR (P-frame)
struct AVCCParser {
    std::vector<uint8_t> sps, pps;
    bool paramsDirty = false;

    std::vector<std::pair<const uint8_t*, size_t>>
    parse(const uint8_t* data, size_t size) {
        std::vector<std::pair<const uint8_t*, size_t>> frameNals;
        size_t offset = 0;
        while (offset + 4 <= size) {
            uint32_t nalLen = (uint32_t(data[offset]) << 24)
                            | (uint32_t(data[offset+1]) << 16)
                            | (uint32_t(data[offset+2]) << 8)
                            | uint32_t(data[offset+3]);
            offset += 4;
            if (nalLen == 0 || offset + nalLen > size) break;

            const uint8_t* nalBody = data + offset;
            uint8_t nalType = nalBody[0] & 0x1F;

            if (nalType == 7) {
                if (sps.size() != nalLen || std::memcmp(sps.data(), nalBody, nalLen) != 0) {
                    sps.assign(nalBody, nalBody + nalLen);
                    paramsDirty = true;
                }
            } else if (nalType == 8) {
                if (pps.size() != nalLen || std::memcmp(pps.data(), nalBody, nalLen) != 0) {
                    pps.assign(nalBody, nalBody + nalLen);
                    paramsDirty = true;
                }
            } else {
                frameNals.emplace_back(nalBody, nalLen);
            }
            offset += nalLen;
        }
        return frameNals;
    }

    bool hasParams() const { return !sps.empty() && !pps.empty(); }
};

struct TileSlot {
    std::unique_ptr<VideoDecoder> decoder;
    AVCCParser avcc;
    // Last decoded BGRA for this tile (owning).
    std::vector<uint8_t> bgra;
    int w = 0, h = 0, stride = 0;
};

} // namespace

struct PlayerWireBridge::Impl {
    Config cfg;
    std::shared_ptr<WsConnection> conn;
    std::unique_ptr<VideoDecoder> decoder;  // legacy full-frame
    AVCCParser avcc;
    std::string sessionId;

    // 🎯T151 tiled mosaic
    std::unordered_map<uint16_t, TileSlot> tiles;
    uint8_t cols = 0, rows = 0;
    uint16_t frameW = 0, frameH = 0;
    uint16_t tileEdge = 0;
    std::vector<uint8_t> mosaic;  // BGRA frameW×frameH
    // Protects mosaic + tile slot pixel blits (VT decode threads + pump).
    std::mutex mosaicMu;

    std::mutex frameMutex;
    DecodedFrame pending;
    bool pendingReady = false;

    PumpStats stats;

    void publishFullFrame(const VideoFrame& f);
    void publishMosaic();
    void handleLegacyVideo(const detail::VideoStreamPayload& pl);
    void handleTiledVideo(const detail::VideoStreamPayload& pl);
};

void PlayerWireBridge::Impl::publishFullFrame(const VideoFrame& f) {
    std::lock_guard<std::mutex> lock(frameMutex);
    pending.format = f.format;
    pending.width = f.width;
    pending.height = f.height;
    pending.stride0 = f.strides[0];
    pending.stride1 = f.strides[1];
    pending.stride2 = f.strides[2];

    auto copyPlane = [](std::vector<uint8_t>& dst,
                        const uint8_t* src, int stride, int rows) {
        if (!src || stride <= 0 || rows <= 0) {
            dst.clear();
            return;
        }
        size_t bytes = static_cast<size_t>(stride) * rows;
        dst.assign(src, src + bytes);
    };

    switch (f.format) {
    case VideoFrame::Format::BGRA:
        copyPlane(pending.plane0, f.planes[0], f.strides[0], f.height);
        pending.plane1.clear();
        pending.plane2.clear();
        break;
    case VideoFrame::Format::NV12:
        copyPlane(pending.plane0, f.planes[0], f.strides[0], f.height);
        copyPlane(pending.plane1, f.planes[1], f.strides[1], f.height / 2);
        pending.plane2.clear();
        break;
    case VideoFrame::Format::IYUV:
        copyPlane(pending.plane0, f.planes[0], f.strides[0], f.height);
        copyPlane(pending.plane1, f.planes[1], f.strides[1], f.height / 2);
        copyPlane(pending.plane2, f.planes[2], f.strides[2], f.height / 2);
        break;
    }
    pendingReady = true;
}

void PlayerWireBridge::Impl::publishMosaic() {
    // Caller must hold mosaicMu for a coherent snapshot, or we take both locks.
    if (frameW == 0 || frameH == 0) return;
    std::lock_guard<std::mutex> mlock(mosaicMu);
    if (mosaic.empty()) return;
    std::lock_guard<std::mutex> lock(frameMutex);
    pending.format = VideoFrame::Format::BGRA;
    pending.width = frameW;
    pending.height = frameH;
    pending.stride0 = int(frameW) * 4;
    pending.stride1 = 0;
    pending.stride2 = 0;
    pending.plane0 = mosaic;
    pending.plane1.clear();
    pending.plane2.clear();
    pendingReady = true;
}

void PlayerWireBridge::Impl::handleLegacyVideo(const detail::VideoStreamPayload& pl) {
    if (!decoder) {
        decoder = std::make_unique<VideoDecoder>(
            [this](const VideoFrame& f) { publishFullFrame(f); });
    }
    auto frameNals = avcc.parse(pl.avccData, pl.avccSize);
    if (decoder && avcc.paramsDirty && avcc.hasParams()) {
        decoder->setParameterSets(
            avcc.sps.data(), avcc.sps.size(),
            avcc.pps.data(), avcc.pps.size());
        avcc.paramsDirty = false;
        SPDLOG_INFO("PlayerWireBridge: decoder initialized with SPS/PPS");
    }
    if (!decoder) return;
    static const uint8_t startCode[] = {0x00, 0x00, 0x00, 0x01};
    std::vector<uint8_t> annexB;
    annexB.reserve(pl.avccSize + 16);
    for (auto& [nalBody, nalSize] : frameNals) {
        uint8_t nalType = nalBody[0] & 0x1F;
        if (nalType != 1 && nalType != 5) continue;
        annexB.insert(annexB.end(), startCode, startCode + 4);
        annexB.insert(annexB.end(), nalBody, nalBody + nalSize);
    }
    if (!annexB.empty()) decoder->decode(annexB.data(), annexB.size());
}

void PlayerWireBridge::Impl::handleTiledVideo(const detail::VideoStreamPayload& pl) {
    if (pl.blank) return;  // leave prior pixels

    if (pl.cols != cols || pl.rows != rows || pl.frameW != frameW ||
        pl.frameH != frameH || pl.tileEdge != tileEdge) {
        std::lock_guard<std::mutex> mlock(mosaicMu);
        cols = pl.cols;
        rows = pl.rows;
        frameW = pl.frameW;
        frameH = pl.frameH;
        tileEdge = pl.tileEdge;
        tiles.clear();
        mosaic.assign(static_cast<size_t>(frameW) * frameH * 4, 0);
        SPDLOG_INFO("PlayerWireBridge: tiled mosaic {}x{} grid {}x{} edge={}",
                    frameW, frameH, int(cols), int(rows), int(tileEdge));
    }

    TileSlot& slot = tiles[pl.tileId];
    if (!slot.decoder) {
        const uint16_t tid = pl.tileId;
        slot.decoder = std::make_unique<VideoDecoder>(
            [this, tid](const VideoFrame& f) {
                if (f.format != VideoFrame::Format::BGRA || !f.planes[0]) return;
                std::lock_guard<std::mutex> mlock(mosaicMu);
                auto it = tiles.find(tid);
                if (it == tiles.end()) return;
                if (frameW == 0 || tileEdge == 0 || cols == 0) return;

                // Blit only the content rect (cell may be padded on the encode side).
                const int col = int(tid) % int(cols);
                const int row = int(tid) / int(cols);
                const int x0 = col * int(tileEdge);
                const int y0 = row * int(tileEdge);
                const int copyW = std::min({f.width, int(tileEdge), int(frameW) - x0});
                const int copyH = std::min({f.height, int(tileEdge), int(frameH) - y0});
                if (copyW <= 0 || copyH <= 0) return;
                // Dense copy: use min(stride, width*4) source pitch.
                const int srcStride = f.strides[0];
                for (int y = 0; y < copyH; ++y) {
                    const uint8_t* src = f.planes[0] + size_t(y) * srcStride;
                    uint8_t* dst = mosaic.data() +
                                   (size_t(y0 + y) * frameW + size_t(x0)) * 4;
                    std::memcpy(dst, src, size_t(copyW) * 4);
                }
                // Publish under mosaicMu; publishMosaic re-takes mosaicMu — avoid
                // deadlock by inlining a frameMutex publish here.
                std::lock_guard<std::mutex> lock(frameMutex);
                pending.format = VideoFrame::Format::BGRA;
                pending.width = frameW;
                pending.height = frameH;
                pending.stride0 = int(frameW) * 4;
                pending.stride1 = 0;
                pending.stride2 = 0;
                pending.plane0 = mosaic;
                pending.plane1.clear();
                pending.plane2.clear();
                pendingReady = true;
            });
    }

    auto frameNals = slot.avcc.parse(pl.avccData, pl.avccSize);
    if (slot.avcc.paramsDirty && slot.avcc.hasParams()) {
        slot.decoder->setParameterSets(
            slot.avcc.sps.data(), slot.avcc.sps.size(),
            slot.avcc.pps.data(), slot.avcc.pps.size());
        slot.avcc.paramsDirty = false;
    }
    // Feed VCL NALs as a single Annex-B access unit (SPS/PPS already in the
    // format description). Splitting into one DecodeFrame per NAL left VT
    // returning -12909 on nearly every tile under the multi-session path.
    static const uint8_t startCode[] = {0x00, 0x00, 0x00, 0x01};
    std::vector<uint8_t> annexB;
    annexB.reserve(pl.avccSize + 16);
    for (auto& [nalBody, nalSize] : frameNals) {
        uint8_t nalType = nalBody[0] & 0x1F;
        if (nalType != 1 && nalType != 5) continue;
        annexB.insert(annexB.end(), startCode, startCode + 4);
        annexB.insert(annexB.end(), nalBody, nalBody + nalSize);
    }
    if (!annexB.empty() && slot.decoder)
        slot.decoder->decode(annexB.data(), annexB.size());
}

PlayerWireBridge::PlayerWireBridge(Config config)
    : i_(std::make_unique<Impl>()) {
    i_->cfg = std::move(config);
}

PlayerWireBridge::~PlayerWireBridge() = default;

bool PlayerWireBridge::connect(wire::SessionConfig& outConfig) {
    const std::string path = "/ws/wire?preference=" + i_->cfg.serverName
                           + "&name=" + i_->cfg.serverName;
    i_->conn = ge::connectWebSocket(i_->cfg.host, i_->cfg.port, path,
                                    i_->cfg.connectTimeoutMs);
    if (!i_->conn || !i_->conn->isOpen()) {
        SPDLOG_ERROR("PlayerWireBridge: failed to connect to stream relay");
        return false;
    }
    SPDLOG_INFO("PlayerWireBridge: connected to stream relay {}:{} name={}",
                i_->cfg.host, i_->cfg.port, i_->cfg.serverName);

    bool gotConfig = false;
    while (i_->conn->isOpen() && !gotConfig) {
        std::vector<char> msg;
        if (!i_->conn->recvBinary(msg) || msg.size() < 8) return false;
        uint32_t magic = 0;
        std::memcpy(&magic, msg.data(), 4);
        if (magic == wire::kSessionConfigMagic &&
            msg.size() >= sizeof(wire::MessageHeader) + sizeof(wire::SessionConfig)) {
            std::memcpy(&outConfig,
                        msg.data() + sizeof(wire::MessageHeader),
                        sizeof(wire::SessionConfig));
            gotConfig = true;
        } else if (magic == wire::kStreamSessionIdMagic &&
                   msg.size() >= sizeof(wire::MessageHeader)) {
            const auto* hdr =
                reinterpret_cast<const wire::MessageHeader*>(msg.data());
            if (sizeof(wire::MessageHeader) + hdr->length <= msg.size()) {
                i_->sessionId.assign(msg.data() + sizeof(wire::MessageHeader),
                                     hdr->length);
                SPDLOG_INFO("PlayerWireBridge: stream session_id={}", i_->sessionId);
            }
        }
    }
    if (gotConfig && i_->sessionId.empty() && i_->conn->isOpen() &&
        i_->conn->available() > 0) {
        std::vector<char> msg;
        if (i_->conn->recvBinary(msg) && msg.size() >= 8) {
            uint32_t magic = 0;
            std::memcpy(&magic, msg.data(), 4);
            if (magic == wire::kStreamSessionIdMagic) {
                const auto* hdr =
                    reinterpret_cast<const wire::MessageHeader*>(msg.data());
                if (sizeof(wire::MessageHeader) + hdr->length <= msg.size()) {
                    i_->sessionId.assign(msg.data() + sizeof(wire::MessageHeader),
                                         hdr->length);
                    SPDLOG_INFO("PlayerWireBridge: stream session_id={}",
                                i_->sessionId);
                }
            }
        }
    }
    return gotConfig;
}

bool PlayerWireBridge::sendDeviceInfo(const wire::DeviceInfo& devInfo) {
    if (!i_->conn || !i_->conn->isOpen()) return false;
    wire::MessageHeader hdr{};
    hdr.magic = wire::kDeviceInfoMagic;
    hdr.length = sizeof(wire::DeviceInfo);
    std::vector<uint8_t> msg(sizeof(hdr) + sizeof(devInfo));
    std::memcpy(msg.data(), &hdr, sizeof(hdr));
    std::memcpy(msg.data() + sizeof(hdr), &devInfo, sizeof(devInfo));
    i_->conn->sendBinary(msg.data(), msg.size());
    // Decoders are created lazily on first video (legacy or per-tile).
    return true;
}

void PlayerWireBridge::sendEvent(const SDL_Event& e) {
    if (!i_->conn || !i_->conn->isOpen()) return;
    wire::MessageHeader hdr{};
    hdr.magic = wire::kSdlEventMagic;
    hdr.length = sizeof(SDL_Event);
    std::vector<uint8_t> msg(sizeof(hdr) + sizeof(SDL_Event));
    std::memcpy(msg.data(), &hdr, sizeof(hdr));
    std::memcpy(msg.data() + sizeof(hdr), &e, sizeof(SDL_Event));
    i_->conn->sendBinary(msg.data(), msg.size());
}

bool PlayerWireBridge::pump() {
    if (!i_->conn) return false;
    i_->stats = {};
    while (i_->conn->isOpen() && i_->conn->available() > 0) {
        std::vector<char> data;
        if (!i_->conn->recvBinary(data) || data.size() < 8) break;

        uint32_t magic = 0;
        std::memcpy(&magic, data.data(), 4);
        if (magic == wire::kVideoStreamMagic) {
            detail::VideoStreamPayload pl;
            if (!detail::decodeVideoStreamMessage(data, pl))
                continue;
            if (pl.tiled)
                i_->handleTiledVideo(pl);
            else
                i_->handleLegacyVideo(pl);
            i_->stats.framesThisTick++;
            i_->stats.lastSeq = pl.seq;
        } else if (magic == wire::kStreamSessionIdMagic) {
            const auto* hdr =
                reinterpret_cast<const wire::MessageHeader*>(data.data());
            if (sizeof(wire::MessageHeader) + hdr->length <= data.size()) {
                i_->sessionId.assign(data.data() + sizeof(wire::MessageHeader),
                                     hdr->length);
                SPDLOG_INFO("PlayerWireBridge: stream session_id={}", i_->sessionId);
            }
        } else if (magic == wire::kServerAssignedMagic) {
            SPDLOG_INFO("PlayerWireBridge: server assigned");
        } else if (magic == wire::kSessionEndMagic) {
            SPDLOG_INFO("PlayerWireBridge: session ended");
        }
    }
    return i_->conn->isOpen();
}

bool PlayerWireBridge::pollFrame(DecodedFrame& out) {
    std::lock_guard<std::mutex> lock(i_->frameMutex);
    if (!i_->pendingReady) return false;
    std::swap(out, i_->pending);
    i_->pendingReady = false;
    return true;
}

PlayerWireBridge::PumpStats PlayerWireBridge::lastPumpStats() const {
    return i_->stats;
}

const std::string& PlayerWireBridge::sessionId() const {
    return i_->sessionId;
}

bool PlayerWireBridge::isOpen() const {
    return i_->conn && i_->conn->isOpen();
}

void PlayerWireBridge::close() {
    if (i_->decoder) i_->decoder->flush();
    for (auto& [_, slot] : i_->tiles)
        if (slot.decoder) slot.decoder->flush();
    if (i_->conn) i_->conn->close();
}

} // namespace ge
