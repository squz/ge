// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// TiltBuggy — a ge sample driving a 2D buggy with tilt gravity.
// Stage 2: Box2D physics + bgfx rendering. On a real device the
// accelerometer drives gravity. On desktop/simulator/emulator
// AccelSynth synthesises SDL_EVENT_SENSOR_UPDATE events from
// Shift-gated mouse drag — hold Shift and drag to tilt the world.

#include "Renderer.h"
#include "Scene.h"

#include <ge/appchannel.h>
#include <ge/iap.h>
#include <ge/Protocol.h>
#include <ge/Resource.h>
#include <ge/sdl_input.h>
#include <ge/SessionHost.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>  // required on iOS/Android; no-op on desktop
#include <spdlog/spdlog.h>

#include <cstring>
#include <memory>

namespace {

constexpr float kWorldHalfExtent = 0.625f;

struct State {
    std::unique_ptr<tiltbuggy::Scene> scene;
    std::unique_ptr<tiltbuggy::Renderer> renderer;
    b2Vec2 gravity{0, 0};
    bool rendererInited = false;
    bool proPurchaseInFlight = false;   // debounce: drop taps while Apple modal is up
};

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

    bool brokered = false;  // default: direct/distribution modality
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--brokered") == 0) brokered = true;
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
        return nlohmann::json{
            {"buggy",   {{"x", p.x}, {"y", p.y}, {"angle", p.angle}}},
            {"gravity", {{"x", state.gravity.x}, {"y", state.gravity.y}}},
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
        nlohmann::json bodies = nlohmann::json::array();
        if (state.scene) {
            const auto p = state.scene->buggyPose();
            const auto v = state.scene->buggyVelocity();
            bodies.push_back({
                {"id",    "buggy"},
                {"pos",   {p.x, p.y}},
                {"vel",   {v.x, v.y}},
                {"angle", p.angle},
            });
        }
        const float e = state.scene ? state.scene->halfExtent() : 0.0f;
        return nlohmann::json{
            {"units",  "metres"},
            {"bodies", std::move(bodies)},
            {"bounds", {{"min", {-e, -e}}, {"max", {e, e}}}},
        };
    },
    // 🎯T116 A representative example payload — the slice's shape (one body),
    // advertised in the hello so a connected agent can write a jq filter (e.g.
    // `.bodies[0].vel`) without a state_query round-trip. One-line snapshot of
    // the shape, not live data.
    nlohmann::json{
        {"units",  "metres"},
        {"bodies", nlohmann::json::array({
            {{"id", "buggy"}, {"pos", {0.0, 0.0}}, {"vel", {0.0, 0.0}}, {"angle", 0.0}},
        })},
        {"bounds", {{"min", {-0.625, -0.625}}, {"max", {0.625, 0.625}}}},
    });
    ge::appchannel::registerStateSerializer(
        [&state] {
            return nlohmann::json{
                {"pro",     ge::iap::owned("pro")},
                {"gravity", {{"x", state.gravity.x}, {"y", state.gravity.y}}},
            };
        },
        [&state](const nlohmann::json& j) {
            if (j.contains("gravity")) {
                state.gravity.x = j["gravity"].value("x", 0.0f);
                state.gravity.y = j["gravity"].value("y", 0.0f);
            }
            ge::iap::testing::setOwned("pro", j.value("pro", false));
        });

    ge::run([&](ge::Context ctx) -> ge::RunConfig {
        state.scene = std::make_unique<tiltbuggy::Scene>(kWorldHalfExtent);
        state.renderer = std::make_unique<tiltbuggy::Renderer>();
        state.rendererInited = false;

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
                    SPDLOG_INFO("tick: dt={:.4f} g=[{:.2f},{:.2f}] pose=[{:.2f},{:.2f},{:.2f}] pro={}",
                                dt, state.gravity.x, state.gravity.y, p.x, p.y, p.angle,
                                ge::iap::owned("pro"));
                }
            },
            .onRender = [&](const ge::Context& c) {
                if (!state.rendererInited) {
                    state.renderer->init(ge::resource(ge::shaderDir()).c_str());
                    state.rendererInited = true;
                }
                state.renderer->drawFrame(*state.scene, c);
            },
            .onEvent = [&, ctx](const SDL_Event& e) {
                SPDLOG_INFO("onEvent type=0x{:x}", e.type);
                if (e.type == SDL_EVENT_SENSOR_UPDATE) {
                    // Engine delivers device acceleration in screen frame.
                    // The world/board accelerates in that direction, so the
                    // buggy (free on the board) experiences gravity in the
                    // opposite direction — hence the negation.
                    state.gravity.x = -e.sensor.data[0];
                    state.gravity.y = -e.sensor.data[1];
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
    }, {
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
