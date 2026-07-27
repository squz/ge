// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T170 Gesture-hint timeline tests — tag order/timing and pointer
// invariants for every built-in gesture. Pure math, no GPU / sokol.

#include <ge/hint.h>

#include <doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace ge;
using namespace ge::hint;

namespace {

constexpr float kStep = 0.001f;  // 1 ms stepping → tag timing resolvable to ±1 step

struct Fired {
    std::string tag;
    float       t;  // player time when the tag arrived
};

// Play a whole (non-looping) clip in kStep increments, recording tags.
std::vector<Fired> play(Player& p, float extra = 0.2f) {
    std::vector<Fired> fired;
    p.onTag = [&](std::string_view tag) { fired.push_back({std::string(tag), p.time()}); };
    const float end = p.duration() + extra;
    for (float t = 0; t < end; t += kStep) p.update(kStep);
    return fired;
}

Params noLoop() {
    Params p;
    p.loop = false;
    return p;
}

}  // namespace

// ── tag order and timing ────────────────────────────────────────────

TEST_CASE("every gesture fires its clip's tags in order, at the declared times") {
    for (Kind kind : {Kind::Tap, Kind::DoubleTap, Kind::LongPress, Kind::Swipe, Kind::Drag,
                      Kind::PinchZoom, Kind::PinchRotate}) {
        CAPTURE(int(kind));
        Params params = noLoop();
        Clip   clip   = makeClip(kind, params);
        REQUIRE(!clip.tags.empty());

        Player p(kind, params);
        auto   fired = play(p);

        REQUIRE(fired.size() == clip.tags.size());
        for (size_t i = 0; i < fired.size(); ++i) {
            CHECK(fired[i].tag == clip.tags[i].tag);
            // The tag must arrive on the update that crosses its time:
            // within one step (plus float accumulation slack).
            CHECK(fired[i].t >= clip.tags[i].t - kStep);
            CHECK(fired[i].t <= clip.tags[i].t + 2 * kStep);
        }
    }
}

TEST_CASE("tap fires contact then release") {
    Player p(Kind::Tap, noLoop());
    auto   fired = play(p);
    REQUIRE(fired.size() == 2);
    CHECK(fired[0].tag == tag::contact);
    CHECK(fired[1].tag == tag::release);
    CHECK(fired[0].t < fired[1].t);
}

TEST_CASE("double-tap fires two contact/release pairs") {
    Player p(Kind::DoubleTap, noLoop());
    auto   fired = play(p);
    REQUIRE(fired.size() == 4);
    CHECK(fired[0].tag == tag::contact);
    CHECK(fired[1].tag == tag::release);
    CHECK(fired[2].tag == tag::contact);
    CHECK(fired[3].tag == tag::release);
}

TEST_CASE("long-press: hold-start lands between contact and release") {
    Player p(Kind::LongPress, noLoop());
    auto   fired = play(p);
    REQUIRE(fired.size() == 3);
    CHECK(fired[0].tag == tag::contact);
    CHECK(fired[1].tag == tag::holdStart);
    CHECK(fired[2].tag == tag::release);
    // The hold must be long enough to read as deliberate.
    CHECK(fired[2].t - fired[0].t > 0.6f);
}

TEST_CASE("swipe and drag: apex lands strictly between contact and release") {
    for (Kind kind : {Kind::Swipe, Kind::Drag}) {
        CAPTURE(int(kind));
        Player p(kind, noLoop());
        auto   fired = play(p);
        REQUIRE(fired.size() == 3);
        CHECK(fired[0].tag == tag::contact);
        CHECK(fired[1].tag == tag::apex);
        CHECK(fired[2].tag == tag::release);
        CHECK(fired[1].t > fired[0].t);
        CHECK(fired[1].t < fired[2].t);
    }
}

// ── pointer-state invariants ────────────────────────────────────────

