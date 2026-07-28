// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T175.12 — characterization tests for the app-channel surfaces the T175
// session-scoping graph will rescope: time control, state slices +
// serializers, the game-thread task queue, perf counters, and the
// hit-target surface/extras. These lock CURRENT single-session behaviour
// so each scoping refactor is behaviour-preserving by test. Also covers
// the wire-fed viewer-backgrounded bit and the cmdstream LiveCapture
// set/clear hook (their CPU-visible halves).
//
// Handlers that marshal via runOnGameThread BLOCK until the game thread
// pumps — invokePumped() below runs the invoke on a worker and pumps from
// this thread, which is itself the characterization of the task queue's
// thread affinity.

#include "appchannel_internal.h"
#include "render/LifecycleInject.h"
#include "render/SensorControl.h"

#include <ge/CmdStream.h>
#include <ge/appchannel.h>
#include <ge/button.h>
#include <ge/metrics.h>

#include <doctest.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>

using nlohmann::json;
namespace ac  = ge::appchannel;
namespace det = ge::appchannel::detail;

namespace {

// Run a (possibly marshalling) handler from a worker thread while this —
// the "game" — thread pumps until it completes.
json invokePumped(const std::string& name, const json& params) {
    json               out;
    std::exception_ptr err;
    std::atomic<bool>  done{false};
    std::thread worker([&] {
        try {
            out = det::invokeMethodForTest(name, params);
        } catch (...) {
            err = std::current_exception();
        }
        done = true;
    });
    while (!done) ac::pumpMainThreadTasks();
    worker.join();
    if (err) std::rethrow_exception(err);
    return out;
}

}  // namespace

TEST_CASE("time-control RPCs drive applyTimeControl: pause holds, step advances N, speed scales, resume clears") {
    det::resetAppChannelStateForTest();
    const float dt = 0.016f;

    CHECK(ac::applyTimeControl(dt) == doctest::Approx(dt));  // pass-through

    det::invokeMethodForTest("pause", json::object());
    CHECK(ac::applyTimeControl(dt) == 0.0f);  // held: no sim advance

    det::invokeMethodForTest("step", json{{"frames", 2}});
    CHECK(ac::applyTimeControl(dt) == doctest::Approx(1.0f / 60.0f));
    CHECK(ac::applyTimeControl(dt) == doctest::Approx(1.0f / 60.0f));
    CHECK(ac::applyTimeControl(dt) == 0.0f);  // re-holds after N frames

    det::invokeMethodForTest("resume", json::object());
    CHECK(ac::applyTimeControl(dt) == doctest::Approx(dt));

    det::invokeMethodForTest("speed", json{{"multiplier", 2.0}});
    CHECK(ac::applyTimeControl(dt) == doctest::Approx(2.0f * dt));
    det::invokeMethodForTest("resume", json::object());  // clears speed to 1
    CHECK(ac::applyTimeControl(dt) == doctest::Approx(dt));

    det::resetAppChannelStateForTest();
}

TEST_CASE("state slices: registration, query via the game-thread marshal, unknown-slice error") {
    det::resetAppChannelStateForTest();

    const auto gameThread = std::this_thread::get_id();
    std::atomic<bool> ranOnGameThread{false};
    ac::registerStateSlice("t175_probe", [&] {
        ranOnGameThread = (std::this_thread::get_id() == gameThread);
        return json{{"v", 42}};
    });

    const json out = invokePumped("state_query", json{{"slice", "t175_probe"}});
    CHECK(out == json{{"v", 42}});
    // The getter must run on the pumping ("game") thread, not the worker.
    CHECK(ranOnGameThread.load());

    CHECK_THROWS(invokePumped("state_query", json{{"slice", "no_such_slice"}}));

    det::resetAppChannelStateForTest();
}

TEST_CASE("state serializer: save_state wraps msgpack bin, restore_state round-trips on the game thread") {
    det::resetAppChannelStateForTest();

    json restored;
    ac::registerStateSerializer([] { return json{{"x", 1}, {"y", "two"}}; },
                                [&](const json& j) { restored = j; });

    const json saved = invokePumped("save_state", json::object());
    REQUIRE(saved.contains("state"));
    REQUIRE(saved["state"].is_binary());
    CHECK(json::from_msgpack(saved["state"].get_binary()) ==
          json{{"x", 1}, {"y", "two"}});

    invokePumped("restore_state", saved);
    CHECK(restored == json{{"x", 1}, {"y", "two"}});

    det::resetAppChannelStateForTest();
}

