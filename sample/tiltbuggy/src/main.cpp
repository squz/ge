// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// TiltBuggy — a ge sample, an exact-match port of the 2013 Chipmunk/iOS
// TiltBuggy (🎯T137): a tiled-asphalt arena with ice + dirt patches and the
// buggy, driven by device tilt. Tilt the device (or, on desktop/sim/emu, hold
// Shift and drag — AccelSynth) and the buggy drives across the arena; ice and
// dirt rob traction. No title, no buttons, no recolour — the screen matches the
// original.
//
// CORE GAME: tilt→gravity, the vehicle-dynamics model (Scene.cpp), the surface
// traps, the camera, and the original textures (Renderer.cpp).
//
// ge ENGINE SHOWCASES remain wired but INVISIBLE / off-by-default (the visible
// ones — BUY PRO, title, recolour, SVG decals — were stripped for parity;
// reintroduce later, 🎯T137.6): app-channel state slices + perf counters
// (🎯T92/T115), GE_RENDER_ON_DEMAND idle (🎯T131/T132), GE_DEBUG_OVERLAY
// (🎯T97), the headless `render` verb + goldens (🎯T124), presentation-tilt on
// synth platforms (🎯T94), and the dormant ge::iap catalogue (🎯T65.7).

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
#include <sqlpipe.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // required on iOS/Android; no-op on desktop
#include <spdlog/spdlog.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>  // TARGET_OS_IOS — guards the host-only render path
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

// 🎯T92.1 GE_FACTORY mode (below) forks game instances via posix_spawn.
#include <spawn.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <vector>
extern char** environ;

namespace {

// 🎯T137.1 Restored to the 2013 ~10-unit world (was 1/16-shrunk to 0.625). The
// follow-camera (🎯T137.3) keeps the buggy on screen, so the arena no longer
// has to be tiny, and box2d v3's solver prefers human-scale bodies.
constexpr float kWorldHalfExtent = 10.0f;

// Device acceleration (≈9.8 m/s² per g) scaled into world gravity. The 2013
// game drove gravity at ~100 per g for a snappy arcade feel; this gain is the
// equivalent knob at this world scale (tuned in 🎯T16).
constexpr float kGravityGain = 10.0f;
// A device standing fully upright puts ~1 g on one axis → |gravity| ≈ 98
// world units/s² against a ~20-unit world: the buggy tunnels through the
// walls and escapes (seen live on the A9 tablet). Clamp tilt gravity to a
// playable ceiling, and teleport an escaped buggy back to spawn — the
// latter also self-heals stale SP2T poses seeded from a glass's old db.
constexpr float kMaxTiltGravity = 30.0f;
constexpr float kEscapeBound = kWorldHalfExtent * 8.0f;

// 🎯T154 SP2T pose durability test: single-row pose table written every 0.5s.
// Schema is applied by DirectRenderHost via SessionHostConfig.schemaDdl.
constexpr const char* kPoseSchemaDdl =
    "CREATE TABLE pose ("
    "  id INTEGER PRIMARY KEY CHECK (id = 1),"
    "  x REAL NOT NULL,"
    "  y REAL NOT NULL,"
    "  angle REAL NOT NULL,"
    "  updated_at REAL NOT NULL"
    ");";

constexpr float kPoseWriteIntervalSec = 0.5f;

struct State {
    std::unique_ptr<tiltbuggy::Scene> scene;
    std::unique_ptr<tiltbuggy::Renderer> renderer;
    b2Vec2 gravity{0, 0};
    bool rendererInited = false;
    bool renderOnDemand = false;        // 🎯T131.5 GE_RENDER_ON_DEMAND demo
    float arenaAspect = 0.f;            // rebuild scene when content aspect changes
    float poseWriteAccum = 0.f;         // 🎯T154 durable pose write cadence
    std::shared_ptr<sqlpipe::Database> db;  // Context::db() for pose persistence
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

    bool renderMode = false;   // 🎯T124 headless render-to-PNG: `tiltbuggy render ...`
    bool isolate = false;      // --batch --isolate: re-exec a fresh process per fixture
    std::string stateFile, batchFile;
    std::string outFile = "frame.png";
    int renderW = 1024, renderH = 768;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "render") == 0) renderMode = true;
        else if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc) stateFile = argv[++i];
        else if (std::strcmp(argv[i], "--out") == 0 && i + 1 < argc) outFile = argv[++i];
        else if (std::strcmp(argv[i], "--batch") == 0 && i + 1 < argc) batchFile = argv[++i];
        else if (std::strcmp(argv[i], "--isolate") == 0) isolate = true;
        else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc)
            std::sscanf(argv[++i], "%dx%d", &renderW, &renderH);
    }

    // 🎯T92.1 GE_FACTORY mode — act as a game-server "device factory". Register
    // spawn_instance so spyder can fork game instances on demand; each instance
    // is this same binary re-exec'd (GE_FACTORY stripped) pointed at the given
    // app-channel, so it dials spyder back as its own session with the full
    // monitor surface. No game window in this mode — the app-channel worker
    // threads service requests while main() idles. Dev-only (installFromEnv and
    // the whole channel compile out under NDEBUG).
