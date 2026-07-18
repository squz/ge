// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// T38: sokol_gfx port. Renders straight into the swap chain via
// SokolContext::beginFrame / endFrame; no offscreen FB, no compose pass.
//
// The pre-T38 implementation kept a tilted-quad compose pass to feed the
// H.264 encoder in headless mode. We're not porting the encoder in this
// pass — the engine now draws one pass per frame and the device's own
// display is the only output target.

#include "DirectRenderHost.h"
#include "AccelSynth.h"
#include <ge/WireSdlEvent.h>
#include "LifecycleInject.h"
#include "ScreenshotBridge.h"

#include "../Attitude.h"
#include "../CutoutInsets.h"

#include <ge/AccelScreen.h>
#include <ge/audio.h>
#include <ge/FileIO.h>
#include <ge/Protocol.h>
#include <ge/RefreshRateBoost.h>
#include <ge/Resource.h>
#include <ge/Signal.h>
#include <ge/SokolContext.h>
#include <ge/StreamHostPolicy.h>
#include <ge/Tweak.h>
#include <ge/ViewerMetrics.h>
#include <ge/InputScript.h>

#include <chrono>
#include <fstream>

#include "RenderOnDemand.h"

#include "../../tools/player_orientation.h"

#include <algorithm>
#include <cmath>

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#elif TARGET_OS_OSX
#import <AppKit/AppKit.h>
#endif
#endif

#if defined(__APPLE__) && TARGET_OS_IOS
namespace ge::audio {
// Defined in audio_apple.mm — install/remove AVAudioSession notification
// observers for interruption (call, alarm) and route-change (headphone unplug).
void installAppleAudioObservers();
void removeAppleAudioObservers();
} // namespace ge::audio
#endif

#if defined(__ANDROID__)
#include <jni.h>
#endif

#include <cstdio>
#include <cstring>
#include <optional>

