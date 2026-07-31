// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T170 Gesture-hint timeline tests — tag order/timing and pointer
// invariants for every built-in gesture. Pure math, no GPU / sokol.

#include <ge/hint.h>
#include <ge/hint_hand.h>

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

// ── 🎯T179 multi-segment authoring ──────────────────────────────────

TEST_CASE("T179: Player from multi-waypoint contact clip stays in contact across the chain") {
    // A→B→A→B→A wiggle: one continuous contact, no intermediate lifts.
    const la::float2 A{0.40f, 0.55f};
    const la::float2 B{0.60f, 0.45f};
    la::float2 wps[] = {A, B, A, B, A};
    PhaseTiming timing;
    timing.approach = 0.20f;
    timing.pressIn  = 0.05f;
    timing.stroke   = 1.20f;
    timing.hold     = 0.02f;
    timing.pressOut = 0.10f;
    timing.exit     = 0.25f;

    Clip clip = makeContactClip(wps, timing, Ease::Linear);
    Params params = noLoop();
    Player p(clip, params);

    // Exactly one approach fade-in and one exit: opacity rises once, falls once.
    int contactEdges = 0;
    bool wasContact  = false;
    bool sawA = false, sawB = false;
    std::vector<la::float2> contactPath;
    const float end = p.duration() + 0.1f;
    for (float t = 0; t < end; t += kStep) {
        p.update(kStep);
        const PointerState& s = p.pointers()[0];
        if (s.contact != wasContact) {
            if (s.contact) ++contactEdges;
            wasContact = s.contact;
        }
        if (s.contact) {
            contactPath.push_back(s.pos);
            if (la::length(s.pos - A) < 0.03f) sawA = true;
            if (la::length(s.pos - B) < 0.03f) sawB = true;
        }
    }
    // One contact onset for the whole chain (not N flourishes).
    CHECK(contactEdges == 1);
    CHECK(sawA);
    CHECK(sawB);
    REQUIRE(contactPath.size() > 10);

    // Path visits waypoints in order: near A, then B, then A, …
    auto near = [](la::float2 p, la::float2 t) { return la::length(p - t) < 0.04f; };
    size_t idx = 0;
    auto advanceTo = [&](la::float2 target) {
        while (idx < contactPath.size() && !near(contactPath[idx], target)) ++idx;
        return idx < contactPath.size();
    };
    CHECK(advanceTo(A));
    CHECK(advanceTo(B));
    CHECK(advanceTo(A));
    CHECK(advanceTo(B));
    CHECK(advanceTo(A));

    // Tag order: one contact, one apex, one release.
    Player p2(clip, params);
    auto fired = play(p2);
    REQUIRE(fired.size() == 3);
    CHECK(fired[0].tag == tag::contact);
    CHECK(fired[1].tag == tag::apex);
    CHECK(fired[2].tag == tag::release);
}

TEST_CASE("T179: phase durations are independently tunable") {
    la::float2 wps[] = {{0.3f, 0.7f}, {0.7f, 0.3f}};

    PhaseTiming base;
    base.approach = 0.40f;
    base.pressIn  = 0.10f;
    base.stroke   = 0.80f;
    base.hold     = 0.05f;
    base.pressOut = 0.15f;
    base.exit     = 0.40f;

    PhaseTiming snappyStroke = base;
    snappyStroke.stroke      = 0.30f;  // only stroke shrinks

    Clip cBase   = makeContactClip(wps, base, Ease::Linear);
    Clip cSnappy = makeContactClip(wps, snappyStroke, Ease::Linear);

    // Contact tag time is approach+pressIn — unchanged when only stroke changes.
    REQUIRE(cBase.tags.size() >= 1);
    REQUIRE(cSnappy.tags.size() >= 1);
    CHECK(cBase.tags[0].t == doctest::Approx(cSnappy.tags[0].t).epsilon(0.001));
    CHECK(cBase.tags[0].tag == tag::contact);

    // Release comes earlier on the snappy clip by ~the stroke delta.
    auto releaseT = [](const Clip& c) {
        for (const auto& e : c.tags)
            if (e.tag == tag::release) return e.t;
        return -1.f;
    };
    float rBase   = releaseT(cBase);
    float rSnappy = releaseT(cSnappy);
    CHECK(rBase > 0);
    CHECK(rSnappy > 0);
    CHECK(rBase - rSnappy == doctest::Approx(base.stroke - snappyStroke.stroke).epsilon(0.02));

    // Approach wall-time: first time opacity reaches ~1 should match for both
    // (independent of stroke).
    auto approachWall = [](const Clip& c) {
        Params params;
        params.loop = false;
        Player p(c, params);
        for (float t = 0; t < p.duration(); t += kStep) {
            p.update(kStep);
            if (p.pointers()[0].opacity >= 0.99f) return p.time();
        }
        return -1.f;
    };
    float a0 = approachWall(cBase);
    float a1 = approachWall(cSnappy);
    CHECK(a0 > 0);
    CHECK(a1 > 0);
    CHECK(a0 == doctest::Approx(a1).epsilon(0.02));
}