#if !defined(__ANDROID__)
    // Desktop dev harness only: posix_spawn is API 28+ on bionic, and a
    // phone game has no business forking instances of itself.
    if (std::getenv("GE_FACTORY")) {
        const std::string self = argv[0];
        ge::appchannel::registerMethod(
            "spawn_instance", [self](const nlohmann::json& p) -> nlohmann::json {
                const std::string chan = p.value("app_channel", std::string{});
                if (chan.empty())
                    throw ge::appchannel::Error{-32602, "spawn_instance: app_channel is required"};
                // Child env: inherit ours, drop GE_FACTORY, point at the instance channel.
                std::vector<std::string> envs;
                for (char** e = environ; e && *e; ++e) {
                    std::string s = *e;
                    if (s.rfind("GE_FACTORY=", 0) == 0) continue;
                    if (s.rfind("SPYDER_APP_CHANNEL=", 0) == 0) continue;
                    envs.push_back(std::move(s));
                }
                envs.push_back("SPYDER_APP_CHANNEL=" + chan);
                std::vector<char*> envp;
                for (auto& s : envs) envp.push_back(s.data());
                envp.push_back(nullptr);
                char* argvv[] = {const_cast<char*>(self.c_str()), nullptr};
                pid_t pid = 0;
                int rc = posix_spawn(&pid, self.c_str(), nullptr, nullptr, argvv, envp.data());
                if (rc != 0)
                    throw ge::appchannel::Error{-32000,
                                                std::string("spawn failed: ") + std::strerror(rc)};
                SPDLOG_INFO("tiltbuggy factory: spawned instance pid={} game={}", pid,
                            p.value("game", std::string{}));
                return nlohmann::json{{"ok", true}, {"pid", pid}, {"game", p.value("game", std::string{})}};
            });
        ge::appchannel::installFromEnv("tiltbuggy-factory", "dev");
        SPDLOG_INFO("tiltbuggy GE_FACTORY: spawn_instance registered; idling");
        for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    }