namespace ge {

// Reference short-side mm for deviceUiScale: iPhone Pro Max class.
// sqrt(thisDevice / reference) is the form-factor multiplier.
constexpr float kReferenceShortSideMm = 65.0f;

// Apple's pt is calibrated to viewing distance, not to a fixed
// physical mm: 1pt ≈ 1/163" on iPhone, ≈ 1/132" on iPad. Same
// convention is used here for Android phone vs tablet — close enough
// in the absence of an OS-provided physical mm query.
float ptToMm(DeviceClass dc) {
    constexpr float inchToMm = 25.4f;
    if (dc == DeviceClass::Tablet)  return inchToMm / 132.0f;  // ≈ 0.192
    return                                 inchToMm / 163.0f;  // ≈ 0.156 (Phone, default)
}

// Form-factor multiplier: sqrt of short-side mm / reference. Returns
// 1.0 on desktop (no phone/tablet form-factor distinction; the user
// controls window size).
float computeDeviceUiScale(DeviceClass dc, int surfaceW, int surfaceH, float pixelsPerPt) {
    if (dc == DeviceClass::Desktop || pixelsPerPt <= 0.0f) return 1.0f;
    const float shortSidePt = float(std::min(surfaceW, surfaceH)) / pixelsPerPt;
    const float shortSideMm = shortSidePt * ptToMm(dc);
    return std::sqrt(shortSideMm / kReferenceShortSideMm);
}

// 🎯T44 / 🎯T45 / 🎯T43 OS-event plumbing.
// The platform fires these on the wrong thread (Android UI thread for
// JNI; iOS notification queue for memory warnings). We can't safely
// touch the game's state from there, so each event sets an atomic
// flag that pumpEvents drains on the game thread before the next
// onUpdate / onRender pair.
//
// kBackHandlerActive answers Java's predictive-back "consumed?"
// query synchronously: if a handler is registered, we own the
// gesture; if not, the OS default (Activity.finish) fires.
std::atomic<bool> g_backHandlerActive{false};
std::atomic<bool> g_pendingBackPressed{false};
// -1 = no event; 0/1/2 = MemoryPressureLevel::{Low, Moderate, Critical}.
std::atomic<int>  g_pendingMemoryWarning{-1};
// 🎯T43 / 🎯T154 audio focus (Android host or stream viewer SP2L).
// +1 gained, -1 lost; pumpEvents drains on game thread.
std::atomic<int> g_pendingAudioFocusChange{0};

#if defined(__ANDROID__)
// Android's TRIM_MEMORY_* constants. Mirror SDK values rather than
// pulling in JNI just to read the public enum — these are part of
// android.content.ComponentCallbacks2's stable API.
constexpr int kTrimMemoryRunningModerate = 5;
constexpr int kTrimMemoryRunningLow      = 10;
constexpr int kTrimMemoryRunningCritical = 15;
constexpr int kTrimMemoryUiHidden        = 20;
constexpr int kTrimMemoryBackground      = 40;
constexpr int kTrimMemoryModerate        = 60;
constexpr int kTrimMemoryComplete        = 80;

int mapAndroidTrimLevel(int level) {
    switch (level) {
    case kTrimMemoryRunningModerate:
        return int(MemoryPressureLevel::Low);
    case kTrimMemoryRunningLow:
    case kTrimMemoryUiHidden:
    case kTrimMemoryBackground:
    case kTrimMemoryModerate:
        return int(MemoryPressureLevel::Moderate);
    case kTrimMemoryRunningCritical:
    case kTrimMemoryComplete:
        return int(MemoryPressureLevel::Critical);
    default:
        return -1;  // unknown level — drop on the floor
    }
}
#endif

// 🎯T156.2 Local virtual-device description: touch-first platforms arm
// AccelSynth on primary drag (no reliable modifier keys). This describes
// the DEVICE the direct build runs on — it is not a stream/direct branch.
#if (defined(__APPLE__) && TARGET_OS_IOS) || defined(__ANDROID__)
constexpr bool kLocalDeviceTouchFirstDefault = true;
#else
constexpr bool kLocalDeviceTouchFirstDefault = false;
#endif
// GE_DEVICE_TOUCH_FIRST overrides the local virtual-device description for
// the parity oracle (declared device facts, like the player's
// --device-class) — it does not select a modality.
inline bool localDeviceTouchFirst() {
    if (const char* v = std::getenv("GE_DEVICE_TOUCH_FIRST"))
        return v[0] == '1';
    return kLocalDeviceTouchFirstDefault;
}

// 🎯T154 viewer-background atomic (wire SP2L); also used by paused().
std::atomic<bool> g_viewerBackgrounded{false};

// 🎯T92.6 Screenshot bridge state.
struct ScreenshotRequest {
    std::vector<std::uint8_t>* rgba = nullptr;
    int* w = nullptr;
    int* h = nullptr;
    std::promise<bool> done;
};
std::mutex        g_ssMu;
ScreenshotRequest* g_ssReq = nullptr;
std::atomic<bool>  g_ssArmed{false};

// 🎯T92.2 / 🎯T154 injection — same atomics OS paths use; pumpEvents drains.
namespace detail {
void injectMemoryWarning(MemoryPressureLevel level) {
    g_pendingMemoryWarning.store(int(level));
}
void injectBackPressed() { g_pendingBackPressed.store(true); }
void injectAudioFocus(bool gained) {
    g_pendingAudioFocusChange.store(gained ? 1 : -1);
}
void injectViewerBackgrounded(bool backgrounded) {
    g_viewerBackgrounded.store(backgrounded, std::memory_order_release);
}
bool viewerBackgrounded() {
    return g_viewerBackgrounded.load(std::memory_order_acquire);
}

bool screenshotArmed() { return g_ssArmed.load(); }

void deliverScreenshot(const std::uint8_t* rgba, int w, int h) {
    std::lock_guard<std::mutex> lk(g_ssMu);
    if (!g_ssReq) return;
    if (rgba && w > 0 && h > 0) {
        g_ssReq->rgba->assign(rgba, rgba + static_cast<std::size_t>(w) * h * 4);
        *g_ssReq->w = w;
        *g_ssReq->h = h;
        g_ssReq->done.set_value(true);
    } else {
        g_ssReq->done.set_value(false);
    }
    g_ssReq = nullptr;
    g_ssArmed.store(false);
}

bool captureFrameRGBA(std::vector<std::uint8_t>& rgba, int& w, int& h) {
    ScreenshotRequest req;
    req.rgba = &rgba;
    req.w = &w;
    req.h = &h;
    auto fut = req.done.get_future();
    {
        std::lock_guard<std::mutex> lk(g_ssMu);
        if (g_ssReq) return false;
        g_ssReq = &req;
    }
    g_ssArmed.store(true);
    if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
        std::lock_guard<std::mutex> lk(g_ssMu);
        g_ssReq = nullptr;
        g_ssArmed.store(false);
        return false;
    }
    return fut.get();
}

bool captureFrameRGBASync(const std::function<void()>& renderOneFrame,
                          std::vector<std::uint8_t>& rgba, int& w, int& h) {
    ScreenshotRequest req;
    req.rgba = &rgba;
    req.w    = &w;
    req.h    = &h;
    auto fut = req.done.get_future();
    {
        std::lock_guard<std::mutex> lk(g_ssMu);
        if (g_ssReq) return false;
        g_ssReq = &req;
    }
    g_ssArmed.store(true);
    renderOneFrame();
    if (fut.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        return fut.get();
    std::lock_guard<std::mutex> lk(g_ssMu);
    if (g_ssReq == &req) { g_ssReq = nullptr; g_ssArmed.store(false); }
    return false;
}
} // namespace detail

#if defined(__ANDROID__)
// Native methods called from ge.GeActivity — see android-shared/.../GeActivity.java.
//
// Both are designed to be cheap and main-thread-safe: they only touch
// atomics, never the game's RunConfig callbacks directly. The game-
// side dispatch happens on the next pumpEvents on the game thread.
extern "C" {

JNIEXPORT jboolean JNICALL
Java_ge_GeActivity_nativeOnBackPressed(JNIEnv*, jclass) {
    if (!g_backHandlerActive.load()) return JNI_FALSE;
    g_pendingBackPressed.store(true);
    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_ge_GeActivity_nativeOnTrimMemory(JNIEnv*, jclass, jint level) {
    int mapped = mapAndroidTrimLevel(level);
    if (mapped >= 0) g_pendingMemoryWarning.store(mapped);
}

// 🎯T43: audio focus change from Java's OnAudioFocusChangeListener.
// focusChange mirrors AudioManager constants:
//   AUDIOFOCUS_GAIN                  =  1 → resume
//   AUDIOFOCUS_LOSS                  = -1 → pause
//   AUDIOFOCUS_LOSS_TRANSIENT        = -2 → pause
//   AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK= -3 → pause (we don't duck, just pause)
JNIEXPORT void JNICALL
Java_ge_GeActivity_nativeOnAudioFocusChange(JNIEnv*, jclass, jint focusChange) {
    // Positive → gained, negative → lost.
    g_pendingAudioFocusChange.store(focusChange > 0 ? 1 : -1);
}

// 🎯T119 note: ge.GeActivity.passLaunchEnv bridges launch-Intent extras into the
// process env, but reuses SDL's own SDLActivity.nativeSetenv (exported from
// libSDL3.so) — ge needs no JNI of its own for it.

} // extern "C"
#endif

struct DirectRenderHost::Impl {
    int width = 0, height = 0;
    std::unique_ptr<SokolContext> sokolCtx;
    std::function<void(const SDL_Event&)> eventHandler;
    bool quit = false;

    // 🎯T88 iOS background gate. True while the app is backgrounded
    // (DID_ENTER_BACKGROUND, or launched directly into the background);
    // cleared on DID_ENTER_FOREGROUND. paused() returns this on iOS so the
    // render loop skips SokolContext::beginFrame — the backgrounded
    // simulator Metal driver blocks command-buffer creation, which trips
    // FrontBoard's scene-update watchdog (0x8BADF00D). While set,
    // pumpEvents idles on SDL_WaitEventTimeout instead of busy-spinning
    // (iOS, unlike Android, does not block ge's loop during background).
    bool backgrounded = false;

    // Host-owned session Context. Built in DirectRenderHost's ctor
    // (db setup is host-specific — desktop persistent file via
    // SDL_GetPrefPath) and refreshed each beginFrame so callback
    // accessors are always live.
    std::optional<Context> ctx;

    std::optional<AccelSynth> synth;
    SDL_Sensor* accelSensor = nullptr;
    // 🎯T156.2 session wants accelerometer input; seat authority decides the
    // source per frame (local device when direct/unattached, primary seat's
    // declared capability while streaming).
    bool wantAccelInput = false;
    bool seatAuthorityLogged = false;

    // 🎯T156.6 parity-oracle instrumentation (env-gated, mode-agnostic:
    // identical in direct and stream hosts).
    //  GE_INPUT_SCRIPT — scripted device-local input injected into the SDL
    //  queue each pump (direct-mode counterpart of the headless player's
    //  --script injection).
    //  GE_SENSOR_TRACE — JSONL trace at the game boundary: every sensor
    //  event the game sees, and per-frame presentation tilt, with wall-time.
    std::optional<InputScriptPlayer> inputScript;
    std::ofstream sensorTrace;
    bool sensorTraceOn = false;

    // 🎯T158 server-owned arm state: report synth arm transitions upstream
    // while a seat is attached; state resets on detach so a re-attaching
    // glass receives the current state.
    std::function<void(bool)> armStateSink;
    bool armSent = false;
    bool armSentValid = false;

    // Parallax pipeline (SessionHostConfig.parallaxFactor > 0).
    // attitude provider polled per frame; baseline is the EMA-tracked
    // neutral, delta is computed inverse(baseline) * current and
    // projected to screen-XY radians, scaled by parallaxFactor.
    float parallaxFactor = 0.0f;
    std::unique_ptr<AttitudeProvider> attitude;
    bool   baselineSet = false;
    la::float4 baseline{0, 0, 0, 1};

    // 🎯T44 / 🎯T45 callbacks — set by runDirect after factory.
    std::function<void()> onBackPressed;
    std::function<void(MemoryPressureLevel)> onMemoryWarning;

    // 🎯T92.2.2 Server mode: when serverActive is non-null and set, each
    // presented frame is routed to serverSink on the game thread (the
    // ServerSession encodes + streams it). Both null in the normal windowed
    // path. serverActive is owned by the ServerSession, which outlives the host.
    // serverCapturePixels: optional; when false, skip GPU readback (cmdstream).
    std::atomic<bool>* serverActive = nullptr;
    std::atomic<bool>* serverCapturePixels = nullptr;
    std::function<void(const std::uint8_t*, int, int)> serverSink;
    // Player DeviceInfo / SafeAreaUpdate → Context discovery (stream only).
    ViewerMetricsStore* viewerMetrics = nullptr;

    // 🎯T63 High-refresh-rate during press.
    // Incremented on every pointer Down, decremented on every Up/Cancel.
    // When non-zero the platform is asked to maintain max display refresh.
    RefreshRateBoost refreshRateBoost;

#if defined(__APPLE__) && TARGET_OS_IOS
    id memoryWarningObserver = nil;
#endif

    // Background clear color for the swap-chain pass. Opaque black; can
    // be made configurable later if a game needs a different letterbox.
    float clearColor[4] = {0.f, 0.f, 0.f, 1.f};

    // Resize handling: events fire on the main thread before frame
    // commands are encoded. SokolContext re-checks pixel size each
    // beginFrame() and resizes the CAMetalLayer's drawableSize itself,
    // so we just stage the new dims here and adopt them at the top of
    // beginFrame so per-frame Context state stays consistent.
    bool     pendingResize = false;
    int      pendingW = 0;
    int      pendingH = 0;
};

DirectRenderHost::DirectRenderHost(const SessionHostConfig& config)
    : i_(std::make_unique<Impl>()) {
    i_->width  = config.width  > 0 ? config.width  : 1280;
    i_->height = config.height > 0 ? config.height : 800;

    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER,
                config.disableScreenSaver ? "0" : "1");

    i_->sokolCtx = std::make_unique<SokolContext>(
        SokolConfig{i_->width, i_->height, config.appName, config.hidden});

#if defined(GE_SERVER_BUILD) && defined(__APPLE__) && TARGET_OS_OSX
    // 🎯T154.1 Server binary is a console stream host, not a Dock game.
    // SDL_Init creates NSApp; reassert Accessory after window creation.
    if (config.hidden) {
        @autoreleasepool {
            if (NSApp == nil) [NSApplication sharedApplication];
            [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        }
    }
#endif

    // On platforms where the actual surface size differs from the
    // caller's hint (notably Android fullscreen, and Retina on macOS),
    // SokolContext picks up the true native dimensions — adopt them so
    // the per-frame viewports and Context match.
    if (i_->sokolCtx->width()  > 0) i_->width  = i_->sokolCtx->width();
    if (i_->sokolCtx->height() > 0) i_->height = i_->sokolCtx->height();

    if (config.sensors & wire::kSensorAccelerometer) {
        i_->wantAccelInput = true;
        // Prefer real sensors when usable. iOS Simulator and Android emulator
        // report sensors but AccelSynth::realSensorAvailable() is false there —
        // host mouse / click-drag is the practical tilt path.
        if (AccelSynth::realSensorAvailable()) {
            int count = 0;
            SDL_SensorID* sensors = SDL_GetSensors(&count);
            if (sensors) {
                for (int k = 0; k < count; k++) {
                    if (SDL_GetSensorTypeForID(sensors[k]) == SDL_SENSOR_ACCEL) {
                        i_->accelSensor = SDL_OpenSensor(sensors[k]);
                        if (i_->accelSensor) {
                            SPDLOG_INFO("DirectRenderHost: opened real accelerometer");
                            break;
                        }
                    }
                }
                SDL_free(sensors);
            }
        }
        // Fall back to gesture synthesis for the LOCAL virtual device
        // (🎯T156.2): arm policy derives from this device's declared facts
        // (touch-first platform → primary-drag arms; keyboard-class → Shift).
        // While a stream seat is attached, the per-frame seat-authority check
        // in refreshFrame supersedes this with the seat's capability.
        if (!i_->accelSensor) {
            i_->synth.emplace();
            i_->synth->setWindow(i_->sokolCtx->window());
            i_->synth->setArmOnPrimary(localDeviceTouchFirst());
            SPDLOG_INFO("DirectRenderHost: gesture accelerometer synthesis "
                        "enabled (armOnPrimary={})", localDeviceTouchFirst());
        }
    }

    if (const char* sp = std::getenv("GE_INPUT_SCRIPT")) {
        std::vector<ScriptedEvent> evs;
        if (loadInputScript(sp, evs)) {
            i_->inputScript.emplace(std::move(evs));
            SPDLOG_INFO("DirectRenderHost: GE_INPUT_SCRIPT loaded ({})", sp);
        } else {
            SPDLOG_ERROR("DirectRenderHost: GE_INPUT_SCRIPT open failed ({})", sp);
        }
    }
    if (const char* tp = std::getenv("GE_SENSOR_TRACE")) {
        i_->sensorTrace.open(tp, std::ios::trunc);
        i_->sensorTraceOn = i_->sensorTrace.good();
        SPDLOG_INFO("DirectRenderHost: GE_SENSOR_TRACE {} ({})",
                    i_->sensorTraceOn ? "on" : "OPEN FAILED", tp);
    }

    SPDLOG_INFO("DirectRenderHost: {}x{}", i_->width, i_->height);

    // 🎯T43: install platform audio-focus observers.
#if defined(__APPLE__) && TARGET_OS_IOS
    ge::audio::installAppleAudioObservers();
#endif
    // Android audio focus is requested from GeActivity.java (onResume) via
    // AudioManager.requestAudioFocus; the JNI bridge
    // nativeOnAudioFocusChange() feeds g_pendingAudioFocusChange which
    // pumpEvents() drains on the game thread below.

    // Parallax pipeline — opt-in via SessionHostConfig.parallaxFactor.
    // The same float controls opt-in and sensitivity (decided 2026-04-13):
    // > 0 starts the platform sensor, scales the screen-XY delta the
    // engine exposes via Context::parallax().
    i_->parallaxFactor = config.parallaxFactor;
    if (i_->parallaxFactor > 0.0f) {
        i_->attitude = makeAttitudeProvider();
        if (!i_->attitude) {
            SPDLOG_INFO("DirectRenderHost: parallax requested but no platform "
                        "attitude provider — Context::parallax() stays {0,0}");
        }
    }

    // Durable db ownership (🎯T154) — see StreamHostPolicy.h.
    // Server build: always :memory:. Direct: PrefPath when available.
    std::string dbPath = ":memory:";
#if defined(GE_SERVER_BUILD)
    dbPath = durableDbPathForHost(/*serverBuild=*/true, config.orgName,
                                  config.appName, nullptr);
    SPDLOG_INFO("DirectRenderHost: stream server ephemeral db ({})", dbPath);
#else
    if (config.orgName && config.appName) {
        if (char* pref = SDL_GetPrefPath(config.orgName, config.appName)) {
            dbPath = durableDbPathForHost(/*serverBuild=*/false, config.orgName,
                                          config.appName, pref);
            // 🎯T91.2: persist tweak overrides alongside the game db.
            if (dbPath != ":memory:")
                tweak::loadOverrides((std::string(pref) + "tweaks.db").c_str());
            SDL_free(pref);
            SPDLOG_INFO("DirectRenderHost: persistent DB at {}", dbPath);
        }
    }
#endif
    i_->ctx.emplace(i_->width, i_->height, deviceClass(),
                    dbPath, config.schemaDdl);
    SPDLOG_INFO("DirectRenderHost: Context ready — applying safe insets");
    i_->ctx->setDrawSafeInsets(drawSafeInsetsInPts());
    SPDLOG_INFO("DirectRenderHost: drawSafe insets applied");
    i_->ctx->setUiSafeInsets(uiSafeInsetsInPts());
    SPDLOG_INFO("DirectRenderHost: uiSafe insets applied");
    // 🎯T101 Install the swapchain-pass factory. The game opens it once at the
    // top of onRender via ctx.swapchainPass(); the ctor opens the sokol swap-
    // chain pass and the returned ge::Pass's teardown arms any pending
    // screenshot and commits + presents — the body that used to live in the
    // loop-driven beginFrame / endFrame bracket.
    i_->ctx->setSwapchainPassFn([this]() -> Pass {
        i_->sokolCtx->beginFrame(i_->clearColor);
        return makePass([this]() {
            if (ge::detail::screenshotArmed()) {
                i_->sokolCtx->captureNextFrame(
                    [](const std::uint8_t* rgba, int w, int h) {
                        ge::detail::deliverScreenshot(rgba, w, h);
                    });
            } else if (i_->serverActive && i_->serverActive->load()) {
                // 🎯T92.2.2 server mode: GPU readback for H.264 / Present.
                // Command-stream (sprite SP2S) sets capturePixels=false and
                // serialises via LiveCapture around onRender instead.
                const bool wantPx = !i_->serverCapturePixels ||
                                    i_->serverCapturePixels->load();
                if (wantPx) {
                    i_->sokolCtx->captureNextFrame(
                        [this](const std::uint8_t* px, int w, int h) {
                            if (i_->serverSink) i_->serverSink(px, w, h);
                        });
                }
            }
            i_->sokolCtx->endFrame();
            i_->ctx->recordPresent();  // 🎯T131.5 advance framesPresented()
        });
    });
    {
        SDL_Window* win = i_->sokolCtx->window();
        // SDL_GetWindowDisplayScale = pixelDensity × displayContentScale.
        // On iOS the second factor is always 1.0, so this matches
        // SDL_GetWindowPixelDensity (== UIKit nativeScale). On Android
        // SDL exposes the surface in raw pixels (pixelDensity=1.0) and
        // the density bucket lives in displayContentScale, so this is
        // what folds Android density into pixelsPerPt.
        const float ppt = win ? SDL_GetWindowDisplayScale(win) : 1.0f;
        i_->ctx->setPixelsPerPt(ppt > 0.0f ? ppt : 1.0f);
        i_->ctx->setDeviceUiScale(computeDeviceUiScale(deviceClass(), i_->width, i_->height, ppt));
    }

#if defined(__APPLE__) && TARGET_OS_IPHONE
    // 🎯T88 Seed the background gate. If the app is launched directly into
    // the background (e.g. spyder/CLI launching the sim without bringing
    // Simulator.app forward), no DID_ENTER_BACKGROUND event ever fires —
    // the scene starts backgrounded. Without this seed the first
    // beginFrame would block on a command buffer the backgrounded scene
    // can't get. The ctor runs on the main thread on iOS (SDL_main is
    // invoked from -postFinishLaunch), so reading applicationState is safe.
    // Only Background pauses; Inactive (transient overlay / app switcher)
    // still permits Metal work, so it stays live.
    if ([UIApplication sharedApplication].applicationState
            == UIApplicationStateBackground) {
        i_->backgrounded = true;
        SPDLOG_INFO("DirectRenderHost: launched into background — render gated until foreground");
    }
#endif
    SPDLOG_INFO("DirectRenderHost: constructor complete");
}

DirectRenderHost::~DirectRenderHost() {
    if (i_->accelSensor) {
        SDL_CloseSensor(i_->accelSensor);
        i_->accelSensor = nullptr;
    }
    g_backHandlerActive.store(false);
#if defined(__APPLE__) && TARGET_OS_IOS
    if (i_->memoryWarningObserver) {
        [[NSNotificationCenter defaultCenter] removeObserver:i_->memoryWarningObserver];
        i_->memoryWarningObserver = nil;
    }
    ge::audio::removeAppleAudioObservers();
#endif
}

int DirectRenderHost::width() const  { return i_->width; }
int DirectRenderHost::height() const { return i_->height; }
DeviceClass DirectRenderHost::deviceClass() const {
#if defined(__APPLE__) && TARGET_OS_IPHONE
    return SDL_IsTablet() ? DeviceClass::Tablet : DeviceClass::Phone;
#elif defined(__ANDROID__)
    return SDL_IsTablet() ? DeviceClass::Tablet : DeviceClass::Phone;
#else
    return DeviceClass::Desktop;
#endif
}
bool DirectRenderHost::paused() const {
    // 🎯T154: viewer background over the wire (stream server) uses the same
    // paused() gate so the game does not branch on modality.
    if (detail::viewerBackgrounded()) return true;
#if defined(__APPLE__) && TARGET_OS_IPHONE
    // 🎯T88 On iOS (device + simulator) a backgrounded scene cannot get a
    // Metal command buffer — SokolContext::beginFrame would block forever
    // and the scene-update watchdog kills us. Gate the renderer on the
    // tracked background state (see Impl::backgrounded). Note the macOS
    // desktop case below still returns false: AppKit keeps the
    // CAMetalLayer live across app hide/space switches.
    return i_->backgrounded;
#else
    // macOS desktop direct: CAMetalLayer survives hide. Android direct tears
    // swap chain via SDL blocking the loop (SessionHost.mm).
    return false;
#endif
}
const Context& DirectRenderHost::context() const { return *i_->ctx; }

SafeAreaInsets DirectRenderHost::uiSafeInsetsInPts() const {
    // SDL3's safe area on Android unions systemBars + systemGestures
    // + mandatorySystemGestures + tappableElement + displayCutout —
    // the full input-safe rectangle. iOS reports its own safeAreaInsets
    // (cutouts + home indicator). Either way, this is what we want
    // for uiSafeRectInPts: the strictest "place interactive UI here" zone.
    //
    // Unit asymmetry between platforms (🎯T81):
    //   iOS: SDL_GetWindowSafeArea + SDL_GetWindowSize return point-space
    //        values (OS-logical coords), so the raw deltas are pt.
    //   Android: the SDL3 backend has pixelDensity=1.0 and exposes the
    //        surface in raw pixels (density bucket lives in
    //        displayContentScale instead — same convention that forces
    //        ppt = SDL_GetWindowDisplayScale in setPixelsPerPt above).
    //        We must divide each inset by ppt to land in pt.
    // drawSafeInsetsInPts() handles the same asymmetry below.
    SafeAreaInsets out{};
    if (!i_->sokolCtx) return out;
    SDL_Window* win = i_->sokolCtx->window();
    if (!win) return out;
    SDL_Rect safe{};
    if (!SDL_GetWindowSafeArea(win, &safe)) return out;
    int winW = 0, winH = 0;
    SDL_GetWindowSize(win, &winW, &winH);
    if (winW <= 0 || winH <= 0) return out;
    // Insets = the distance from each edge to the safe rect edge.
    // Screen is y-down (SDL convention), so the OS-supplied "top" inset
    // goes into the smaller-y edge field (y0), "bottom" into y1.
    auto ptInset = [](int v) {
        return v <= 0 ? 0.0f : std::ceil(float(v));
    };
    out.x0 = ptInset(safe.x);                 // left in y-down
    out.x1 = ptInset(winW - safe.x - safe.w); // right
    out.y0 = ptInset(safe.y);                 // top
    out.y1 = ptInset(winH - safe.y - safe.h); // bottom
#if defined(__ANDROID__)
    const float ppt = (SDL_GetWindowDisplayScale(win) > 0.0f)
                      ? SDL_GetWindowDisplayScale(win) : 1.0f;
    out = {out.y0 / ppt, out.y1 / ppt, out.x0 / ppt, out.x1 / ppt};
#endif

#if defined(__APPLE__) && TARGET_OS_IOS
    // iOS's SDL_GetWindowSafeArea reports only the static-chrome insets
    // (notch / status bar / home indicator). It does NOT include the
    // ~20pt top edge where Control / Notification Center swipe-down
    // captures touches, nor the ~34pt bottom edge around the home
    // indicator where edge-swipe gestures defer the first touch.
    //
    // Apple doesn't expose those zones via any public API. The honest
    // approximation is to enforce HIG-documented minimum clearances on
    // top and bottom: if the OS-reported safe area already covers them
    // (e.g. an iPhone notch leaves ~47pt at the top), keep the larger
    // value; otherwise floor at the gesture-zone constant. max() per
    // edge — not addition — so notched devices aren't double-inset.
    //
    // Constants from Apple's HIG / WWDC guidance; no runtime query
    // exists. Re-tune if Apple changes the deferral region in a future
    // iOS version.
    constexpr float kIosTopGesturePt    = 20.0f;
    constexpr float kIosBottomGesturePt = 34.0f;
    out.y0 = std::max(out.y0, kIosTopGesturePt);
    out.y1 = std::max(out.y1, kIosBottomGesturePt);
#endif

    return out;
}

SafeAreaInsets DirectRenderHost::drawSafeInsetsInPts() const {
    // Display-cutout-only insets in pt — what physically obscures pixels.
    // On Android we query WindowInsets.Type.displayCutout() via JNI
    // (gesture / tappable zones excluded). On iOS / desktop we fall
    // back to the full SDL safe area — iOS doesn't decompose its
    // safeAreaInsets cleanly so drawSafeRectInPts == uiSafeRectInPts there.
#if defined(__ANDROID__)
    // queryDisplayCutoutInsets returns pixel units (Android's WindowInsets
    // uses px). Divide by pixelsPerPt to convert to pt. (🎯T60)
    const SafeAreaInsets px = queryDisplayCutoutInsets();
    SDL_Window* win = i_->sokolCtx ? i_->sokolCtx->window() : nullptr;
    const float ppt = (win && SDL_GetWindowDisplayScale(win) > 0.0f)
                      ? SDL_GetWindowDisplayScale(win) : 1.0f;
    return {px.y0 / ppt, px.y1 / ppt, px.x0 / ppt, px.x1 / ppt};
#else
    return uiSafeInsetsInPts();
#endif
}

void DirectRenderHost::send(const wire::SessionConfig& cfg) {
    // iPadOS 26 ignores Info.plist orientation restrictions on iPad — the
    // only working lock is the prefersInterfaceOrientationLocked swizzle in
    // player_orientation_ios.mm. See ge/CLAUDE.md "iOS orientation lock".
    playerForceOrientation(cfg.orientation);  // 0 = no-op
    // Sensors are opened in the constructor from SessionHostConfig.
    // Sensor-frame rotation is keyed on the LIVE display orientation in
    // pumpEvents — cfg.orientation is just the lock request, not ground
    // truth, and tells us nothing in the no-lock case.
}

void DirectRenderHost::setEventHandler(std::function<void(const SDL_Event&)> h) {
    if (i_->sensorTraceOn) {
        auto* impl = i_.get();
        h = [impl, inner = std::move(h)](const SDL_Event& e) {
            if (e.type == SDL_EVENT_SENSOR_UPDATE && impl->sensorTraceOn) {
                using namespace std::chrono;
                const auto eus = duration_cast<microseconds>(
                    system_clock::now().time_since_epoch()).count();
                impl->sensorTrace << "{\"k\":\"sensor\",\"t_ms\":"
                                  << SDL_GetTicks() << ",\"e_us\":" << eus
                                  << ",\"gx\":"
                                  << e.sensor.data[0] << ",\"gy\":"
                                  << e.sensor.data[1] << "}\n";
                impl->sensorTrace.flush();
            }
            inner(e);
        };
    }
    i_->eventHandler = h;
    if (i_->synth) i_->synth->setEmit(h);
}

void DirectRenderHost::setBackPressedHandler(std::function<void()> h) {
    i_->onBackPressed = std::move(h);
    g_backHandlerActive.store(static_cast<bool>(i_->onBackPressed));
}

void DirectRenderHost::setMemoryWarningHandler(
        std::function<void(MemoryPressureLevel)> h) {
    i_->onMemoryWarning = std::move(h);
#if defined(__APPLE__) && TARGET_OS_IOS
    if (i_->onMemoryWarning && !i_->memoryWarningObserver) {
        i_->memoryWarningObserver =
            [[NSNotificationCenter defaultCenter]
                addObserverForName:UIApplicationDidReceiveMemoryWarningNotification
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification*) {
                            // iOS reports a single ungraded warning;
                            // bubble as Critical.
                            g_pendingMemoryWarning.store(
                                int(MemoryPressureLevel::Critical));
                        }];
    }
#endif
}

void DirectRenderHost::setServerFrameSink(
        std::function<void(const std::uint8_t*, int, int)> fn,
        std::atomic<bool>* active,
        std::atomic<bool>* capturePixels) {
    i_->serverSink = std::move(fn);
    i_->serverActive = active;
    i_->serverCapturePixels = capturePixels;
}

void DirectRenderHost::setViewerMetricsStore(ViewerMetricsStore* store) {
    i_->viewerMetrics = store;
}

void DirectRenderHost::setArmStateSink(std::function<void(bool)> sink) {
    i_->armStateSink = std::move(sink);
}

void DirectRenderHost::pumpEvents() {
    // 🎯T156.6: scripted input (parity oracle) enters the same SDL queue
    // as OS input — no separate delivery path.
    if (i_->inputScript && !i_->inputScript->done()) {
        i_->inputScript->poll(static_cast<uint32_t>(SDL_GetTicks()),
                              [this](const SDL_Event& e) {
                                  if (e.type == SDL_EVENT_SENSOR_UPDATE) {
                                      // Script SENSOR speaks screen frame —
                                      // the same boundary the player forwards
                                      // real samples at. Bypass the raw
                                      // device-frame rotation below.
                                      if (i_->eventHandler) i_->eventHandler(e);
                                      return;
                                  }
                                  if (e.type == SDL_EVENT_QUIT) {
                                      SDL_Event ev = e;
                                      SDL_PushEvent(&ev);
                                      return;
                                  }
                                  // Scripted device input: deliver through
                                  // the synth/handler directly. Real host
                                  // input is dropped below while a script
                                  // is active, so oracle runs are
                                  // deterministic even on a busy desktop.
                                  if (i_->synth && i_->synth->handle(e))
                                      return;
                                  if (i_->eventHandler) i_->eventHandler(e);
                              });
    }
    // Drain off-thread OS events first so back / memory-warning
    // callbacks see a coherent game state without racing the SDL
    // event drain below.
    if (g_pendingBackPressed.exchange(false)) {
        if (i_->onBackPressed) i_->onBackPressed();
    }
    int mem = g_pendingMemoryWarning.exchange(-1);
    if (mem >= 0 && i_->onMemoryWarning) {
        i_->onMemoryWarning(static_cast<MemoryPressureLevel>(mem));
    }
#if defined(__ANDROID__)
    // 🎯T43: drain Android audio focus change (set from Java's
    // OnAudioFocusChangeListener via nativeOnAudioFocusChange).
    int focusDelta = g_pendingAudioFocusChange.exchange(0);
    if (focusDelta < 0) {
        ge::audio::onAudioFocusLost();
    } else if (focusDelta > 0) {
        ge::audio::onAudioFocusGained();
    }
#endif

    // 🎯T88 While backgrounded on iOS the run loop has nothing to draw, so
    // block on the first event rather than spin: SDL does not block ge's
    // loop on iOS the way it does on Android, so a naive paused()->continue
    // would burn 100% CPU (and earn a CPU-usage watchdog kill) until the OS
    // suspends us. SDL_WaitEventTimeout idles the thread at ~0% CPU and
    // wakes within one event of foregrounding; the timeout bounds how long
    // we wait before the loop re-checks shouldQuit. The first delivered
    // event clears `blocking` so the remaining queue drains non-blocking.
    // Desktop/Android: paused() is false, so this is always a plain poll.
    constexpr int kPausedWaitMs = 250;
    // 🎯T88 paused (backgrounded) OR 🎯T132 idle (consumer opted out of
    // continuous rendering and no redraw is pending): block on the event source
    // instead of spinning. SDL_WaitEventTimeout idles at ~0% CPU and bounds the
    // shouldQuit re-check; the first delivered event clears `blocking` so the
    // rest of the queue drains non-blocking.
    // 🎯T131.1 Stay awake (poll, don't block) while a render trigger is active —
    // a still-awake physics sim / moving animation keeps the loop at full rate
    // until it settles, at which point all triggers go false and we idle.
    const bool onDemandIdle = i_->ctx && !i_->ctx->continuousRendering() &&
                              !i_->ctx->redrawPending() &&
                              !i_->ctx->anyRenderTriggerActive();
    bool blocking = paused() || onDemandIdle;
    bool sawRealEvent = false;
    SDL_Event e;
    for (;;) {
        bool have = blocking ? SDL_WaitEventTimeout(&e, kPausedWaitMs)
                             : SDL_PollEvent(&e);
        if (!have) break;
        blocking = false;
        // 🎯T132 Drop the redraw-wake sentinel — requestRedraw() already set the
        // pending flag; this event exists only to wake a blocked WaitEvent.
        if (e.type == SDL_EVENT_USER && e.user.code == Context::kRedrawEventCode) {
            continue;
        }
        // 🎯T132/T134 Resume rendering on a real event — but a continuous sensor
        // stream (accelerometer) isn't discrete input demanding a frame, so it
        // doesn't force a redraw on a static screen (see eventResumesRendering).
        if (detail::eventResumesRendering(e)) sawRealEvent = true;
        if (e.type == SDL_EVENT_QUIT) {
            i_->quit = true;
            continue;
        }
        // 🎯T7: iOS background/foreground audio lifecycle.
        // SDL fires these events on iOS (not Android) when the app moves
        // to the background / returns to foreground. Route to ge::audio
        // before continuing so audio silencing is immediate.
        if (e.type == SDL_EVENT_DID_ENTER_BACKGROUND) {
            ge::audio::onBackground();
            i_->backgrounded = true;   // 🎯T88 gate the renderer
            // Drain presses so the display isn't left pinned at max
            // refresh across the background transition (mirrors Android).
            i_->refreshRateBoost.drainPresses();
            continue;
        }
        if (e.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
            ge::audio::onForeground();
            // 🎯T108: drop the SokolContext's stale Metal drawable ref so
            // the first foreground beginFrame acquires a fresh one rather
            // than reusing a pre-background reference that renders black.
            // Also re-sync our shadow of the layer dimensions in case the
            // OS rotated/resized while we were backgrounded (mirrors the
            // Android FOCUS_GAINED branch's width/height sync below).
            if (i_->sokolCtx) {
                i_->sokolCtx->onForeground();
                i_->width  = i_->sokolCtx->width();
                i_->height = i_->sokolCtx->height();
            }
            i_->backgrounded = false;  // 🎯T88 resume rendering
            continue;
        }
        // Activity lifecycle on Android. SDL3 doesn't deliver the
        // higher-level SDL_EVENT_DID_ENTER_BACKGROUND/_FOREGROUND
        // events to the C side on Android (only iOS); we use the
        // window-level focus + minimize/restore events instead.
        // FOCUS_LOST is the earliest signal we get when the activity
        // starts backgrounding — gate the renderer immediately so a
        // dead surface doesn't get drawn into.
        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
            e.type == SDL_EVENT_WINDOW_HIDDEN     ||
            e.type == SDL_EVENT_WINDOW_MINIMIZED) {
            ge::audio::onBackground();  // 🎯T7: silence audio on Android background
            // 🎯T63 Drain presses so we don't leave the display pinned
            // at max refresh after a background transition.
            i_->refreshRateBoost.drainPresses();
            // TODO(T38-android): SokolContext_android — when the parallel
            // agent lands the Android backend, route this to
            // sokolCtx->onBackground() so the swap chain is torn down.
            continue;
        }
        if (e.type == SDL_EVENT_WINDOW_RESTORED   ||
            e.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
            e.type == SDL_EVENT_WINDOW_SHOWN) {
            ge::audio::onForeground();  // 🎯T7: resume audio on Android foreground
            // TODO(T38-android): SokolContext_android — when the parallel
            // agent lands the Android backend, route this to
            // sokolCtx->onForeground() to re-acquire the new ANativeWindow.
            // sokolCtx may have picked up a resized backbuffer; sync our
            // view of it so the next frame's viewports match.
            i_->width  = i_->sokolCtx->width();
            i_->height = i_->sokolCtx->height();
            continue;
        }
        // Any event that could signal a drawable-size change. On iOS the
        // definitive one is SDL_EVENT_WINDOW_METAL_VIEW_RESIZED — the
        // CAMetalLayer's drawableSize is only guaranteed current by then.
        // Stage the new size; apply at frame boundary.
        if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
            e.type == SDL_EVENT_WINDOW_RESIZED ||
            e.type == SDL_EVENT_WINDOW_METAL_VIEW_RESIZED) {
            int newW = 0, newH = 0;
            SDL_GetWindowSizeInPixels(i_->sokolCtx->window(), &newW, &newH);
            if (newW > 0 && newH > 0 &&
                (newW != i_->width || newH != i_->height)) {
                i_->pendingResize = true;
                i_->pendingW = newW;
                i_->pendingH = newH;
            }
            continue;
        }
        // Oracle determinism: while a script drives this host, real device
        // input (keys/mouse/finger/sensor) is dropped — the script is the
        // sole input source. Window/lifecycle events still flow.
        if (i_->inputScript) {
            switch (e.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_WHEEL:
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_CANCELED:
            case SDL_EVENT_SENSOR_UPDATE:
                continue;
            default:
                break;
            }
        }
        if (i_->synth && i_->synth->handle(e)) continue;
        // 🎯T156.2: no sensor arbitration here. Authority is constructional —
        // the synth only exists for a virtual device that declared no real
        // accelerometer, so a competing SENSOR_UPDATE source cannot exist
        // (spectator input is dropped at the SeatPolicy boundary).
        // Rotate real-sensor accel into screen frame using the live
        // display orientation. AccelSynth events bypass — they arrive
        // via setEmit() callback, already in screen frame
        // (mouse-displacement physics).
        // Server mode: player-side already screen-framed SENSOR_UPDATE
        // (PlayerRender) before the wire; re-rotating with the Mac host's
        // display orientation would swap/invert axes. Skip when streaming.
        if (e.type == SDL_EVENT_SENSOR_UPDATE &&
            !(i_->serverActive && i_->serverActive->load())) {
            SDL_DisplayOrientation o = SDL_ORIENTATION_UNKNOWN;
            if (SDL_DisplayID disp = SDL_GetDisplayForWindow(i_->sokolCtx->window())) {
                o = SDL_GetCurrentDisplayOrientation(disp);
            }
            ge::rotateAccelToScreen(o, e.sensor.data);
        }
        // 🎯T63 High-refresh-rate during press.
        // Track all pointer down/up events to maintain an accurate press
        // counter. SDL_TOUCH_MOUSEID synthetic mouse events from touch are
        // filtered: on platforms that generate both finger and synthetic
        // mouse events for a touch (iOS/Android), only the finger events
        // arrive here (the player filters SDL_TOUCH_MOUSEID in sdl_input).
        // DirectRenderHost runs on real hardware so SDL finger events are
        // the authoritative source; mouse button events cover desktop too.
        if (e.type == SDL_EVENT_FINGER_DOWN) {
            i_->refreshRateBoost.engagePress();
        } else if (e.type == SDL_EVENT_FINGER_UP ||
                   e.type == SDL_EVENT_FINGER_CANCELED) {
            i_->refreshRateBoost.releasePress();
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            if (e.button.which != SDL_TOUCH_MOUSEID) {
                i_->refreshRateBoost.engagePress();
            }
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            if (e.button.which != SDL_TOUCH_MOUSEID) {
                i_->refreshRateBoost.releasePress();
            }
        }
        if (i_->eventHandler) {
            ge::guardCallback("onEvent", [&] { i_->eventHandler(e); });  // 🎯T136
        }
    }
    // 🎯T132 Any real event (input, resize, lifecycle) resumes rendering: mark a
    // redraw so the run loop's on-demand skip renders the next frame. No-op in
    // continuous mode (the loop never consumes the flag there).
    if (sawRealEvent && i_->ctx) i_->ctx->markRedraw();
}