TEST_CASE("T179: every built-in Kind is expressible via the public authoring path") {
    // makeClip routes through public builders; Player(Clip) must fire the
    // same tag order/monotonic times as Player(Kind).
    for (Kind kind : {Kind::Tap, Kind::DoubleTap, Kind::LongPress, Kind::Swipe, Kind::Drag,
                      Kind::PinchZoom, Kind::PinchRotate}) {
        CAPTURE(int(kind));
        Params params = noLoop();
        Clip   clip   = makeClip(kind, params);

        Player viaKind(kind, params);
        Player viaClip(clip, params);

        auto tagsKind = play(viaKind);
        auto tagsClip = play(viaClip);
        REQUIRE(tagsKind.size() == tagsClip.size());
        REQUIRE(tagsKind.size() == clip.tags.size());
        for (size_t i = 0; i < tagsKind.size(); ++i) {
            CHECK(tagsKind[i].tag == tagsClip[i].tag);
            CHECK(tagsKind[i].t == doctest::Approx(tagsClip[i].t).epsilon(0.01));
            CHECK(tagsClip[i].tag == clip.tags[i].tag);
            // Monotonic tag times within each player.
            if (i > 0) CHECK(tagsClip[i].t >= tagsClip[i - 1].t);
        }
    }
}

TEST_CASE("T179: multi-waypoint net-zero path starts and ends at the same contact pos") {
    // yourworld2 3× wiggle: A→B→A→B→A is net-zero by construction.
    const la::float2 A{0.46f, 0.54f};
    const la::float2 B{0.58f, 0.47f};
    la::float2 wps[] = {A, B, A, B, A};
    PhaseTiming timing;
    timing.stroke = 0.90f;
    Clip clip = makeContactClip(wps, timing, Ease::InOut);

    Params params = noLoop();
    Player p(clip, params);
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
    CHECK(first.x == doctest::Approx(A.x).epsilon(0.02));
    CHECK(first.y == doctest::Approx(A.y).epsilon(0.02));
    CHECK(last.x == doctest::Approx(A.x).epsilon(0.02));
    CHECK(last.y == doctest::Approx(A.y).epsilon(0.02));
}

// ── 🎯T180 pointing-hand articulation ───────────────────────────────

TEST_CASE("T180: contact tip of layout equals the supplied pointer tip") {
    const la::float2 tip{120.f, 340.f};
    const float      S = 100.f;
    auto layout = layoutPointingHand(tip, S, /*press=*/1.f, /*bodyHome=*/tip);
    CHECK(layout.tip.x == doctest::Approx(tip.x));
    CHECK(layout.tip.y == doctest::Approx(tip.y));
    // Index distal starts at local (0,0) → world tip after xform.
    REQUIRE(!layout.capsules.empty());
    bool foundTip = false;
    for (const Capsule& c : layout.capsules) {
        if (c.chain != 1) continue;
        if (la::length(c.a - tip) < 0.5f || la::length(c.b - tip) < 0.5f) foundTip = true;
    }
    CHECK(foundTip);
}