#endif // !__ANDROID__

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
        // 🎯T137.3 Aspect-scale the arena to drawSafe (the playfield edge). Under
        // immersive that is the full surface; dirt/walls reach the short edge
        // without a separate fullRect path. Rebuild if drawSafe aspect changes
        // (stream content retarget, chrome, …).
        const auto play0 = ctx.drawSafeRectInPts();
        const auto full0 = ctx.fullRectInPts();
        const ge::Rect aspectSrc = (play0.h > 0.f) ? play0 : full0;
        state.arenaAspect = (aspectSrc.h > 0.f)
            ? aspectSrc.w / aspectSrc.h : 1.0f;
        state.scene = std::make_unique<tiltbuggy::Scene>(kWorldHalfExtent,
                                                         state.arenaAspect,
                                                         state.renderOnDemand);
        state.renderer = std::make_unique<tiltbuggy::Renderer>();
        state.rendererInited = false;
        state.db = ctx.db();
        state.poseWriteAccum = 0.f;

        // 🎯T154: restore last durable pose if present (stream reconnect / direct relaunch).
        if (state.db) {
            try {
                auto rows = state.db->query(
                    "SELECT x, y, angle FROM pose WHERE id = 1");
                if (!rows.rows.empty() && rows.rows[0].size() >= 3) {
                    auto asF = [](const sqlpipe::Value& v) -> float {
                        if (const auto* d = std::get_if<double>(&v))
                            return static_cast<float>(*d);
                        if (const auto* i = std::get_if<std::int64_t>(&v))
                            return static_cast<float>(*i);
                        return 0.f;
                    };
                    const float x = asF(rows.rows[0][0]);
                    const float y = asF(rows.rows[0][1]);
                    const float a = asF(rows.rows[0][2]);
                    state.scene->applyPose({x, y, a});
                    SPDLOG_INFO("tiltbuggy: restored pose from db "
                                "[{:.2f},{:.2f},{:.2f}]", x, y, a);
                }
            } catch (const std::exception& e) {
                SPDLOG_WARN("tiltbuggy: pose restore skipped: {}", e.what());
            }
        }

        // 🎯T131.5 Render-on-demand demo (opt-in via GE_RENDER_ON_DEMAND). The
        // buggy is a physics body: render every frame while it's moving (box2d
        // island-awake trigger), idle once the whole world sleeps.
        if (state.renderOnDemand) {
            ctx.setContinuousRendering(false);
            ge::box2d::renderWhileAwake(ctx, state.scene->worldId());
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

                // 🎯T154 SP2T test: durable pose every 0.5s. Under stream the
                // server working set is :memory:; ServerSession pushes SP2T
                // to the player PrefPath file (per-game) on the same cadence.
                state.poseWriteAccum += dt;
                if (state.db && state.poseWriteAccum >= kPoseWriteIntervalSec) {
                    state.poseWriteAccum = 0.f;
                    const double t = SDL_GetTicks() / 1000.0;
                    char sql[256];
                    std::snprintf(sql, sizeof(sql),
                        "INSERT OR REPLACE INTO pose(id,x,y,angle,updated_at) "
                        "VALUES(1,%.9g,%.9g,%.9g,%.9g)",
                        static_cast<double>(p.x), static_cast<double>(p.y),
                        static_cast<double>(p.angle), t);
                    try {
                        state.db->exec(sql);
                        SPDLOG_INFO("tiltbuggy: pose → db "
                                    "[{:.2f},{:.2f},{:.2f}] t={:.1f}",
                                    p.x, p.y, p.angle, t);
                    } catch (const std::exception& e) {
                        SPDLOG_WARN("tiltbuggy: pose write failed: {}", e.what());
                    }
                }

                if (std::fabs(p.x) > kEscapeBound ||
                    std::fabs(p.y) > kEscapeBound) {
                    SPDLOG_WARN("tiltbuggy: buggy escaped world at "
                                "[{:.1f},{:.1f}] — respawning", p.x, p.y);
                    state.scene->applyPose({0.0f, 0.0f, 0.0f});
                }

                static int frame = 0;
                if (++frame % 60 == 0) {
                    const auto gr = state.scene->gripState();
                    SPDLOG_INFO("tick: dt={:.4f} g=[{:.2f},{:.2f}] pose=[{:.2f},{:.2f},{:.2f}] grip=[{:.0f},{:.0f}] pro={}",
                                dt, state.gravity.x, state.gravity.y, p.x, p.y, p.angle,
                                gr.front, gr.rear, ge::iap::owned("pro"));
                }
            },
            .onRender = [&](const ge::Context& c) {
                // Rebuild walls when drawSafe aspect changes (content retarget
                // under stream, chrome settle, …). Same source as the camera.
                {
                    const auto play = c.drawSafeRectInPts();
                    const auto full = c.fullRectInPts();
                    const ge::Rect src = (play.h > 0.f) ? play : full;
                    if (src.h > 0.f) {
                        const float a = src.w / src.h;
                        if (std::fabs(a - state.arenaAspect) >= 0.005f) {
                            const auto pose = state.scene->buggyPose();
                            state.arenaAspect = a;
                            state.scene = std::make_unique<tiltbuggy::Scene>(
                                kWorldHalfExtent, state.arenaAspect,
                                state.renderOnDemand);
                            state.scene->applyPose(pose);
                            if (state.renderOnDemand)
                                ge::box2d::renderWhileAwake(c, state.scene->worldId());
                            SPDLOG_INFO("tiltbuggy: arena aspect → {:.3f} "
                                        "(drawSafe {}x{})", a, src.w, src.h);
                        }
                    }
                }
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
                    const float gm = std::sqrt(state.gravity.x * state.gravity.x +
                                               state.gravity.y * state.gravity.y);
                    if (gm > kMaxTiltGravity) {
                        state.gravity.x *= kMaxTiltGravity / gm;
                        state.gravity.y *= kMaxTiltGravity / gm;
                    }
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
                // (The IAP buy-button is an engine showcase, not part of the
                // 2013 game — reintroduce later, 🎯T137.6. The catalogue stays
                // registered below so ge::iap is exercised, just not drawn.)
            },
            .onShutdown = [&] {
                state.scene.reset();
                state.renderer.reset();
                SPDLOG_INFO("TiltBuggy shutdown");
            },
        };
    };

    // 🎯T124 Headless render mode: `tiltbuggy render --out <png> [--state <f>] [--size WxH]`.
    // Renders one frame of the given state to a PNG with no window, no stream broker — the
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
            // Host-only: std::system is unavailable on iOS (sandbox), and the
            // render verb is a desktop golden-generation tool anyway.
            if (isolate) {
#if defined(__APPLE__) && TARGET_OS_IOS
                SPDLOG_ERROR("render --isolate is not available on iOS");
                return 1;
#else
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
#endif
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
                items.push_back({[&state, sj] {                    if (!sj.is_null()) applyState(state, sj);
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
        auto prepare = [&] {            if (!stateJson.is_null()) applyState(state, stateJson);
        };
        const bool ok = ge::renderToPng(factory,
            {.width = renderW, .height = renderH, .appName = "tiltbuggy"},
            prepare, outFile);
        return ok ? 0 : 1;
    }

    // 🎯T131.5 Opt into the render-on-demand demo for the live run only — the
    // headless render path above must stay continuous/deterministic.
    state.renderOnDemand = std::getenv("GE_RENDER_ON_DEMAND") != nullptr;

    // 🎯T92.2.2 Mode-agnostic: the app calls ge::run() identically in every
    // build. A server build (GE_SERVER_BUILD) makes ge::run a hidden-window
    // streaming host that dials spyder's relay (address from the GE_SERVER env);
    // a desktop/mobile build makes it windowed. The app neither knows nor cares —
    // no server fields, no env parsing here.
    ge::run(factory, {
        .width = 1024,
        .height = 768,
        .orgName = "squz",
        .appName = "tiltbuggy",
        .schemaDdl = kPoseSchemaDdl,
        .sensors = wire::kSensorAccelerometer,
        .orientation = wire::kOrientationAnyLandscape,
        .disableScreenSaver = true,
        .immersive = true,
    });
    return 0;
}
