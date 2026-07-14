// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T128.2 spike oracles: pass-through + content-addressed cache round-trip
// and cold/warm/steady bandwidth vs an H.264 full-res estimate.

#include <doctest.h>
#include <ge/CmdStream.h>

#include <cstring>
#include <vector>

using namespace ge::cmdstream;

namespace {

// Consume every typed op field so the reader can walk a full synthetic frame.
bool skipVisit(Op op, Reader::Cursor& c, void*) {
    switch (op) {
    case Op::Blob:
    case Op::BlobRef:
    case Op::End:
        return true;
    case Op::FrameBegin:
        (void)c.u32();
        (void)c.u8();
        return c.ok;
    case Op::FrameEnd:
    case Op::EndPass:
    case Op::Commit:
        return true;
    case Op::MakeBuffer:
        (void)c.u32();
        (void)c.u32();
        (void)c.u32();
        (void)c.hash();
        return c.ok;
    case Op::MakeImage:
        (void)c.u32();
        (void)c.u16();
        (void)c.u16();
        (void)c.u32();
        (void)c.hash();
        return c.ok;
    case Op::UpdateBuffer:
    case Op::UpdateImage:
        (void)c.u32();
        (void)c.hash();
        return c.ok;
    case Op::DestroyBuffer:
    case Op::DestroyImage:
        (void)c.u32();
        return c.ok;
    case Op::BeginPass: {
        uint8_t n = c.u8();
        for (uint8_t i = 0; i < n * 4; ++i) (void)c.u32();
        (void)c.u8();
        return c.ok;
    }
    case Op::ApplyPipeline:
        (void)c.u32();
        return c.ok;
    case Op::ApplyBindings: {
        uint8_t nv = c.u8();
        for (uint8_t i = 0; i < nv; ++i) (void)c.u32();
        (void)c.u32();
        uint8_t ni = c.u8();
        for (uint8_t i = 0; i < ni; ++i) (void)c.u32();
        return c.ok;
    }
    case Op::ApplyUniforms:
        (void)c.u8();
        (void)c.hash();
        return c.ok;
    case Op::Draw:
        (void)c.i32();
        (void)c.i32();
        (void)c.i32();
        return c.ok;
    }
    return false; // unknown
}

} // namespace

TEST_CASE("cmdstream hash is stable and content-addressed") {
    const char* a = "hello";
    const char* b = "hello";
    const char* c = "world";
    CHECK(hashBytes(a, 5) == hashBytes(b, 5));
    CHECK(hashBytes(a, 5) != hashBytes(c, 5));
    CHECK(hashHex(hashBytes(a, 5)).size() == 32);
}

TEST_CASE("cmdstream cache put/get") {
    Cache cache;
    Hash h = hashBytes("abc", 3);
    CHECK_FALSE(cache.contains(h));
    CHECK(cache.put(h, "abc", 3));
    CHECK(cache.contains(h));
    auto* v = cache.get(h);
    REQUIRE(v);
    CHECK(v->size() == 3);
    CHECK(std::memcmp(v->data(), "abc", 3) == 0);
    CHECK_FALSE(cache.put(h, "abc", 3)); // overwrite, not new
}