TEST_CASE("game-thread task queue: two blocked workers both complete, getters run on the pump thread") {
    det::resetAppChannelStateForTest();

    const auto pumpThread = std::this_thread::get_id();
    int  runs = 0;
    bool affinityOk = true;
    ac::registerStateSlice("a", [&] {
        ++runs;
        affinityOk = affinityOk && std::this_thread::get_id() == pumpThread;
        return json{{"s", "a"}};
    });
    ac::registerStateSlice("b", [&] {
        ++runs;
        affinityOk = affinityOk && std::this_thread::get_id() == pumpThread;
        return json{{"s", "b"}};
    });

    json ra, rb;
    std::atomic<int> done{0};
    std::thread wa([&] { ra = det::invokeMethodForTest("state_query", json{{"slice", "a"}}); ++done; });
    std::thread wb([&] { rb = det::invokeMethodForTest("state_query", json{{"slice", "b"}}); ++done; });
    while (done.load() < 2) ac::pumpMainThreadTasks();
    wa.join();
    wb.join();

    CHECK(ra == json{{"s", "a"}});
    CHECK(rb == json{{"s", "b"}});
    CHECK(runs == 2);       // each ran exactly once...
    CHECK(affinityOk);      // ...on the pumping ("game") thread, never a worker

    det::resetAppChannelStateForTest();
}

TEST_CASE("perf counters: perfEmit stores latest per name; the window emits frame_ms + counters then resets") {
    det::resetAppChannelStateForTest();

    ac::perfEmit("buggy_x", 3.5);
    ac::perfEmit("buggy_x", 4.5);  // latest wins
    CHECK(det::perfCountersSnapshotForTest() == json{{"buggy_x", 4.5}});

    // Window is 1000 ms: three 400 ms frames cross it on the third.
    CHECK(det::perfAccumulate(400.0f).is_null());
    CHECK(det::perfAccumulate(400.0f).is_null());
    const json samples = det::perfAccumulate(400.0f);
    REQUIRE(samples.is_object());
    CHECK(samples["frame_ms"].get<double>() == doctest::Approx(1200.0 / 3.0));
    CHECK(samples["buggy_x"].get<double>() == doctest::Approx(4.5));

    // The window reset: next frame starts a fresh accumulation.
    CHECK(det::perfAccumulate(400.0f).is_null());

    det::resetAppChannelStateForTest();
}

TEST_CASE("hit_targets slice: published buttons + extras normalised against the registered surface") {
    det::resetAppChannelStateForTest();

    ac::setHitTargetSurfacePts(200.0f, 100.0f);
    ac::setExtraHitTargets(json::array({{{"id", "region1"},
                                         {"kind", "region"},
                                         {"enabled", true},
                                         {"bbox", {20, 10, 40, 20}}}}));

    ge::Button btn;
    btn.id   = "press_me";
    btn.role = "action";
    btn.setHitRect(ge::Rect{10, 10, 20, 20});
    ge::publishHitTarget(&btn);

    const json out = invokePumped("state_query", json{{"slice", "hit_targets"}});
    REQUIRE(out.contains("targets"));
    bool sawButton = false, sawExtra = false;
    for (const auto& t : out["targets"]) {
        if (t.value("id", "") == "press_me") {
            sawButton = true;
            REQUIRE(t.contains("bbox_norm"));
            CHECK(t["bbox_norm"][0].get<double>() == doctest::Approx(10.0 / 200.0));
            CHECK(t["bbox_norm"][1].get<double>() == doctest::Approx(10.0 / 100.0));
        }
        if (t.value("id", "") == "region1") sawExtra = true;
    }
    CHECK(sawButton);
    CHECK(sawExtra);

    ge::unpublishHitTarget(&btn);
    det::resetAppChannelStateForTest();
}