TEST_CASE("contact flag tracks the contact/release (pinch-start/end) tags") {
    for (Kind kind : {Kind::Tap, Kind::LongPress, Kind::Swipe, Kind::PinchZoom}) {
        CAPTURE(int(kind));
        Player p(kind, noLoop());
        int    depth = 0;  // 0 = up, 1 = down
        p.onTag = [&](std::string_view t) {
            if (t == tag::contact || t == tag::pinchStart) depth = 1;
            if (t == tag::release || t == tag::pinchEnd) depth = 0;
            // Immediately after the tag, every pointer's contact flag must
            // agree with the tag we just processed (they share key times).
            for (const PointerState& s : p.pointers()) CHECK(s.contact == (depth == 1));
        };
        const float end = p.duration() + 0.2f;
        for (float t = 0; t < end; t += kStep) {
            p.update(kStep);
            // Between tags the flag must hold the last tagged state.
            for (const PointerState& s : p.pointers()) CHECK(s.contact == (depth == 1));
        }
    }
}

TEST_CASE("while in contact, pointers stay inside the unit box") {
    for (Kind kind : {Kind::Tap, Kind::DoubleTap, Kind::LongPress, Kind::Swipe, Kind::Drag,
                      Kind::PinchZoom, Kind::PinchRotate}) {
        CAPTURE(int(kind));
        Player      p(kind, noLoop());
        const float end = p.duration() + 0.1f;
        for (float t = 0; t < end; t += kStep) {
            p.update(kStep);
            for (const PointerState& s : p.pointers()) {
                if (!s.contact) continue;
                CHECK(s.pos.x >= 0.0f);
                CHECK(s.pos.x <= 1.0f);
                CHECK(s.pos.y >= 0.0f);
                CHECK(s.pos.y <= 1.0f);
            }
        }
    }
}

TEST_CASE("opacity starts at 0, peaks at 1 during contact, ends at 0") {
    Player p(Kind::Tap, noLoop());
    CHECK(p.pointers()[0].opacity == 0.0f);
    bool sawFull = false;
    const float end = p.duration() + 0.1f;
    for (float t = 0; t < end; t += kStep) {
        p.update(kStep);
        const PointerState& s = p.pointers()[0];
        if (s.contact) {
            CHECK(s.opacity == doctest::Approx(1.0f));
            sawFull = true;
        }
    }
    CHECK(sawFull);
    CHECK(p.pointers()[0].opacity == doctest::Approx(0.0f).epsilon(0.01));
}

TEST_CASE("swipe travels from `from` to `to` while in contact") {
    Params params  = noLoop();
    params.from    = {0.2f, 0.8f};
    params.to      = {0.8f, 0.2f};
    Player p(Kind::Swipe, params);

    la::float2 first{-1, -1}, last{-1, -1};
    const float end = p.duration();
    for (float t = 0; t < end; t += kStep) {
        p.update(kStep);
        const PointerState& s = p.pointers()[0];
        if (s.contact) {
            if (first.x < 0) first = s.pos;
            last = s.pos;
        }
    }
    CHECK(first.x == doctest::Approx(params.from.x).epsilon(0.02));
    CHECK(first.y == doctest::Approx(params.from.y).epsilon(0.02));
    CHECK(last.x == doctest::Approx(params.to.x).epsilon(0.02));
    CHECK(last.y == doctest::Approx(params.to.y).epsilon(0.02));
}

TEST_CASE("pinch-zoom: two pointers spread apart during the pinch") {
    Player p(Kind::PinchZoom, noLoop());
    REQUIRE(p.pointers().size() == 2);
    float dStart = -1, dEnd = -1;
    const float end = p.duration();
    for (float t = 0; t < end; t += kStep) {
        p.update(kStep);
        auto ps = p.pointers();
        if (ps[0].contact && ps[1].contact) {
            float d = la::length(ps[1].pos - ps[0].pos);
            if (dStart < 0) dStart = d;
            dEnd = d;
        }
    }
    REQUIRE(dStart >= 0);
    CHECK(dEnd > dStart * 1.5f);
}