TEST_CASE("cmdstream cold connect pays full assets; warm is draw-list only") {
    auto scene = SyntheticScene::tiltbuggyLike();
    Cache serverCache;
    Cache playerCache;

    // ── cold connect ──────────────────────────────────────────────
    Writer cold(&serverCache);
    scene.writeFrame(cold, /*seq*/ 0, /*first*/ true);
    auto coldBytes = cold.take();
    auto coldW = cold.stats();

    Reader rCold(&playerCache);
    CHECK(rCold.decode(coldBytes, skipVisit, nullptr));
    auto coldR = rCold.stats();
    CHECK(coldR.fullBlobCount >= 4); // 2 images + 2 buffers
    CHECK(coldR.cacheMisses == 0);
    CHECK(playerCache.size() >= 4);
    CHECK(coldW.fullBlobBytes == coldR.fullBlobBytes);

    // ── warm frames (steady state) ────────────────────────────────
    // Per-frame uniforms change → small full blobs; large assets are not
    // re-sent. Measure draw/uniform traffic only.
    size_t warmTotal = 0;
    constexpr int kWarm = 60;
    for (int i = 1; i <= kWarm; ++i) {
        Writer w(&serverCache);
        w.resetStats();
        scene.writeFrame(w, static_cast<uint32_t>(i), /*first*/ false);
        auto payload = w.take();
        warmTotal += payload.size();
        CHECK(w.stats().fullBlobBytes < 4096); // uniforms only

        Reader r(&playerCache);
        CHECK(r.decode(payload, skipVisit, nullptr));
        CHECK(r.stats().cacheMisses == 0);
    }

    const size_t avgWarm = warmTotal / kWarm;
    // Steady frame should be tiny relative to cold asset dump.
    CHECK(avgWarm < coldBytes.size() / 100);
    CHECK(avgWarm < 2048);

    // ── warm reconnect: re-send first-frame ops with hot server cache ──
    // Large textures/mesh become BlobRef; only small new uniforms are full.
    Writer recon(&serverCache);
    recon.resetStats();
    scene.writeFrame(recon, /*seq*/ 0, /*first*/ true);
    auto reconBytes = recon.take();
    auto reconSt = recon.stats();
    CHECK(reconSt.refBlobCount >= 4); // dirt, asphalt, verts, idx
    CHECK(reconBytes.size() < coldBytes.size() / 10);

    Reader rRecon(&playerCache);
    CHECK(rRecon.decode(reconBytes, skipVisit, nullptr));
    CHECK(rRecon.stats().cacheMisses == 0);
    CHECK(rRecon.stats().refBlobCount >= 4);

    // ── create-tax: deliberate large asset change ─────────────────
    Writer churn(&serverCache);
    scene.writeFrame(churn, 1000, /*first*/ false, /*forceTextureChurn*/ true);
    auto churnBytes = churn.take();
    auto churnSt = churn.stats();
    CHECK(churnSt.fullBlobCount >= 1);
    CHECK(churnBytes.size() > avgWarm * 10);
    Reader rChurn(&playerCache);
    CHECK(rChurn.decode(churnBytes, skipVisit, nullptr));

    // ── vs H.264 full-res estimate ────────────────────────────────
    // Steady-state is the product metric (OTA continuous play). Cold dump is
    // a one-time connect cost (accepted create-tax class).
    auto h264 = estimateH264FullRes(2048, 1536);
    const size_t h264_60 = h264.bytesForFrames(60);
    CHECK(warmTotal < h264_60);
    // Warm reconnect of the full resource set should also beat one IDR.
    CHECK(reconBytes.size() < h264.keyframeBytes);

    MESSAGE("cold_bytes=", coldBytes.size(),
            " avg_warm=", avgWarm,
            " recon_bytes=", reconBytes.size(),
            " recon_refs=", reconSt.refBlobCount,
            " churn_bytes=", churnBytes.size(),
            " warm_60f=", warmTotal,
            " h264_60f=", h264_60,
            " h264_key=", h264.keyframeBytes);
}

TEST_CASE("cmdstream BlobRef fails on cold player cache") {
    Cache serverOnly;
    Writer w(&serverOnly);
    const char* payload = "asset-bytes-here";
    w.emitBlob(payload, std::strlen(payload));
    // Second emit is a ref (server remembers).
    w.emitBlob(payload, std::strlen(payload));
    auto bytes = w.take();

    Cache emptyPlayer;
    Reader r(&emptyPlayer);
    // First Blob fills cache; second BlobRef hits — should succeed.
    CHECK(r.decode(bytes, skipVisit, nullptr));
    CHECK(r.stats().fullBlobCount == 1);
    CHECK(r.stats().refBlobCount == 1);

    // Ref-only payload against empty cache fails.
    Writer w2(&serverOnly);
    w2.emitBlob(payload, std::strlen(payload)); // ref only
    auto refOnly = w2.take();
    Cache empty2;
    Reader r2(&empty2);
    CHECK_FALSE(r2.decode(refOnly, skipVisit, nullptr));
    CHECK(r2.stats().cacheMisses == 1);
}
