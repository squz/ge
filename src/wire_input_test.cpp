// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Regression tests for the brokered-bridge wire input guards
// (docs/audit/fable-2026-07.md F5 / F6 — 🎯T142 / 🎯T143).

#include <doctest.h>

#include "bridge/wire_input.h"

#include <cstdint>
#include <cstring>
#include <vector>

using ge::detail::decodeVideoStreamMessage;
using ge::detail::wsPayloadWithinCap;

namespace {

void putU32(std::vector<char>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<char>((x >> (8 * i)) & 0xFF));
}

// A kVideoStreamMagic message: [magic][length][payload...]. `length` is written
// verbatim into the header (so tests can inject inconsistent values); the caller
// controls how many payload bytes actually follow.
std::vector<char> makeVideoMsg(uint32_t length, uint32_t seq, size_t payloadBytes) {
    std::vector<char> v;
    putU32(v, wire::kVideoStreamMagic);
    putU32(v, length);
    if (payloadBytes >= 1) v.push_back(0x00);  // flags byte @8
    if (payloadBytes >= 5) {                    // seq @9..12
        for (int i = 0; i < 4; ++i) v.push_back(static_cast<char>((seq >> (8 * i)) & 0xFF));
    }
    while (v.size() < 8 + payloadBytes) v.push_back(static_cast<char>(0xAB));  // avcc filler
    return v;
}

}  // namespace

TEST_CASE("decodeVideoStreamMessage accepts a well-formed message") {
    // length=9 → flags(1)+seq(4)+avcc(4); total 17 bytes.
    auto data = makeVideoMsg(/*length=*/9, /*seq=*/0x11223344, /*payloadBytes=*/9);
    uint32_t seq = 0;
    const uint8_t* avccData = nullptr;
    size_t avccSize = 0;
    REQUIRE(decodeVideoStreamMessage(data, seq, avccData, avccSize));
    CHECK(seq == 0x11223344u);
    CHECK(avccSize == 4u);  // length - 5
    CHECK(avccData == reinterpret_cast<const uint8_t*>(data.data()) + 13);
}

TEST_CASE("decodeVideoStreamMessage accepts the minimal length==5 message (empty AVCC)") {
    auto data = makeVideoMsg(/*length=*/5, /*seq=*/7, /*payloadBytes=*/5);  // total 13
    uint32_t seq = 0;
    const uint8_t* avccData = nullptr;
    size_t avccSize = 0;
    REQUIRE(decodeVideoStreamMessage(data, seq, avccData, avccSize));
    CHECK(seq == 7u);
    CHECK(avccSize == 0u);
}

TEST_CASE("decodeVideoStreamMessage rejects length < 5 (would underflow length-5)") {
    // F5 trigger #2: length=1, 9-byte buffer. Old code passed the guard
    // (8+1==9, !(9<9)) then read seq at [9,13) OOB and set avccSize = 1-5 = ~4GB.
    auto data = makeVideoMsg(/*length=*/1, /*seq=*/0, /*payloadBytes=*/1);  // total 9
    REQUIRE(data.size() == 9u);
    uint32_t seq = 0;
    const uint8_t* avccData = nullptr;
    size_t avccSize = 0;
    CHECK_FALSE(decodeVideoStreamMessage(data, seq, avccData, avccSize));
}

TEST_CASE("decodeVideoStreamMessage rejects length that overflows 8+length in 32-bit") {
    // F5 trigger #1: length=0xFFFFFFFF. Old code computed 8+length in uint32
    // (== 7), so even a tiny buffer passed the guard. In 64-bit it must fail.
    auto data = makeVideoMsg(/*length=*/0xFFFFFFFFu, /*seq=*/0, /*payloadBytes=*/1);  // total 9
    uint32_t seq = 0;
    const uint8_t* avccData = nullptr;
    size_t avccSize = 0;
    CHECK_FALSE(decodeVideoStreamMessage(data, seq, avccData, avccSize));
}

TEST_CASE("decodeVideoStreamMessage rejects a payload that is one byte short") {
    // length=9 claims 9 payload bytes but only 8 are present (total 16, need 17).
    auto data = makeVideoMsg(/*length=*/9, /*seq=*/1, /*payloadBytes=*/8);
    REQUIRE(data.size() == 16u);
    uint32_t seq = 0;
    const uint8_t* avccData = nullptr;
    size_t avccSize = 0;
    CHECK_FALSE(decodeVideoStreamMessage(data, seq, avccData, avccSize));
}

TEST_CASE("decodeVideoStreamMessage rejects a truncated header (size < 8)") {
    std::vector<char> data(4, 0x00);  // magic only
    uint32_t seq = 0;
    const uint8_t* avccData = nullptr;
    size_t avccSize = 0;
    CHECK_FALSE(decodeVideoStreamMessage(data, seq, avccData, avccSize));
}

TEST_CASE("wsPayloadWithinCap accepts lengths up to kMaxMessageSize") {
    CHECK(wsPayloadWithinCap(0, 0));
    CHECK(wsPayloadWithinCap(1000, 0));
    CHECK(wsPayloadWithinCap(wire::kMaxMessageSize, 0));
}

TEST_CASE("wsPayloadWithinCap rejects a single oversized frame") {
    CHECK_FALSE(wsPayloadWithinCap(wire::kMaxMessageSize + 1, 0));
    // F6 trigger: the 127-extended length form with the top bit set.
    CHECK_FALSE(wsPayloadWithinCap(0x7FFFFFFFFFFFFFFFull, 0));
    CHECK_FALSE(wsPayloadWithinCap(UINT64_MAX, 0));
}

TEST_CASE("wsPayloadWithinCap bounds fragmented-continuation accumulation") {
    // Each frame is individually allocatable but the running total exceeds the cap.
    CHECK_FALSE(wsPayloadWithinCap(wire::kMaxMessageSize, 1));
    CHECK_FALSE(wsPayloadWithinCap(1, wire::kMaxMessageSize));
    // Exactly filling the remaining budget is allowed.
    CHECK(wsPayloadWithinCap(100, wire::kMaxMessageSize - 100));
}