TEST_CASE("wire-fed viewer-backgrounded bit: inject sets, clears, and reads back") {
    CHECK(!ge::detail::viewerBackgrounded());
    ge::detail::injectViewerBackgrounded(true);
    CHECK(ge::detail::viewerBackgrounded());
    ge::detail::injectViewerBackgrounded(false);
    CHECK(!ge::detail::viewerBackgrounded());
}

TEST_CASE("cmdstream LiveCapture: set / read-back / clear (the draw path's CPU-visible hook)") {
    CHECK(ge::cmdstream::liveCapture() == nullptr);
    ge::cmdstream::LiveCapture cap;
    ge::cmdstream::setLiveCapture(&cap);
    CHECK(ge::cmdstream::liveCapture() == &cap);
    ge::cmdstream::setLiveCapture(nullptr);
    CHECK(ge::cmdstream::liveCapture() == nullptr);
}

// ── 🎯T175.1 session identity + RPC addressing ──────────────────────

TEST_CASE("🎯T175.1 session ids: unique, shared by copies, registry tracks lifetime") {
    const auto baseline = ge::liveSessionIds().size();
    {
        ge::Context a(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
        ge::Context b(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
        CHECK(a.sessionId() != b.sessionId());

        ge::Context aCopy = a;  // shared M — same session
        CHECK(aCopy.sessionId() == a.sessionId());

        const auto live = ge::liveSessionIds();
        CHECK(live.size() == baseline + 2);

        auto got = ge::sessionById(a.sessionId());
        REQUIRE(got.has_value());
        CHECK(got->sessionId() == a.sessionId());
    }
    // Both sessions died with their last Context copy.
    CHECK(ge::liveSessionIds().size() == baseline);
    CHECK(!ge::sessionById(0xFFFFFFu).has_value());
}

TEST_CASE("🎯T175.1 resolveSessionParam: sole-session default, explicit id, ambiguity + unknown errors") {
    REQUIRE(ge::liveSessionIds().empty());  // test isolation precondition
    CHECK_THROWS(det::resolveSessionParam(json::object()));  // no session at all

    ge::Context a(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    CHECK(det::resolveSessionParam(json::object()) == a.sessionId());  // sole default
    CHECK(det::resolveSessionParam(json{{"session", a.sessionId()}}) == a.sessionId());

    ge::Context b(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    CHECK_THROWS(det::resolveSessionParam(json::object()));  // ambiguous, unnamed
    CHECK(det::resolveSessionParam(json{{"session", b.sessionId()}}) == b.sessionId());
    CHECK_THROWS(det::resolveSessionParam(json{{"session", 0xFFFFFF}}));  // unknown id
}

// ── 🎯T175.2/3/4/10 per-session isolation ───────────────────────────

namespace {
// Session-addressed variant: pump the addressed session's queue.
json invokePumpedCtx(const std::string& name, const json& params, const ge::Context& ctx) {
    json               out;
    std::exception_ptr err;
    std::atomic<bool>  done{false};
    std::thread worker([&] {
        try {
            out = det::invokeMethodForTest(name, params);
        } catch (...) {
            err = std::current_exception();
        }
        done = true;
    });
    while (!done) ac::pumpMainThreadTasks(ctx);
    worker.join();
    if (err) std::rethrow_exception(err);
    return out;
}
}  // namespace

TEST_CASE("🎯T175.2/3/10: two sessions hold distinct slices, share defaults, pause independently") {
    det::resetAppChannelStateForTest();
    REQUIRE(ge::liveSessionIds().empty());

    ge::Context a(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    ge::Context b(64, 64, ge::DeviceClass::Desktop, ":memory:", "");

    ac::registerStateSlice(a, "who", [] { return json{{"i", "A"}}; });
    ac::registerStateSlice(b, "who", [] { return json{{"i", "B"}}; });
    ac::registerStateSlice("shared", [] { return json{{"d", 1}}; });  // defaults

    // Same slice name, different sessions, different answers — the
    // second registration no longer overwrites the first (T175.2), and
    // each runs via its own session's task queue (T175.3).
    CHECK(invokePumpedCtx("state_query",
                          json{{"slice", "who"}, {"session", a.sessionId()}}, a) ==
          json{{"i", "A"}});
    CHECK(invokePumpedCtx("state_query",
                          json{{"slice", "who"}, {"session", b.sessionId()}}, b) ==
          json{{"i", "B"}});

    // Defaults fall through per key for any session.
    CHECK(invokePumpedCtx("state_query",
                          json{{"slice", "shared"}, {"session", b.sessionId()}}, b) ==
          json{{"d", 1}});

    // Pause means ONE session (T175.10).
    det::invokeMethodForTest("pause", json{{"session", a.sessionId()}});
    CHECK(ac::applyTimeControl(a, 0.016f) == 0.0f);
    CHECK(ac::applyTimeControl(b, 0.016f) == doctest::Approx(0.016f));

    // Ambiguity: two live sessions, none named.
    CHECK_THROWS(det::invokeMethodForTest("pause", json::object()));

    det::resetAppChannelStateForTest();
}

TEST_CASE("🎯T175.4: perf counters are per-session") {
    det::resetAppChannelStateForTest();
    ge::Context a(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    ge::Context b(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    ac::perfEmit(a, "c", 1.0);
    ac::perfEmit(b, "c", 2.0);
    CHECK(det::perfCountersSnapshotForTest(a) == json{{"c", 1.0}});
    CHECK(det::perfCountersSnapshotForTest(b) == json{{"c", 2.0}});
    det::resetAppChannelStateForTest();
}

TEST_CASE("🎯T175.2: hit_targets surface comes from the addressed session's Context; extras overlay") {
    det::resetAppChannelStateForTest();
    ge::Context a(64, 64, ge::DeviceClass::Desktop, ":memory:", "");  // 64pt surface
    ge::Context b(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    ac::setExtraHitTargets(b, json::array({{{"id", "b_only"},
                                            {"kind", "region"},
                                            {"enabled", true},
                                            {"bbox", {8, 8, 16, 16}}}}));

    ge::Button btn;
    btn.id = "shared_btn";
    btn.setHitRect(ge::Rect{16, 16, 32, 32});
    ge::publishHitTarget(&btn);

    const json qa = invokePumpedCtx(
        "state_query", json{{"slice", "hit_targets"}, {"session", a.sessionId()}}, a);
    const json qb = invokePumpedCtx(
        "state_query", json{{"slice", "hit_targets"}, {"session", b.sessionId()}}, b);

    bool aHasExtra = false, bHasExtra = false, aNormOk = false;
    for (const auto& t : qa["targets"]) {
        if (t.value("id", "") == "b_only") aHasExtra = true;
        if (t.value("id", "") == "shared_btn" && t.contains("bbox_norm"))
            aNormOk = t["bbox_norm"][0].get<double>() == doctest::Approx(16.0 / 64.0);
    }
    for (const auto& t : qb["targets"])
        if (t.value("id", "") == "b_only") bHasExtra = true;
    CHECK(!aHasExtra);  // b's extras are b's own
    CHECK(bHasExtra);
    CHECK(aNormOk);     // normalised against session a's 64pt Context surface

    ge::unpublishHitTarget(&btn);
    det::resetAppChannelStateForTest();
}

// ── 🎯T175.5/6/7/8/9 independent scoping moves ──────────────────────

TEST_CASE("🎯T175.5: button registries are per-session; defaults visible to all; dead sessions pruned") {
    det::resetAppChannelStateForTest();
    ge::Button dflt, aBtn;
    dflt.id = "dflt";
    dflt.setHitRect(ge::Rect{1, 1, 2, 2});
    aBtn.id = "a_btn";
    aBtn.setHitRect(ge::Rect{1, 1, 2, 2});

    uint32_t deadId = 0;
    {
        ge::Context a(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
        ge::Context b(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
        ge::publishHitTarget(&dflt);       // two live → defaults registry
        ge::publishHitTarget(a, &aBtn);    // session a's own
        CHECK(ge::publishedHitTargets(a.sessionId()).size() == 2);
        CHECK(ge::publishedHitTargets(b.sessionId()).size() == 1);
        deadId = a.sessionId();
    }
    // a and b died: a's registry entry is pruned; defaults remain.
    const auto after = ge::publishedHitTargets(deadId);
    CHECK(after.size() == 1);
    CHECK(after[0] == &dflt);

    ge::unpublishHitTarget(&dflt);
    ge::unpublishHitTarget(&aBtn);
    CHECK(ge::publishedHitTargets().empty());
}

TEST_CASE("🎯T175.6: per-session capture sinks; the engine binds the active session's") {
    ge::Context a(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    ge::Context b(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    ge::cmdstream::LiveCapture ca, cb;
    ge::cmdstream::setLiveCapture(a.sessionId(), &ca);
    ge::cmdstream::setLiveCapture(b.sessionId(), &cb);

    ge::cmdstream::bindActiveCapture(a.sessionId());
    CHECK(ge::cmdstream::liveCapture() == &ca);   // a's frame sees a's sink
    ge::cmdstream::bindActiveCapture(b.sessionId());
    CHECK(ge::cmdstream::liveCapture() == &cb);   // b's frame sees b's sink

    ge::cmdstream::setLiveCapture(a.sessionId(), nullptr);
    ge::cmdstream::setLiveCapture(b.sessionId(), nullptr);
    ge::cmdstream::bindActiveCapture(0);
    CHECK(ge::cmdstream::liveCapture() == nullptr);
}

TEST_CASE("🎯T175.7: sensor authority is per-session") {
    using ge::detail::SensorStreamMode;
    ge::Context a(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    ge::Context b(64, 64, ge::DeviceClass::Desktop, ":memory:", "");

    ge::detail::setAccelStreamMode(a.sessionId(), SensorStreamMode::Override);
    ge::detail::setAccelLatch(a.sessionId(), 1.f, 2.f, 3.f);

    CHECK(ge::detail::accelStreamMode(b.sessionId()) == SensorStreamMode::Passthrough);
    float x = 0, y = 0, z = 0;
    CHECK(!ge::detail::accelLatch(b.sessionId(), x, y, z));  // b has no latch
    CHECK(ge::detail::accelLatch(a.sessionId(), x, y, z));
    CHECK(x == 1.f);

    ge::detail::resetSensorControl();
    CHECK(ge::detail::accelStreamMode(a.sessionId()) == SensorStreamMode::Passthrough);
}

TEST_CASE("🎯T175.8: wire-fed viewer bit is per-session, ORed with the process bit") {
    ge::Context a(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    ge::Context b(64, 64, ge::DeviceClass::Desktop, ":memory:", "");

    ge::detail::injectViewerBackgrounded(a.sessionId(), true);
    CHECK(ge::detail::viewerBackgroundedFor(a.sessionId()));
    CHECK(!ge::detail::viewerBackgroundedFor(b.sessionId()));  // b unaffected

    ge::detail::injectViewerBackgrounded(true);  // process bit pauses all
    CHECK(ge::detail::viewerBackgroundedFor(b.sessionId()));

    ge::detail::injectViewerBackgrounded(false);
    ge::detail::injectViewerBackgrounded(a.sessionId(), false);
    CHECK(!ge::detail::viewerBackgroundedFor(a.sessionId()));
    CHECK(!ge::detail::viewerBackgroundedFor(b.sessionId()));
}

TEST_CASE("🎯T175.9: metrics scopes resolve within a session; untagged scopes stay process-wide") {
    ge::Context a(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    ge::metrics::Scope sa("t175_sa");  // sole live session → tagged a
    CHECK(sa.session() == a.sessionId());

    ge::Context b(64, 64, ge::DeviceClass::Desktop, ":memory:", "");
    ge::metrics::Scope sw("t175_wide");  // two live → untagged (process-wide)
    CHECK(sw.session() == 0);

    const auto va = ge::metrics::Scope::all(a.sessionId());
    const auto vb = ge::metrics::Scope::all(b.sessionId());
    CHECK(std::find(va.begin(), va.end(), &sa) != va.end());
    CHECK(std::find(va.begin(), va.end(), &sw) != va.end());
    CHECK(std::find(vb.begin(), vb.end(), &sa) == vb.end());  // a's scope invisible to b
    CHECK(std::find(vb.begin(), vb.end(), &sw) != vb.end());
}