void DirectRenderHost::refreshFrame(float dt) {
    // While the activity is backgrounded the swap chain is gone (Android)
    // and any draw work will reference a stale ANativeWindow and crash on
    // present. Skip everything; SDL3 also blocks the main loop on Android
    // during background, so this path is mostly a no-op until the OS
    // resumes us.
    if (paused()) return;

    // Adopt any staged resize at the frame boundary so the Context dims
    // line up with what the next pass renders into. SokolContext itself
    // re-syncs the CAMetalLayer drawableSize each beginFrame, so we just
    // need to update our local mirror.
    if (i_->pendingResize) {
        SPDLOG_INFO("DirectRenderHost: resize {}x{} -> {}x{}",
                    i_->width, i_->height, i_->pendingW, i_->pendingH);
        i_->width = i_->pendingW;
        i_->height = i_->pendingH;
        i_->pendingResize = false;
    }

    // Drive AccelSynth ease-back even when no tilt is being applied —
    // it still emits SDL_EVENT_SENSOR_UPDATE events the game consumes.
    //
    // 🎯T94 Presentation tilt: expose the synth's contribution as a radians
    // angle-vector for ge::ortho::tilt. The synth only exists when no real
    // accelerometer was found, so this is {0,0} on real-accel platforms — the
    // view tilts only where there's no physical tilt to double up on. Small
    // deadzone (0.7 px) mirrors the player composite's threshold.
    // 🎯T156.2 Seat sensor authority: while a primary seat is attached, its
    // DeviceInfo capability decides this virtual device's sensor story.
    // Glass declares an accelerometer → its real stream is the authority and
    // no synth exists. Glass declares none → synth, with arm policy from the
    // seat's device class. Detach reverts to the local device's facts.
    if (i_->wantAccelInput) {
        const bool seatAttached =
            i_->serverActive && i_->serverActive->load() &&
            i_->viewerMetrics && i_->viewerMetrics->valid.load();
        if (seatAttached) {
            const ViewerWindow v = i_->viewerMetrics->snapshot();
            if (v.hasAccelerometer) {
                if (i_->synth) {
                    i_->synth.reset();
                    SPDLOG_INFO("DirectRenderHost: seat declares a real "
                                "accelerometer — synth destroyed; the glass "
                                "is the sensor authority");
                }
            } else {
                if (!i_->synth) {
                    i_->synth.emplace();
                    i_->synth->setWindow(i_->sokolCtx->window());
                    if (i_->eventHandler) i_->synth->setEmit(i_->eventHandler);
                    SPDLOG_INFO("DirectRenderHost: seat declares no "
                                "accelerometer — gesture synthesis enabled");
                }
                i_->synth->setArmOnPrimary(
                    v.deviceClass == DeviceClass::Phone ||
                    v.deviceClass == DeviceClass::Tablet);
            }
            if (!i_->seatAuthorityLogged) {
                i_->seatAuthorityLogged = true;
                SPDLOG_INFO("DirectRenderHost: seat sensor authority = {}",
                            v.hasAccelerometer ? "glass accelerometer"
                                               : "host AccelSynth");
            }
        } else {
            i_->seatAuthorityLogged = false;
            // Unattached (or direct): local device facts.
            if (!i_->accelSensor && !i_->synth) {
                i_->synth.emplace();
                i_->synth->setWindow(i_->sokolCtx->window());
                if (i_->eventHandler) i_->synth->setEmit(i_->eventHandler);
                i_->synth->setArmOnPrimary(localDeviceTouchFirst());
            }
        }
    }

    la::float2 presentationTilt{0.0f, 0.0f};
    if (i_->synth) {
        // Stream: denormalize finger 0–1 against the *player* surface
        // (DeviceInfo), not the host Mac window — same units as direct on
        // that device. Direct: setWindow already tracks local pixels.
        if (i_->serverActive && i_->serverActive->load() &&
            i_->viewerMetrics && i_->viewerMetrics->valid.load()) {
            const ViewerWindow v = i_->viewerMetrics->snapshot();
            if (v.w >= 2 && v.h >= 2)
                i_->synth->setSurfacePixels(v.w, v.h);
        }
        i_->synth->update();
        const Tilt t = i_->synth->current();
        if (std::sqrt(t.x * t.x + t.y * t.y) > 0.7f) {
            // AccelSynth's tilt is raw mouse displacement from the press point;
            // the view follows it. X maps directly (drag right → view tilts
            // right). Y is negated because SDL mouse-y grows downward, opposite
            // the presentation-tilt's y-up "tilt forward/back" sense.
            presentationTilt = la::float2{t.x, -t.y} * kTiltRadPerPixel;
        }
    }
    if (i_->sensorTraceOn) {
        i_->sensorTrace << "{\"k\":\"tilt\",\"t_ms\":" << SDL_GetTicks()
                        << ",\"x\":" << presentationTilt.x
                        << ",\"y\":" << presentationTilt.y << "}\n";
    }

    // 🎯T158: signal AccelSynth arm transitions to the primary glass while
    // a seat is attached (initial state included per attach).
    if (i_->armStateSink) {
        const bool seatAttached =
            i_->serverActive && i_->serverActive->load() &&
            i_->viewerMetrics && i_->viewerMetrics->valid.load();
        if (seatAttached) {
            const bool armedNow = i_->synth && i_->synth->armed();
            if (!i_->armSentValid || armedNow != i_->armSent) {
                i_->armStateSink(armedNow);
                i_->armSent = armedNow;
                i_->armSentValid = true;
            }
        } else {
            i_->armSentValid = false;
        }
    }

    // 🎯T101 The swap-chain pass is no longer opened here — the game opens it
    // via Context::swapchainPass() at the top of onRender (see the factory
    // installed in initialize()). This method only refreshes per-frame Context
    // state (post-resize + current insets) so onUpdate / onRender accessors
    // observe live values.
    //
    // Stream path: when a player has advertised DeviceInfo, publish that
    // surface into Context (draw/ui safe, device class, ui scale). Physical
    // path keeps the local OS. Compile-time backend is still
    // app vs server; this is discovery *source*, not a public switch.
    if (i_->ctx) {
        SDL_Window* win = i_->sokolCtx->window();
        const float hostPpt = win ? SDL_GetWindowDisplayScale(win) : 1.0f;
        const float ppt = hostPpt > 0.0f ? hostPpt : 1.0f;

        const bool streaming =
            i_->serverActive && i_->serverActive->load() &&
            i_->viewerMetrics && i_->viewerMetrics->valid.load();
        if (streaming) {
            const ViewerWindow v = i_->viewerMetrics->snapshot();
            // Match content aspect to the viewer so the stream fills the glass.
            // Fixed host 4:3 into a 16:10 tablet leaves ~1" pillarboxes — the
            // car stops short of the short edges and dirt never reaches them.
            // Pixel budget keeps the longer edge of the current surface.
            if (v.w >= 2 && v.h >= 2 && win) {
                const float targetA = float(v.w) / float(v.h);
                const float curA = float(i_->width) / float(std::max(i_->height, 1));
                if (std::fabs(targetA - curA) > 0.005f) {
                    const int budget = std::max(i_->width, i_->height);
                    int nw = 0, nh = 0;
                    if (targetA >= 1.f) {
                        nw = budget;
                        nh = std::max(2, int(std::lround(float(budget) / targetA)));
                    } else {
                        nh = budget;
                        nw = std::max(2, int(std::lround(float(budget) * targetA)));
                    }
                    nw &= ~1;
                    nh &= ~1;
                    if (nw != i_->width || nh != i_->height) {
                        float dens = SDL_GetWindowPixelDensity(win);
                        if (dens <= 0.f) dens = 1.f;
                        const int ptW = std::max(1, int(std::lround(float(nw) / dens)));
                        const int ptH = std::max(1, int(std::lround(float(nh) / dens)));
                        SDL_SetWindowSize(win, ptW, ptH);
                        SPDLOG_INFO("DirectRenderHost: content aspect → viewer "
                                    "{:.3f} ({}x{} → {}x{})",
                                    targetA, i_->width, i_->height, nw, nh);
                        i_->width = nw;
                        i_->height = nh;
                    }
                }
            }

            i_->ctx->setSurfaceDimensions(i_->width, i_->height);
            // Map viewer chrome into content space. Dual draw/ui when the
            // player sends distinct rects (v8); else equal.
            const DualSafeInsets dual =
                mapViewerDualSafeInsets(v, i_->width, i_->height, ppt);
            i_->ctx->setDrawSafeInsets(dual.draw);
            i_->ctx->setUiSafeInsets(dual.ui);
            i_->ctx->setDeviceClass(v.deviceClass);
            // pixelsPerPt: content-surface conversion (stable fullRect).
            // deviceUiScale: viewer form-factor (glass).
            i_->ctx->setPixelsPerPt(ppt);
            const float viewerPpt = v.pixelRatio > 0.f ? v.pixelRatio : 1.f;
            i_->ctx->setDeviceUiScale(
                computeDeviceUiScale(v.deviceClass, v.w, v.h, viewerPpt));
            // Parallax from host attitude would lie about the viewer — zero it
            // under stream (🎯T154: same call site, documented zero).
            i_->ctx->setParallax({0, 0});
        } else {
            i_->ctx->setSurfaceDimensions(i_->width, i_->height);
            i_->ctx->setDrawSafeInsets(drawSafeInsetsInPts());
            i_->ctx->setUiSafeInsets(uiSafeInsetsInPts());
            i_->ctx->setDeviceClass(deviceClass());
            i_->ctx->setPixelsPerPt(ppt);
            i_->ctx->setDeviceUiScale(
                computeDeviceUiScale(deviceClass(), i_->width, i_->height, ppt));
            i_->ctx->setParallax(updateParallax());
        }
        i_->ctx->setPresentationTilt(presentationTilt);
        i_->ctx->recordFrameTime(dt);  // 🎯T111 frame-time EMA → fps()/onMetrics
    }
}