TEST_CASE("pinch-rotate: pointer pair rotates about a fixed center") {
    Params params = noLoop();
    Player p(Kind::PinchRotate, params);
    la::float2 center = (params.from + params.to) * 0.5f;

    float      angStart = 0, angEnd = 0, radius0 = -1;
    bool       any = false;
    const float end = p.duration();
    for (float t = 0; t < end; t += kStep) {
        p.update(kStep);
        auto ps = p.pointers();
        if (!(ps[0].contact && ps[1].contact)) continue;
        la::float2 r = ps[0].pos - center;
        float      ang = std::atan2(r.y, r.x);
        if (!any) { angStart = ang; radius0 = la::length(r); any = true; }
        angEnd = ang;
        // Radius stays constant — motion is an arc, not a chord.
        CHECK(la::length(r) == doctest::Approx(radius0).epsilon(0.03));
    }
    REQUIRE(any);
    CHECK(std::fabs(angEnd - angStart) > 1.0f);  // ~75° sweep expected
}

// ── playback mechanics ──────────────────────────────────────────────

TEST_CASE("looping replays tags every cycle") {
    Params params  = {};
    params.loop    = true;
    params.loopGap = 0.1f;
    Player p(Kind::Tap, params);
    int contacts = 0;
    p.onTag = [&](std::string_view t) { contacts += t == tag::contact; };
    const float cycles = 3.0f * (makeClip(Kind::Tap, params).duration + params.loopGap);
    for (float t = 0; t < cycles; t += kStep) p.update(kStep);
    CHECK(contacts == 3);
}

TEST_CASE("speed scales tag timing (wall clock)") {
    Params fast = noLoop();
    fast.speed  = 2.0f;
    Player p(Kind::Tap, fast);
    std::vector<float> wallTimes;
    float              wall = 0;
    p.onTag = [&](std::string_view) { wallTimes.push_back(wall); };
    while (wall < p.duration() + 0.1f) {
        p.update(kStep);
        wall += kStep;
    }
    Clip clip = makeClip(Kind::Tap, fast);
    REQUIRE(wallTimes.size() == 2);
    CHECK(wallTimes[0] == doctest::Approx(clip.tags[0].t / 2.0f).epsilon(0.02));
}

TEST_CASE("one giant dt still fires every tag, once, in order") {
    Player p(Kind::DoubleTap, noLoop());
    std::vector<std::string> fired;
    p.onTag = [&](std::string_view t) { fired.emplace_back(t); };
    p.update(1000.0f);
    REQUIRE(fired.size() == 4);
    CHECK(fired[0] == tag::contact);
    CHECK(fired[3] == tag::release);
    CHECK(!p.active());
}

TEST_CASE("a giant dt on a looping player does not replay unboundedly") {
    Params params  = {};
    params.loop    = true;
    params.loopGap = 0.2f;
    Player p(Kind::Tap, params);
    int fired = 0;
    p.onTag = [&](std::string_view) { ++fired; };
    p.update(100.0f);
    // Wrapping fires each crossed cycle; the count is bounded by cycles crossed.
    const float cycle = makeClip(Kind::Tap, params).duration + params.loopGap;
    CHECK(fired <= int(100.0f / cycle + 2) * 2);
    CHECK(p.time() < cycle);
}

TEST_CASE("non-looping player goes inactive after the clip; looping stays active") {
    Player once(Kind::Tap, noLoop());
    once.update(1000.0f);
    CHECK(!once.active());

    Player forever(Kind::Tap, {});
    forever.update(1000.0f);
    CHECK(forever.active());
}

TEST_CASE("reset replays tags from the start") {
    Player p(Kind::Tap, noLoop());
    int contacts = 0;
    p.onTag = [&](std::string_view t) { contacts += t == tag::contact; };
    p.update(1000.0f);
    CHECK(contacts == 1);
    p.reset();
    CHECK(p.time() == 0.0f);
    p.update(1000.0f);
    CHECK(contacts == 2);
}

TEST_CASE("default player is inert and safe") {
    Player p;
    p.update(0.016f);
    CHECK(p.pointers().empty());
}
