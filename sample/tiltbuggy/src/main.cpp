// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// TiltBuggy — a ge sample driving a 2D buggy with tilt gravity.
// Stage 2: Box2D physics + sokol rendering. On a real device the
// accelerometer drives gravity. On desktop/simulator/emulator
// AccelSynth synthesises SDL_EVENT_SENSOR_UPDATE events from
// Shift-gated mouse drag — hold Shift and drag to tilt the world.

#include "Renderer.h"
#include "Scene.h"

#include <ge/appchannel.h>
#include <ge/box2d_render.h>  // 🎯T131.2 renderWhileAwake
#include <ge/box2d_slice.h>
#include <ge/iap.h>
#include <ge/Protocol.h>
#include <ge/Resource.h>
#include <ge/sdl_input.h>
#include <ge/SessionHost.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // required on iOS/Android; no-op on desktop
#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

// 🎯T137.1 Restored to the 2013 ~10-unit world (was 1/16-shrunk to 0.625). The
// follow-camera (🎯T137.3) keeps the buggy on screen, so the arena no longer
// has to be tiny, and box2d v3's solver prefers human-scale bodies.
constexpr float kWorldHalfExtent = 10.0f;

// Device acceleration (≈9.8 m/s² per g) scaled into world gravity. The 2013
// game drove gravity at ~100 per g for a snappy arcade feel; this gain is the
// equivalent knob at this world scale (tuned in 🎯T16).
constexpr float kGravityGain = 4.0f;

struct State {
    std::unique_ptr<tiltbuggy::Scene> scene;
    std::unique_ptr<tiltbuggy::Renderer> renderer;
    b2Vec2 gravity{0, 0};
    bool rendererInited = false;
    bool renderOnDemand = false;        // 🎯T131.5 GE_RENDER_ON_DEMAND demo
    bool proPurchaseInFlight = false;   // debounce: drop taps while Apple modal is up
};

// 🎯T124 Apply a serialized state to the live game — shared by the app-channel
// restore and the headless render path. The static arena is rebuilt by the
// Scene ctor; only the dynamic buggy pose + gravity + entitlement round-trip.
void applyState(State& state, const nlohmann::json& j) {
    if (j.contains("gravity")) {
        state.gravity.x = j["gravity"].value("x", 0.0f);
        state.gravity.y = j["gravity"].value("y", 0.0f);
    }
    if (j.contains("buggy") && state.scene) {
        const auto& b = j["buggy"];
        state.scene->applyPose({b.value("x", 0.0f), b.value("y", 0.0f),
                                b.value("angle", 0.0f)});
    }
    ge::iap::testing::setOwned("pro", j.value("pro", false));
}

} // namespace

