// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T92.2.1 render-fidelity oracle (the "V" node: encoder+decoder run against
// each other). A VideoEncoder→VideoDecoder round-trip must preserve colour
// channels. This is the committed, repeatable check for the R↔B channel-swap
// fault class — the escape that reached a human's eyes ("dirt was blue, no red
// tail lights") when the encoder wrapped ge's RGBA capture as BGRA. Feed a frame
// with a known RED region and a BLUE region; after encode+decode, red must stay
// red and blue must stay blue. A channel swap flips both, so this test IS the
// injected-fault check for that fault class (oracle-first rule 12: test the
// oracle, not just the code — revert VideoEncoder_apple.mm's swap and this
// fails).

#include <doctest.h>

#include <ge/VideoDecoder.h>
#include <ge/VideoEncoder.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

// Split an AVCC access unit (4-byte big-endian length + NAL) by NAL type into
// SPS (7), PPS (8), and IDR slice (5) — the encoder prepends SPS/PPS on
// keyframes, and the decoder wants the parameter sets separately.
void splitAVCC(const std::vector<uint8_t>& au, std::vector<uint8_t>& sps,
               std::vector<uint8_t>& pps, std::vector<uint8_t>& idr) {
    for (size_t i = 0; i + 4 <= au.size();) {
        const uint32_t len = (uint32_t(au[i]) << 24) | (uint32_t(au[i + 1]) << 16) |
                             (uint32_t(au[i + 2]) << 8) | uint32_t(au[i + 3]);
        i += 4;
        if (len == 0 || i + len > au.size()) break;
        const uint8_t type = au[i] & 0x1f;
        if (type == 7) sps.assign(&au[i], &au[i] + len);
        else if (type == 8) pps.assign(&au[i], &au[i] + len);
        else if (type == 5) idr.assign(&au[i], &au[i] + len);
        i += len;
    }
}

}  // namespace

TEST_CASE("VideoEncoder→VideoDecoder round-trip preserves R/B channels (no swap)") {
    constexpr int W = 128, H = 64;

    // Left half red, right half blue — in RGBA, ge's frame-capture contract.
    std::vector<uint8_t> rgba(size_t(W) * H * 4);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t* p = &rgba[(size_t(y) * W + x) * 4];
            const bool left = x < W / 2;
            p[0] = left ? 255 : 0;  // R
            p[1] = 0;               // G
            p[2] = left ? 0 : 255;  // B
            p[3] = 255;             // A
        }

    // Encode → capture the keyframe (AVCC: SPS+PPS+IDR).
    std::mutex mu;
    std::vector<uint8_t> keyframe;
    {
        ge::VideoEncoder enc(W, H, 60, [&](ge::VideoEncoder::Frame f) {
            std::lock_guard<std::mutex> lk(mu);
            if (f.isKeyframe && keyframe.empty())
                keyframe.assign(f.data, f.data + f.size);
        });
        enc.encode(rgba.data(), size_t(W) * 4);
        enc.flush();
    }
    REQUIRE_FALSE(keyframe.empty());

    std::vector<uint8_t> sps, pps, idr;
    splitAVCC(keyframe, sps, pps, idr);
    REQUIRE_FALSE(sps.empty());
    REQUIRE_FALSE(pps.empty());
    REQUIRE_FALSE(idr.empty());

    // Decode → capture the BGRA frame.
    ge::VideoFrame frame{};
    std::vector<uint8_t> pixels;
    bool got = false;
    {
        ge::VideoDecoder dec([&](const ge::VideoFrame& vf) {
            std::lock_guard<std::mutex> lk(mu);
            if (got) return;
            got = true;
            frame = vf;
            pixels.resize(size_t(vf.height) * vf.strides[0]);
            std::memcpy(pixels.data(), vf.planes[0], pixels.size());
        });
        dec.setParameterSets(sps.data(), sps.size(), pps.data(), pps.size());
        std::vector<uint8_t> annexb = {0, 0, 0, 1};  // Annex-B start code + IDR
        annexb.insert(annexb.end(), idr.begin(), idr.end());
        dec.decode(annexb.data(), annexb.size());
        dec.flush();
    }
    REQUIRE(got);
    REQUIRE(frame.format == ge::VideoFrame::Format::BGRA);

    // Sample well inside each region (avoid the boundary + H.264 chroma bleed).
    auto bgra = [&](int x, int y) {
        const uint8_t* p = pixels.data() + size_t(y) * frame.strides[0] + size_t(x) * 4;
        return std::array<int, 3>{p[0], p[1], p[2]};  // B, G, R
    };
    const auto red = bgra(W / 4, H / 2);       // left region
    const auto blue = bgra(3 * W / 4, H / 2);  // right region

    // Red stays red: R (index 2) ≫ B (index 0). The +40 margin absorbs H.264
    // 4:2:0 chroma loss; a channel swap would invert the inequality.
    CHECK(red[2] > red[0] + 40);
    // Blue stays blue: B ≫ R.
    CHECK(blue[0] > blue[2] + 40);
}
