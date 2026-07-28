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

#include <ge/CmdStream.h>
#include <ge/appchannel.h>
#include <ge/button.h>

#include <doctest.h>

#include <nlohmann/json.hpp>

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