la::float2 DirectRenderHost::updateParallax() {
    using la::float4;
    if (i_->parallaxFactor <= 0.0f || !i_->attitude || !i_->attitude->valid()) {
        return {0, 0};
    }
    // Quaternion EMA: with α small the result stays close to a unit
    // quaternion, so plain componentwise lerp is fine — no slerp, no
    // renormalization needed (drift is self-correcting under
    // repeated blends toward unit-length samples).
    constexpr float kBaselineTau = 1.0f;  // seconds
    constexpr float kAssumedDt   = 1.0f / 60.0f;
    const float a = 1.0f - std::exp(-kAssumedDt / kBaselineTau);

    const float4 cur = i_->attitude->quaternion();
    if (!i_->baselineSet) {
        i_->baseline = cur;
        i_->baselineSet = true;
        return {0, 0};
    }
    i_->baseline = linalg::lerp(i_->baseline, cur, a);

    // delta = inverse(baseline) * current. Quaternion inverse for a
    // unit quaternion is the conjugate.
    const float4 b = i_->baseline;
    const float4 binv{-b.x, -b.y, -b.z, b.w};
    // Hamilton product binv * cur.
    const float4 d{
        binv.w * cur.x + binv.x * cur.w + binv.y * cur.z - binv.z * cur.y,
        binv.w * cur.y - binv.x * cur.z + binv.y * cur.w + binv.z * cur.x,
        binv.w * cur.z + binv.x * cur.y - binv.y * cur.x + binv.z * cur.w,
        binv.w * cur.w - binv.x * cur.x - binv.y * cur.y - binv.z * cur.z,
    };
    // Small-angle approximation: rotation around the screen-X axis is
    // 2 * d.x radians, around screen-Y is 2 * d.y. Sign matches "tilt
    // toward +axis = positive value".
    return la::float2{2.0f * d.x, 2.0f * d.y} * i_->parallaxFactor;
}

bool DirectRenderHost::shouldQuit() const {
    return i_->quit || ge::shouldQuit();
}

} // namespace ge