TEST_CASE("T180: small tip delta keeps palm nearly anchored") {
    const float      S    = 100.f;
    const la::float2 home{200.f, 200.f};
    auto rest = layoutPointingHand(home, S, 1.f, home);

    const la::float2 smallTip = home + la::float2{6.f, 0.f};  // 0.06 S < softReach
    auto moved = layoutPointingHand(smallTip, S, 1.f, home);

    const float tipDisp  = la::length(smallTip - home);
    const float palmDisp = la::length(moved.palmCentroid - rest.palmCentroid);
    CHECK(tipDisp > 0.f);
    // Finger/wrist dominate: palm moves much less than the tip.
    CHECK(palmDisp < tipDisp * 0.35f);
}

TEST_CASE("T180: large tip delta moves the palm bodily") {
    const float      S    = 100.f;
    const la::float2 home{200.f, 200.f};
    auto rest = layoutPointingHand(home, S, 1.f, home);

    const la::float2 largeTip = home + la::float2{80.f, 0.f};  // 0.80 S > hardReach
    auto moved = layoutPointingHand(largeTip, S, 1.f, home);

    const float tipDisp  = la::length(largeTip - home);
    const float palmDisp = la::length(moved.palmCentroid - rest.palmCentroid);
    // Bodily regime: palm tracks a large fraction of the tip travel.
    CHECK(palmDisp > tipDisp * 0.70f);
}

TEST_CASE("T180: intermediate amplitudes blend continuously (no step)") {
    const float      S    = 100.f;
    const la::float2 home{200.f, 200.f};
    auto rest = layoutPointingHand(home, S, 1.f, home);

    // Sample palm displacement vs tip offset magnitude; successive samples
    // must not jump by more than a small fraction of S (continuous blend).
    float prevPalm = 0.f;
    float prevTip  = 0.f;
    float maxJump  = 0.f;
    for (int i = 0; i <= 40; ++i) {
        float tipOff = float(i) / 40.f * 0.90f * S;  // 0 → 0.9 S
        auto L = layoutPointingHand(home + la::float2{tipOff, 0.f}, S, 1.f, home);
        float palmOff = la::length(L.palmCentroid - rest.palmCentroid);
        if (i > 0) {
            float dPalm = palmOff - prevPalm;
            float dTip  = tipOff - prevTip;
            // Palm path derivative vs tip offset — no discontinuous leap.
            if (dTip > 1e-4f) maxJump = std::max(maxJump, std::fabs(dPalm));
            CHECK(dPalm >= -0.5f);  // palm shouldn't reverse sharply
        }
        prevPalm = palmOff;
        prevTip  = tipOff;
    }
    // Per-step palm change is bounded (continuous); 40 steps over 0.9S →
    // a hard switch would show a single step ≈ full palm travel (~0.9S).
    CHECK(maxJump < 0.25f * S);
}

TEST_CASE("T180: over-reach increases wrist pivot rather than pure pan") {
    const float      S    = 100.f;
    const la::float2 home{200.f, 200.f};
    PointingLayoutParams p;
    p.softReach = 0.10f;
    p.hardReach = 0.55f;
    p.fingerMax = 0.22f;

    // Hold bodyHome fixed and push tip past fingerMax with bodyT still low
    // (offset just above soft, but we'll force large residual by fixing home
    // after a small body follow). Use a moderate offset that leaves residual.
    // Force bodyT ~ 0 by using soft threshold high, fingerMax low:
    p.softReach = 0.50f;
    p.hardReach = 0.90f;
    p.fingerMax = 0.15f;

    auto atRest = layoutPointingHand(home, S, 1.f, home, nullptr, p);
    CHECK(std::fabs(atRest.wristAngle) < 0.05f);

    const la::float2 farTip = home + la::float2{0.f, 50.f};  // 0.5 S residual
    auto over = layoutPointingHand(farTip, S, 1.f, home, nullptr, p);
    CHECK(std::fabs(over.wristAngle) > 0.15f);

    // Tip still pinned.
    CHECK(over.tip.x == doctest::Approx(farTip.x));
    CHECK(over.tip.y == doctest::Approx(farTip.y));
}
