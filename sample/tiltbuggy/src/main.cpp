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
