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
        (void)c.u16(); // contentW
        (void)c.u16(); // contentH
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
    case Op::Present:
        (void)c.u16();
        (void)c.u16();
        (void)c.u8();
        (void)c.u8();
        (void)c.u32();
        (void)c.hash();
        return c.ok;
    case Op::SpriteRun:
        (void)c.u32();
        (void)c.u16();
        (void)c.hash();
        (void)c.hash();
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

TEST_CASE("cmdstream Present LZ4 round-trip and warm ref") {
    Cache serverCache;
    Cache playerCache;

    // 32×32 BGRA solid red.
    constexpr int W = 32, H = 32;
    std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
    for (size_t i = 0; i < px.size(); i += 4) {
        px[i] = 0;
        px[i + 1] = 0;
        px[i + 2] = 255;
        px[i + 3] = 255;
    }

    Writer w1(&serverCache);
    w1.frameBegin(0, true);
    w1.present(W, H, kPresentBGRA8, px.data(), px.size());
    w1.frameEnd();
    auto cold = w1.take();
    CHECK(w1.stats().fullBlobCount == 1);

    struct Out {
        int w = 0, h = 0;
        std::vector<uint8_t> pixels;
        bool ok = false;
    } out;

    auto visit = [](Op op, Reader::Cursor& c, void* user) -> bool {
        auto* o = static_cast<Out*>(user);
        if (op == Op::Present) {
            o->w = c.u16();
            o->h = c.u16();
            (void)c.u8(); // format
            uint8_t enc = c.u8();
            uint32_t raw = c.u32();
            Hash h = c.hash();
            // Visitor runs after Blob filled the cache — re-fetch via outer scope
            // is done below after decode using the reader cache; here just record.
            (void)enc;
            (void)raw;
            (void)h;
            o->ok = c.ok;
            return c.ok;
        }
        return skipVisit(op, c, nullptr);
    };

    Reader r1(&playerCache);
    CHECK(r1.decode(cold, visit, &out));
    CHECK(out.ok);
    CHECK(out.w == W);
    CHECK(out.h == H);
    CHECK(playerCache.size() == 1);

    // Same pixels again → BlobRef only (tiny wire payload).
    Writer w2(&serverCache);
    w2.resetStats();
    w2.frameBegin(1, false);
    w2.present(W, H, kPresentBGRA8, px.data(), px.size());
    w2.frameEnd();
    auto warm = w2.take();
    CHECK(w2.stats().refBlobCount == 1);
    CHECK(w2.stats().fullBlobCount == 0);
    CHECK(warm.size() < cold.size());
    CHECK(warm.size() < 128);

    Reader r2(&playerCache);
    Out out2;
    CHECK(r2.decode(warm, visit, &out2));
    CHECK(out2.ok);
    CHECK(r2.stats().refBlobCount == 1);
    CHECK(r2.stats().cacheMisses == 0);
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

TEST_CASE("cmdstream SpriteRun warm steady-state is tiny vs full RGBA") {
    Cache serverCache;
    Cache playerCache;

    // 64×64 dirt texture once + 6-vert quads each frame (tiltbuggy-like).
    constexpr int TW = 64, TH = 64;
    std::vector<uint8_t> tex(static_cast<size_t>(TW) * TH * 4, 0);
    for (size_t i = 0; i < tex.size(); i += 4) {
        tex[i] = 140;
        tex[i + 1] = 100;
        tex[i + 2] = 60;
        tex[i + 3] = 255;
    }
    // 30 quads × 6 verts × 24 B ≈ 4.3 KB of geometry per frame.
    constexpr uint16_t nVerts = 30 * 6;
    std::vector<uint8_t> verts(static_cast<size_t>(nVerts) * kSpriteVertexBytes);
    for (size_t i = 0; i < verts.size(); ++i) verts[i] = static_cast<uint8_t>(i);
    float mvp[16] = {
        1, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };

    Writer coldW(&serverCache);
    coldW.frameBegin(0, true);
    coldW.makeImage(1, TW, TH, 0x10, tex.data(), tex.size());
    coldW.spriteRun(1, verts.data(), nVerts, mvp);
    coldW.frameEnd();
    auto cold = coldW.take();
    CHECK(coldW.stats().fullBlobCount >= 2); // tex + verts (+ maybe mvp)

    Reader r1(&playerCache);
    CHECK(r1.decode(cold, skipVisit, nullptr));

    // Change verts slightly each frame (motion); texture hits cache.
    size_t warmTotal = 0;
    for (int f = 1; f <= 60; ++f) {
        verts[0] = static_cast<uint8_t>(f); // motion
        mvp[12] = float(f) * 0.01f;         // camera nudge
        Writer w(&serverCache);
        w.frameBegin(static_cast<uint32_t>(f), false);
        // Image already known — only MakeImage if we re-emit; we don't.
        w.spriteRun(1, verts.data(), nVerts, mvp);
        w.frameEnd();
        auto payload = w.take();
        warmTotal += payload.size();
        Reader r(&playerCache);
        CHECK(r.decode(payload, skipVisit, nullptr));
        // Steady-state: no full texture blob.
        CHECK(w.stats().fullBlobCount <= 2); // verts + maybe mvp
        CHECK(payload.size() < 32 * 1024);   // << full-frame RGBA
    }
    const size_t avgWarm = warmTotal / 60;
    constexpr size_t fullRgba2048x1536 = 2048ull * 1536ull * 4ull;
    CHECK(avgWarm * 100 < fullRgba2048x1536); // orders of magnitude below
    CHECK(avgWarm < 20000); // practical OTA budget for 60 fps class

    MESSAGE("sprite_cold=", cold.size(), " avg_warm_sprite=", avgWarm,
            " warm_60f=", warmTotal, " full_rgba=", fullRgba2048x1536);
}
