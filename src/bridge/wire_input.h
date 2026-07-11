// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Input-validation guards for the brokered bridge's two untrusted-length wire
// paths. Header-only + pure so they are unit-testable without a live socket or
// decoder (src/wire_input_test.cpp). Both fixes trace to the Fable-5 audit
// (docs/audit/fable-2026-07.md, findings F5 / F6):
//
//   * decodeVideoStreamMessage — 🎯T142 (PlayerWireBridge::pump). Validates a
//     kVideoStreamMagic message's length field before it is used to compute
//     pointers/sizes, closing the unsigned underflow of (length - 5), the 32-bit
//     overflow of (8 + length), and the out-of-bounds seq / AVCC heap reads.
//
//   * wsPayloadWithinCap — 🎯T143 (WebSocketClient::recvFrame). Bounds a
//     wire-supplied 64-bit payload length by wire::kMaxMessageSize before any
//     allocation, covering both a single oversized frame and unbounded
//     fragmented-continuation accumulation.
#pragma once

#include <ge/Protocol.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ge {
namespace detail {

// Parsed GE2V body (after MessageHeader).
struct VideoStreamPayload {
    uint8_t flags = 0;
    uint32_t seq = 0;  // frame_seq (tiled) or monotonic seq (legacy)
    bool tiled = false;
    bool blank = false;
    uint16_t tileId = 0;
    uint8_t cols = 0;
    uint8_t rows = 0;
    uint16_t frameW = 0;
    uint16_t frameH = 0;
    uint16_t tileEdge = 0;
    const uint8_t* avccData = nullptr;
    size_t avccSize = 0;
};

// `data` is the full wire message: [u32 magic][u32 length][payload…].
// Legacy: payload = [u8 flags][u32 seq][avcc…]  (length >= 5)
// Tiled:  payload = [u8 flags][u32 seq][u16 tile][u8 cols][u8 rows]
//                   [u16 fw][u16 fh][u16 tile_edge][avcc?]
//                   (length >= 15; avcc absent if blank)
inline bool decodeVideoStreamMessage(const std::vector<char>& data,
                                     VideoStreamPayload& out) {
    if (data.size() < 8) return false;
    uint32_t length = 0;
    std::memcpy(&length, data.data() + 4, 4);
    if (length < 5) return false;
    if (static_cast<uint64_t>(8) + length > data.size()) return false;

    const auto* p = reinterpret_cast<const uint8_t*>(data.data()) + 8;
    out = {};
    out.flags = p[0];
    std::memcpy(&out.seq, p + 1, 4);
    out.tiled = (out.flags & wire::kVideoFlagTiled) != 0;
    out.blank = (out.flags & wire::kVideoFlagBlank) != 0;

    if (!out.tiled) {
        out.avccData = p + 5;
        out.avccSize = static_cast<size_t>(length) - 5;
        return true;
    }

    // Tiled header: 1+4+2+1+1+2+2+2 = 15 bytes
    if (length < 15) return false;
    std::memcpy(&out.tileId, p + 5, 2);
    out.cols = p[7];
    out.rows = p[8];
    std::memcpy(&out.frameW, p + 9, 2);
    std::memcpy(&out.frameH, p + 11, 2);
    std::memcpy(&out.tileEdge, p + 13, 2);
    if (out.cols == 0 || out.rows == 0 || out.tileEdge == 0) return false;
    if (out.tileId >= uint16_t(out.cols) * uint16_t(out.rows)) return false;

    out.avccData = p + 15;
    out.avccSize = static_cast<size_t>(length) - 15;
    if (out.blank) {
        out.avccData = nullptr;
        out.avccSize = 0;
    }
    return true;
}

// Back-compat for tests / callers that only need seq + avcc (legacy layout).
inline bool decodeVideoStreamMessage(const std::vector<char>& data,
                                     uint32_t& seq,
                                     const uint8_t*& avccData,
                                     size_t& avccSize) {
    VideoStreamPayload pl;
    if (!decodeVideoStreamMessage(data, pl)) return false;
    if (pl.tiled) return false;  // legacy unpacker rejects tiled
    seq = pl.seq;
    avccData = pl.avccData;
    avccSize = pl.avccSize;
    return true;
}

// Returns true iff appending a WebSocket frame of `payloadLen` bytes to a
// message already holding `accumulated` bytes keeps the running total within
// wire::kMaxMessageSize. Overflow-safe: the single-frame cap is checked first,
// so the accumulation comparison can never wrap.
inline bool wsPayloadWithinCap(uint64_t payloadLen, size_t accumulated) {
    if (payloadLen > wire::kMaxMessageSize) return false;
    if (accumulated > wire::kMaxMessageSize - static_cast<size_t>(payloadLen))
        return false;
    return true;
}

}  // namespace detail
}  // namespace ge