int main(int argc, char* argv[]) {
    // spdlog logs flow to platform-native channels via ge::log::install,
    // which ge::run calls automatically below. No per-app sink wiring
    // needed (🎯T66).
    //
    // For dev-time diagnostics that bypass Apple unified logging / logcat
    // entirely (🎯T83), stream logs over TCP — no code change here, just set
    // the LOG_TARGET=host:port convention in the launch env.
    //   Preferred (spyder v0.51.0+): log_collect_start picks a port + reports
    //     your LAN IPs; pass one as LOG_TARGET to launch_app/deploy_app, then
    //     log_collect_get. No port-picking, works on real devices.
    //   No-spyder fallback:
    //     Mac:      nc -l 9999
    //     desktop:  LOG_TARGET=127.0.0.1:9999 bin/tiltbuggy
    //     iOS sim:  SIMCTL_CHILD_LOG_TARGET=127.0.0.1:9999 xcrun simctl launch …
    //     Android:  adb reverse tcp:9999 tcp:9999;
    //               adb shell setprop debug.ge.log_target 127.0.0.1:9999
    // Debug builds only — the sink is compiled out under NDEBUG.

    bool brokered = false;     // default: direct/distribution modality
    bool renderMode = false;   // 🎯T124 headless render-to-PNG: `tiltbuggy render ...`
    bool isolate = false;      // --batch --isolate: re-exec a fresh process per fixture
    std::string stateFile, batchFile;
    std::string outFile = "frame.png";
    int renderW = 1024, renderH = 768;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--brokered") == 0) brokered = true;
        else if (std::strcmp(argv[i], "render") == 0) renderMode = true;
        else if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc) stateFile = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) outFile = argv[++i];
        else if (std::strcmp(argv[i], "--batch") == 0 && i + 1 < argc) batchFile = argv[++i];
        else if (std::strcmp(argv[i], "--isolate") == 0) isolate = true;
        else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc)
            std::sscanf(argv[++i], "%dx%d", &renderW, &renderH);
    }

    State state;

    // Register the IAP catalogue and pre-populate the entitlement cache.
    // The `pro` SKU must also be registered in App Store Connect as
    // com.squz.tiltbuggy.pro and in Play Console with the matching id.
    // T65.7 demo: once registered, sandbox / license-tester accounts
    // can buy() this from the device and `owned("pro")` will flip.
    ge::iap::setCatalogue({
        {.id = "pro",           .type = ge::iap::Type::NonConsumable},
        {.id = "powerboost10",  .type = ge::iap::Type::Consumable},
    });
    ge::iap::restore([](ge::iap::Result r) {
        SPDLOG_INFO("iap: restore complete ok={} error={}", r.ok, r.error);
    });

    // 🎯T92.5 App-channel state registry — the copyable proving-ground for
    // ge consumers. Registered BEFORE ge::run so the slice names ride in the
    // hello; the getters/serializer run on the game thread (ge marshals them).
    // Slices are read-only snapshots; the serializer round-trips the bits that
    // are cheaply settable here (gravity + the pro entitlement).
    ge::appchannel::registerStateSlice("scene", [&state] {
        const auto p = state.scene ? state.scene->buggyPose() : tiltbuggy::Pose{};
        const auto g = state.scene ? state.scene->gripState()
                                   : tiltbuggy::GripState{};
        return nlohmann::json{
            {"buggy",   {{"x", p.x}, {"y", p.y}, {"angle", p.angle}}},
            {"gravity", {{"x", state.gravity.x}, {"y", state.gravity.y}}},
            // 🎯T137.2 Per-axle grip — drops when a tread is over ice/dirt, so a
            // state-capture across an input sequence shows the surface effect.
            {"grip",    {{"front", g.front}, {"rear", g.rear}}},
        };
    });
    ge::appchannel::registerStateSlice("iap", [] {
        return nlohmann::json{{"pro", ge::iap::owned("pro")}};
    });
    // 🎯T115 "geometry" slice — the recommended geometry/physics schema: bodies
    // in world units carrying position + velocity, so spyder renders/compares
    // physics state uniformly across ge games (and an agent can diff it across
    // an input sequence). TiltBuggy has one dynamic body; a richer consumer
    // (multimaze2's marbles + walls) fills in the constraints / sensors arrays.
    ge::appchannel::registerStateSlice("geometry", [&state] {
        // 🎯T117 The whole physics world as data, read out by
        // ge::box2d::worldGeometry — no hand-rolled body formatting. It walks
        // every body/shape (the arena walls, surface patches, and the named
        // "buggy"); the app just adds the unit + bounds context box2d can't know.
        nlohmann::json geo = state.scene
            ? ge::box2d::worldGeometry(state.scene->worldId())
            : nlohmann::json{{"bodies", nlohmann::json::array()}};
        geo["units"] = "metres";
        const float e = state.scene ? state.scene->halfExtent() : 0.0f;
        geo["bounds"] = {{"min", {-e, -e}}, {"max", {e, e}}};
        return geo;
    },
    // 🎯T116 example — the slice's shape, for filter-writing without a
    // state_query round-trip. One representative named body with a shape outline
    // (worldGeometry emits one entry per body, shapes grouped under it).
    nlohmann::json{
        {"units",  "metres"},
        {"bodies", nlohmann::json::array({
            {{"id", "buggy"}, {"pos", {0.0, 0.0}}, {"vel", {0.0, 0.0}}, {"angle", 0.0},
             {"shapes", nlohmann::json::array({
                 {{"type", "polygon"},
                  {"vertices", {{-1.0, -0.5}, {1.0, -0.5}, {1.0, 0.5}, {-1.0, 0.5}}}},
             })}},
        })},
        {"bounds", {{"min", {-10.0, -10.0}}, {"max", {10.0, 10.0}}}},
    });
    ge::appchannel::registerStateSerializer(
        [&state] {
            nlohmann::json j{
                {"pro",     ge::iap::owned("pro")},
                {"gravity", {{"x", state.gravity.x}, {"y", state.gravity.y}}},
            };
            if (state.scene) {
                const auto p = state.scene->buggyPose();
                j["buggy"] = {{"x", p.x}, {"y", p.y}, {"angle", p.angle}};
            }
            return j;
        },
        [&state](const nlohmann::json& j) { applyState(state, j); });

    auto factory = [&](ge::Context ctx) -> ge::RunConfig {
        // 🎯T131.5 In render-on-demand mode the buggy is allowed to sleep so a
        // settled scene lets the loop idle (onEvent wakes it on a real tilt).
        state.scene = std::make_unique<tiltbuggy::Scene>(kWorldHalfExtent,
                                                         state.renderOnDemand);
        state.renderer = std::make_unique<tiltbuggy::Renderer>();
        state.rendererInited = false;

        // 🎯T131.5 Render-on-demand demo (opt-in via GE_RENDER_ON_DEMAND). The
        // buggy is a physics body: render every frame while it's moving (box2d
        // island-awake trigger), idle once the whole world sleeps. The render-
        // liveness diagnostic spin changes the frame every tick, so it would
        // defeat idle — turn it off in this mode.
        if (state.renderOnDemand) {
            ctx.setContinuousRendering(false);
            ge::box2d::renderWhileAwake(ctx, state.scene->worldId());
            state.renderer->setDiagnosticSpin(false);
            SPDLOG_INFO("tiltbuggy: render-on-demand ON (box2d-awake trigger)");
        }

        return {
            .onUpdate = [&](float dt) {
                state.scene->step(dt, state.gravity);
                auto p = state.scene->buggyPose();
                // 🎯T92.4 Sample app-channel perf counter — the copyable
                // proving-ground pattern for ge consumers. Surfaces in
                // spyder's app_perf_get alongside the engine's frame_ms.
                ge::appchannel::perfEmit("buggy_x", p.x);
                static int frame = 0;
                if (++frame % 60 == 0) {
                    const auto gr = state.scene->gripState();
                    SPDLOG_INFO("tick: dt={:.4f} g=[{:.2f},{:.2f}] pose=[{:.2f},{:.2f},{:.2f}] grip=[{:.2f},{:.2f}] pro={}",
                                dt, state.gravity.x, state.gravity.y, p.x, p.y, p.angle,
                                gr.front, gr.rear, ge::iap::owned("pro"));
                }
            },
            .onRender = [&](const ge::Context& c) {
                if (!state.rendererInited) {
                    state.renderer->init(ge::resource(ge::shaderDir()).c_str());
                    state.rendererInited = true;
                }
                state.renderer->drawFrame(*state.scene, c);
                // 🎯T131.5 Push the present counter so a spyder matrix cell can
                // assert it goes flat when the buggy settles (idle) and resumes
                // on tilt. Pushed from onRender, so it only advances on a drawn
                // frame — exactly the signal we want — and reads as stale (flat)
                // to app_perf_get while the loop idles.
                ge::appchannel::perfEmit("frames_presented", double(c.framesPresented()));
            },
            .onEvent = [&, ctx](const SDL_Event& e) {
                SPDLOG_INFO("onEvent type=0x{:x}", e.type);
                if (e.type == SDL_EVENT_SENSOR_UPDATE) {
                    // Engine delivers device acceleration in screen frame.
                    // The world/board accelerates in that direction, so the
                    // buggy (free on the board) experiences gravity in the
                    // opposite direction — hence the negation.
                    const b2Vec2 oldG = state.gravity;
                    state.gravity.x = -e.sensor.data[0] * kGravityGain;
                    state.gravity.y = -e.sensor.data[1] * kGravityGain;
                    // 🎯T131.5/T134 The continuous sensor stream doesn't wake the
                    // idle loop on its own, but a *meaningful tilt change* must:
                    // otherwise a tilt while the buggy is asleep would update
                    // gravity and never step it (no frame runs), so the buggy
                    // would never start moving. Threshold filters accelerometer
                    // noise so a still device still idles; the box2d-awake trigger
                    // then keeps rendering until the buggy settles again.
                    if (state.renderOnDemand) {
                        const float dx = state.gravity.x - oldG.x;
                        const float dy = state.gravity.y - oldG.y;
                        if (dx * dx + dy * dy > 0.04f) {
                            state.scene->wakeBuggy();  // re-apply gravity to a settled buggy
                            ctx.requestRedraw();       // and draw the frame that steps it
                        }
                    }
                    SPDLOG_INFO("ACCEL accel=[{:+.2f},{:+.2f},{:+.2f}] gravity=[{:+.2f},{:+.2f}]",
                                e.sensor.data[0], e.sensor.data[1], e.sensor.data[2],
                                state.gravity.x, state.gravity.y);
                    return;
                }

                // BUY PRO button tap (🎯T65.7): convert SDL pointer events
                // to ge::PointerEvent in pt space (🎯T60), then hit-test
                // against the same screen rect Renderer draws at.
                auto pe = ge::input::fromSdl(e, ctx.fullRectInPts().size());
                if (pe && pe->kind == ge::PointerEvent::Down
                       && !ge::iap::owned("pro")
                       && !state.proPurchaseInFlight
                       && tiltbuggy::proButtonRect(ctx).contains(pe->pos)) {
                    state.proPurchaseInFlight = true;
                    SPDLOG_INFO("iap: tapping BUY PRO");
                    ge::iap::buy("pro", [&state](ge::iap::Result r) {
                        state.proPurchaseInFlight = false;
                        SPDLOG_INFO("iap: buy pro complete ok={} error={}", r.ok, r.error);
                    });
                }
            },
            .onShutdown = [&] {
                state.scene.reset();
                state.renderer.reset();
                SPDLOG_INFO("TiltBuggy shutdown");
            },
        };
    };

    // 🎯T124 Headless render mode: `tiltbuggy render --out <png> [--state <f>] [--size WxH]`.
    // Renders one frame of the given state to a PNG with no window, no ged — the
    // hermetic primitive an agent uses to eyeball a state after a code change.
    if (renderMode) {
        // 🎯T124 batch: `render --batch <manifest.json>`, manifest =
        // [{"state": "<file>", "out": "<png>"}, ...]. One process, one host.
        if (!batchFile.empty()) {
            std::ifstream bin(batchFile);
            if (!bin) { SPDLOG_ERROR("render: cannot open batch manifest '{}'", batchFile); return 1; }
            nlohmann::json manifest;
            try { bin >> manifest; }
            catch (const std::exception& e) { SPDLOG_ERROR("render: bad manifest: {}", e.what()); return 1; }

            // --isolate: re-exec a fresh process per fixture (hard-crash safe).
            if (isolate) {
                int fails = 0;
                for (const auto& e : manifest) {
                    const std::string sf = e.value("state", std::string());
                    const std::string of = e.value("out", std::string());
                    std::string cmd = std::string(argv[0]) + " render --out '" + of + "'"
                        + (sf.empty() ? "" : " --state '" + sf + "'")
                        + " --size " + std::to_string(renderW) + "x" + std::to_string(renderH);
                    if (std::system(cmd.c_str()) != 0) { ++fails; SPDLOG_ERROR("isolate: failed {}", of); }
                }
                return fails == 0 ? 0 : 1;
            }

            // Default: one host, every fixture in-process (amortised startup).
            std::vector<ge::RenderItem> items;
            for (const auto& e : manifest) {
                const std::string of = e.value("out", std::string());
                nlohmann::json sj;
                if (e.contains("state")) {
                    std::ifstream sin(e["state"].get<std::string>());
                    if (sin) { try { sin >> sj; } catch (...) {} }
                }
                items.push_back({[&state, sj] {
                    if (state.renderer) state.renderer->setDiagnosticSpin(false);
                    if (!sj.is_null()) applyState(state, sj);
                }, of});
            }
            const int ok = ge::renderBatch(factory,
                {.width = renderW, .height = renderH, .appName = "tiltbuggy"}, items);
            SPDLOG_INFO("batch: {}/{} rendered", ok, static_cast<int>(items.size()));
            return ok == static_cast<int>(items.size()) ? 0 : 1;
        }

        nlohmann::json stateJson;
        if (!stateFile.empty()) {
            std::ifstream in(stateFile);
            if (!in) { SPDLOG_ERROR("render: cannot open state file '{}'", stateFile); return 1; }
            try { in >> stateJson; }
            catch (const std::exception& e) {
                SPDLOG_ERROR("render: bad state JSON in '{}': {}", stateFile, e.what());
                return 1;
            }
        }
        auto prepare = [&] {
            if (state.renderer) state.renderer->setDiagnosticSpin(false);  // determinism
            if (!stateJson.is_null()) applyState(state, stateJson);
        };
        const bool ok = ge::renderToPng(factory,
            {.width = renderW, .height = renderH, .appName = "tiltbuggy"},
            prepare, outFile);
        return ok ? 0 : 1;
    }

    // 🎯T131.5 Opt into the render-on-demand demo for the live run only — the
    // headless render path above must stay continuous/deterministic.
    state.renderOnDemand = std::getenv("GE_RENDER_ON_DEMAND") != nullptr;

    ge::run(factory, {
        .width = brokered ? 0 : 1024,
        .height = brokered ? 0 : 768,
        .headless = brokered,
        .appName = "tiltbuggy",
        .sensors = wire::kSensorAccelerometer,
        .orientation = wire::kOrientationAnyLandscape,
        .disableScreenSaver = true,
        .immersive = true,
    });
    return 0;
}
