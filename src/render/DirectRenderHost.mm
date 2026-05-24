// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "DirectRenderHost.h"
#include "AccelSynth.h"

#include "../Attitude.h"
#include "../CutoutInsets.h"

#include <ge/audio.h>
#include <ge/FileIO.h>
#include <ge/Protocol.h>
#include <ge/Resource.h>
#include <ge/Signal.h>

#include "../../tools/player_orientation.h"

#include <algorithm>
#include <iterator>
#include <vector>

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <spdlog/spdlog.h>

#include <atomic>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
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

namespace {

// View 0: engine's game rendering.
// View 1: composite pass (textured quad with tilt, targets the swap chain).
constexpr bgfx::ViewId kGameView     = 0;
constexpr bgfx::ViewId kComposeView  = 1;

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

#if defined(__ANDROID__)
// 🎯T43 Android audio focus. Java's OnAudioFocusChangeListener fires on
// the UI thread and stores a signed delta here:
//   +1 = focus gained (resume)
//   -1 = focus lost (pause: LOSS, LOSS_TRANSIENT, LOSS_TRANSIENT_CAN_DUCK)
// pumpEvents drains this on the game thread.
std::atomic<int> g_pendingAudioFocusChange{0};
#endif

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

// Vertex layout for the composite quad.
struct ComposeVertex {
    float x, y, z;
    float u, v;
};

bgfx::VertexLayout composeLayout() {
    bgfx::VertexLayout l;
    l.begin()
        .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
        .end();
    return l;
}

// Rotate a 3-axis accelerometer sample from the device-hardware frame
// (SDL3's reported convention: +X = physical right edge up, +Y = physical
// top edge up, +Z = screen up) into the game's screen frame.
//
// The rotation is keyed on the LIVE display orientation reported by SDL
// (SDL_GetCurrentDisplayOrientation) — not on SessionConfig.orientation,
// which only records what the app *requested*. The live value reflects
// what the OS actually rotated to (post-lock-if-honored, or the live
// rotation when no lock was requested), so this stays correct in both
// locked and free-orientation modes and across the brief window between
// a lock request and the OS settling.
//
// Touch and mouse coordinates are NOT rotated — both iOS and Android
// already deliver those in the rotated UI frame. Accelerometer is the
// outlier because the sensor chip is fixed to the chassis and has no
// notion of UI orientation. Z (out of screen) is invariant under any
// in-plane UI rotation, so it passes through.
//
// Each case is a 2D rotation expressed as a swap-and-sign on (x, y):
//
//   Portrait        identity            ( x,  y)
//   Landscape       device rotated CW   (-y,  x)
//   PortraitFlipped device upside down  (-x, -y)
//   LandscapeFlip   device rotated CCW  ( y, -x)
void rotateAccelToScreen(SDL_DisplayOrientation orient, float d[/*≥3*/]) {
    const float x = d[0];
    const float y = d[1];
    switch (orient) {
    case SDL_ORIENTATION_LANDSCAPE:
        d[0] = -y; d[1] =  x; break;
    case SDL_ORIENTATION_LANDSCAPE_FLIPPED:
        d[0] =  y; d[1] = -x; break;
    case SDL_ORIENTATION_PORTRAIT_FLIPPED:
        d[0] = -x; d[1] = -y; break;
    case SDL_ORIENTATION_PORTRAIT:
    case SDL_ORIENTATION_UNKNOWN:
    default:
        break;  // identity — fall back to device frame on unknown
    }
}

bgfx::ShaderHandle loadShader(const char* path) {
    auto stream = ge::openFile(path, /*binary=*/true);
    if (!stream || !*stream) {
        SPDLOG_ERROR("DirectRenderHost: can't open shader {}", path);
        return BGFX_INVALID_HANDLE;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(*stream)),
                              std::istreambuf_iterator<char>());
    if (data.empty()) {
        SPDLOG_ERROR("DirectRenderHost: empty shader {}", path);
        return BGFX_INVALID_HANDLE;
    }
    const bgfx::Memory* mem = bgfx::alloc(static_cast<uint32_t>(data.size() + 1));
    std::memcpy(mem->data, data.data(), data.size());
    mem->data[data.size()] = '\0';
    return bgfx::createShader(mem);
}

} // namespace

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

} // extern "C"
#endif

struct DirectRenderHost::Impl {
    int width = 0, height = 0;
    std::unique_ptr<BgfxContext> bgfxCtx;
    std::function<void(const SDL_Event&)> eventHandler;
    bool quit = false;

    // Host-owned session Context. Built in DirectRenderHost's ctor
    // (db setup is host-specific — desktop persistent file via
    // SDL_GetPrefPath) and refreshed each beginFrame so callback
    // accessors are always live.
    std::optional<Context> ctx;

    std::optional<AccelSynth> synth;
    SDL_Sensor* accelSensor = nullptr;

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
#if defined(__APPLE__) && TARGET_OS_IOS
    id memoryWarningObserver = nil;
#endif

    // Offscreen pipeline — lazily created on first non-zero tilt.
    bool offscreenReady = false;
    int  offscreenW = 0, offscreenH = 0;
    bgfx::FrameBufferHandle offFB    = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     colorTex = BGFX_INVALID_HANDLE;
    bgfx::TextureHandle     depthTex = BGFX_INVALID_HANDLE;
    bgfx::ProgramHandle     compose  = BGFX_INVALID_HANDLE;
    bgfx::UniformHandle     sampler  = BGFX_INVALID_HANDLE;
    bgfx::VertexLayout      layout;

    // Per-frame flag: is tilt active on this frame?
    bool tiltActiveThisFrame = false;

    // Resize handling: events fire on the main thread before frame
    // commands are encoded, but bgfx::reset must be paired with all
    // resize-dependent work (destroyOffscreen, setViewRect) at a
    // frame boundary. We stage the new dims here and apply them at
    // the top of beginFrame, then clear the flag.
    bool     pendingResize = false;
    int      pendingW = 0;
    int      pendingH = 0;

    void ensureOffscreen() {
        if (offscreenReady && offscreenW == width && offscreenH == height)
            return;
        // Stale offscreen (size mismatch after a resize that wasn't
        // cleanly torn down): rebuild at current dims.
        if (offscreenReady) destroyOffscreen();
        layout = composeLayout();
        colorTex = bgfx::createTexture2D(
            width, height, false, 1, bgfx::TextureFormat::BGRA8,
            BGFX_TEXTURE_RT);
        depthTex = bgfx::createTexture2D(
            width, height, false, 1, bgfx::TextureFormat::D24S8,
            BGFX_TEXTURE_RT_WRITE_ONLY);
        bgfx::TextureHandle atts[] = { colorTex, depthTex };
        offFB = bgfx::createFrameBuffer(2, atts, /*destroyTextures=*/false);

        bgfx::ShaderHandle vs = loadShader(ge::resource(ge::renderShaderDir() + "/ge_compose_vs.bin").c_str());
        bgfx::ShaderHandle fs = loadShader(ge::resource(ge::renderShaderDir() + "/ge_compose_fs.bin").c_str());
        if (!bgfx::isValid(vs) || !bgfx::isValid(fs)) {
            SPDLOG_ERROR("DirectRenderHost: compose shaders missing — viewport tilt disabled");
            return;
        }
        compose = bgfx::createProgram(vs, fs, /*destroyShaders=*/true);
        sampler = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);
        offscreenReady = true;
        offscreenW = width;
        offscreenH = height;
        SPDLOG_INFO("DirectRenderHost: offscreen pipeline created ({}x{})", width, height);
    }

    void destroyOffscreen() {
        if (bgfx::isValid(sampler))  { bgfx::destroy(sampler);  sampler  = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(compose))  { bgfx::destroy(compose);  compose  = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(offFB))    { bgfx::destroy(offFB);    offFB    = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(colorTex)) { bgfx::destroy(colorTex); colorTex = BGFX_INVALID_HANDLE; }
        if (bgfx::isValid(depthTex)) { bgfx::destroy(depthTex); depthTex = BGFX_INVALID_HANDLE; }
        offscreenReady = false;
    }
};

DirectRenderHost::DirectRenderHost(const SessionHostConfig& config)
    : i_(std::make_unique<Impl>()) {
    i_->width  = config.width  > 0 ? config.width  : 1280;
    i_->height = config.height > 0 ? config.height : 800;

    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER,
                config.disableScreenSaver ? "0" : "1");

    i_->bgfxCtx = std::make_unique<BgfxContext>(
        BgfxConfig{i_->width, i_->height, /*headless=*/false, config.appName});

    // On platforms where the actual surface size differs from the
    // caller's hint (notably Android fullscreen), BgfxContext picks up
    // the true native dimensions — adopt them so our offscreen
    // pipeline and per-frame viewports match.
    if (i_->bgfxCtx->width()  > 0) i_->width  = i_->bgfxCtx->width();
    if (i_->bgfxCtx->height() > 0) i_->height = i_->bgfxCtx->height();

    if (config.sensors & wire::kSensorAccelerometer) {
        // Try a real sensor first (real device, Android emulator virtual sensors).
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
        // Fall back to Shift-mouse synthesis.
        if (!i_->accelSensor) {
            i_->synth.emplace();
            i_->synth->setWindow(i_->bgfxCtx->window());
            SPDLOG_INFO("DirectRenderHost: Shift-mouse accelerometer synthesis enabled");
        }
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

    // Build the session Context. Direct mode uses a persistent on-disk
    // db rooted at SDL_GetPrefPath(orgName, appName); fall back to an
    // in-memory db if either is missing.
    std::string dbPath = ":memory:";
    if (config.orgName && config.appName) {
        if (char* pref = SDL_GetPrefPath(config.orgName, config.appName)) {
            dbPath = std::string(pref) + "game.db";
            SDL_free(pref);
            SPDLOG_INFO("DirectRenderHost: persistent DB at {}", dbPath);
        }
    }
    i_->ctx.emplace(i_->width, i_->height, deviceClass(),
                    dbPath, config.schemaDdl);
    i_->ctx->setDrawSafeInsets(drawSafeInsetsInPts());
    i_->ctx->setUiSafeInsets(uiSafeInsetsInPts());
    {
        SDL_Window* win = i_->bgfxCtx->window();
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
    i_->destroyOffscreen();
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
bool DirectRenderHost::paused() const { return i_->bgfxCtx && i_->bgfxCtx->paused(); }
const Context& DirectRenderHost::context() const { return *i_->ctx; }

SafeAreaInsets DirectRenderHost::uiSafeInsetsInPts() const {
    // SDL3's safe area on Android unions systemBars + systemGestures
    // + mandatorySystemGestures + tappableElement + displayCutout —
    // the full input-safe rectangle. iOS reports its own safeAreaInsets
    // (cutouts + home indicator). Either way, this is what we want
    // for uiSafeRectInPts: the strictest "place interactive UI here" zone.
    //
    // SDL_GetWindowSafeArea and SDL_GetWindowSize both return point-space
    // values (OS-logical coords), so the insets this function computes are
    // already in pt — no pixel-density multiplication needed. (🎯T60)
    SafeAreaInsets out{};
    if (!i_->bgfxCtx) return out;
    SDL_Window* win = i_->bgfxCtx->window();
    if (!win) return out;
    SDL_Rect safe{};
    if (!SDL_GetWindowSafeArea(win, &safe)) return out;
    int winW = 0, winH = 0;
    SDL_GetWindowSize(win, &winW, &winH);
    if (winW <= 0 || winH <= 0) return out;
    // SDL window coords are in points. Insets = the distance from each
    // edge to the safe rect edge, in pt. Screen is y-down (SDL/bgfx),
    // so the OS-supplied "top" inset goes into the smaller-y edge
    // field (y0), and "bottom" into the larger-y edge field (y1).
    auto ptInset = [](int v) {
        return v <= 0 ? 0.0f : std::ceil(float(v));
    };
    out.x0 = ptInset(safe.x);               // left in y-down, pt
    out.x1 = ptInset(winW - safe.x - safe.w); // right, pt
    out.y0 = ptInset(safe.y);               // top in y-down, pt
    out.y1 = ptInset(winH - safe.y - safe.h); // bottom, pt

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
    SDL_Window* win = i_->bgfxCtx ? i_->bgfxCtx->window() : nullptr;
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

void DirectRenderHost::pumpEvents() {
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

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
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
            continue;
        }
        if (e.type == SDL_EVENT_DID_ENTER_FOREGROUND) {
            ge::audio::onForeground();
            continue;
        }
        // Activity lifecycle on Android. SDL3 doesn't deliver the
        // higher-level SDL_EVENT_DID_ENTER_BACKGROUND/_FOREGROUND
        // events to the C side on Android (only iOS); we use the
        // window-level focus + minimize/restore events instead.
        // FOCUS_LOST is the earliest signal we get when the activity
        // starts backgrounding — gate bgfx immediately to avoid the
        // BLAST BufferQueue-abandoned spam from a dead surface.
        // RESTORED / FOCUS_GAINED is the resumption signal — re-acquire
        // the new ANativeWindow and rebuild the swap chain.
        // Both no-op on Apple (BgfxContext::onForeground is #if Android).
        if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST ||
            e.type == SDL_EVENT_WINDOW_HIDDEN     ||
            e.type == SDL_EVENT_WINDOW_MINIMIZED) {
            ge::audio::onBackground();  // 🎯T7: silence audio on Android background
            i_->bgfxCtx->onBackground();
            continue;
        }
        if (e.type == SDL_EVENT_WINDOW_RESTORED   ||
            e.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
            e.type == SDL_EVENT_WINDOW_SHOWN) {
            ge::audio::onForeground();  // 🎯T7: resume audio on Android foreground
            i_->bgfxCtx->onForeground();
            // bgfxCtx may have picked up a resized backbuffer; sync our
            // view of it so the next frame's viewports match.
            i_->width  = i_->bgfxCtx->width();
            i_->height = i_->bgfxCtx->height();
            // Offscreen pipeline references the old swap chain — drop it
            // so it's recreated on demand against the new backbuffer.
            i_->destroyOffscreen();
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
            SDL_GetWindowSizeInPixels(i_->bgfxCtx->window(), &newW, &newH);
            if (newW > 0 && newH > 0 &&
                (newW != i_->width || newH != i_->height)) {
                i_->pendingResize = true;
                i_->pendingW = newW;
                i_->pendingH = newH;
            }
            continue;
        }
        if (i_->synth && i_->synth->handle(e)) continue;
        // Rotate real-sensor accel into screen frame using the live
        // display orientation. AccelSynth events bypass — they arrive
        // via setEmit() callback, already in screen frame
        // (mouse-displacement physics).
        if (e.type == SDL_EVENT_SENSOR_UPDATE) {
            SDL_DisplayOrientation o = SDL_ORIENTATION_UNKNOWN;
            if (SDL_DisplayID disp = SDL_GetDisplayForWindow(i_->bgfxCtx->window())) {
                o = SDL_GetCurrentDisplayOrientation(disp);
            }
            rotateAccelToScreen(o, e.sensor.data);
        }
        if (i_->eventHandler) i_->eventHandler(e);
    }
}

void DirectRenderHost::beginFrame() {
    // While the activity is backgrounded the swap chain is gone (Android)
    // and any bgfx::reset / view setup we do here will reference a stale
    // ANativeWindow and crash on next bgfx::frame(). Skip everything; SDL3
    // also blocks the main loop on Android during background, so this
    // path is mostly a no-op until the OS resumes us.
    if (i_->bgfxCtx->paused()) return;

    // Apply any staged resize at the frame boundary so all bgfx state
    // (backbuffer, offscreen RT, view rects) moves together.
    if (i_->pendingResize) {
        SPDLOG_INFO("DirectRenderHost: resize {}x{} -> {}x{}",
                    i_->width, i_->height, i_->pendingW, i_->pendingH);
        i_->width = i_->pendingW;
        i_->height = i_->pendingH;
        i_->pendingResize = false;
        bgfx::reset(uint32_t(i_->width), uint32_t(i_->height),
                    BGFX_RESET_VSYNC);
        // Offscreen pipeline tied to old size — tear down; it will be
        // recreated below if this frame needs the composited path.
        i_->destroyOffscreen();
    }

    // Decide for this frame: is tilt non-zero? If so, use the composited
    // path (render game into offscreen FB, compose a tilted quad onto the
    // swap chain). Otherwise, render straight to the swap chain.
    Tilt t{};
    if (i_->synth) {
        i_->synth->update();   // drive ease-back when Shift has been released
        t = i_->synth->current();
    }
    i_->tiltActiveThisFrame = (t.x * t.x + t.y * t.y) > 0.5f;  // >~0.7 px
    if (i_->tiltActiveThisFrame) i_->ensureOffscreen();

    if (i_->tiltActiveThisFrame && i_->offscreenReady) {
        bgfx::setViewFrameBuffer(kGameView, i_->offFB);
    } else {
        bgfx::setViewFrameBuffer(kGameView, BGFX_INVALID_HANDLE);
    }
    bgfx::setViewRect(kGameView, 0, 0, uint16_t(i_->width), uint16_t(i_->height));

    // Submit the compose pass in the same frame as the game render.
    // bgfx executes views in ID order, so view 0 (game) writes to offFB
    // before view 1 (compose) samples it. Submitting in endFrame instead
    // would queue compose into the *next* frame's buffer — a handle
    // referenced by that pending draw could then be freed during a
    // resize-triggered destroyOffscreen, flashing magenta.
    if (i_->tiltActiveThisFrame && i_->offscreenReady) {
        submitCompose(t.x, t.y);
    }

    // Refresh per-frame Context state (post-resize + current insets)
    // so onUpdate / onRender accessors observe live values.
    if (i_->ctx) {
        i_->ctx->setSurfaceDimensions(i_->width, i_->height);
        i_->ctx->setDrawSafeInsets(drawSafeInsetsInPts());
        i_->ctx->setUiSafeInsets(uiSafeInsetsInPts());
        SDL_Window* win = i_->bgfxCtx->window();
        // SDL_GetWindowDisplayScale = pixelDensity × displayContentScale.
        // On iOS the second factor is always 1.0, so this matches
        // SDL_GetWindowPixelDensity (== UIKit nativeScale). On Android
        // SDL exposes the surface in raw pixels (pixelDensity=1.0) and
        // the density bucket lives in displayContentScale, so this is
        // what folds Android density into pixelsPerPt.
        const float ppt = win ? SDL_GetWindowDisplayScale(win) : 1.0f;
        i_->ctx->setPixelsPerPt(ppt > 0.0f ? ppt : 1.0f);
        i_->ctx->setDeviceUiScale(computeDeviceUiScale(deviceClass(), i_->width, i_->height, ppt));
        i_->ctx->setParallax(updateParallax());
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

void DirectRenderHost::endFrame(uint32_t /*bgfxFrameNumber*/) {
    // Direct mode has nothing to do post-frame; brokered mode reads
    // the backbuffer here for video encoding.
}

void DirectRenderHost::submitCompose(float tx, float ty) {
    float mag = std::sqrt(tx * tx + ty * ty);
    float angle = mag * kTiltRadPerPixel;

    // Rotation axis in screen plane, perpendicular to displacement.
    float axX =  (mag > 0.f) ? (-ty / mag) : 0.f;
    float axY =  (mag > 0.f) ? ( tx / mag) : 0.f;
    float axZ = 0.f;

    // Set up view 1 targeting the swap chain.
    bgfx::setViewFrameBuffer(kComposeView, BGFX_INVALID_HANDLE);
    bgfx::setViewRect(kComposeView, 0, 0, uint16_t(i_->width), uint16_t(i_->height));
    bgfx::setViewClear(kComposeView, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
                       0x000000ff, 1.0f, 0);

    // Perspective camera: at distance D from origin, FOV chosen so a
    // 2×2 quad at z=0 exactly fills clip space when untilted.
    // tan(fovY/2) = 1/D  →  fovY = 2·atan(1/D).
    const float D = 2.0f;
    const float aspect = float(i_->width) / float(i_->height);
    const float fovY = 2.f * std::atan(1.f / D) * 180.f / 3.14159265f;
    float proj[16];
    bx::mtxProj(proj, fovY, aspect, 0.01f, D * 4.f,
                bgfx::getCaps()->homogeneousDepth);

    float view[16];
    const bx::Vec3 eye{0.f, 0.f, D};
    const bx::Vec3 at {0.f, 0.f, 0.f};
    const bx::Vec3 up {0.f, 1.f, 0.f};
    bx::mtxLookAt(view, eye, at, up);
    bgfx::setViewTransform(kComposeView, view, proj);

    // Quad dimensions: width = aspect, height = 1 (so it fits the frustum
    // horizontally as well as vertically at zero tilt).
    const float halfW = aspect;
    const float halfH = 1.f;
    // bx::mtxLookAt defaults to left-handed, which flips X in view space
    // (eye at +Z looking at origin → side = up×forward = -X). Sampling u=1
    // at the quad's -X edge compensates so world-space orientation on the
    // render target maps back correctly on the composited quad.
    //
    // Render-target texel origin is top-left on Metal/Vulkan, bottom-left
    // on GLES — bgfx reports this via caps so we flip V only when needed.
    const bool  flipV = bgfx::getCaps()->originBottomLeft;
    const float v0 = flipV ? 1.f : 0.f;  // world +Y (top) → texel v
    const float v1 = flipV ? 0.f : 1.f;  // world -Y (bottom) → texel v
    ComposeVertex verts[6] = {
        { -halfW, -halfH, 0.f, 1.f, v1 },
        {  halfW, -halfH, 0.f, 0.f, v1 },
        {  halfW,  halfH, 0.f, 0.f, v0 },
        { -halfW, -halfH, 0.f, 1.f, v1 },
        {  halfW,  halfH, 0.f, 0.f, v0 },
        { -halfW,  halfH, 0.f, 1.f, v0 },
    };

    bgfx::TransientVertexBuffer tvb;
    bgfx::allocTransientVertexBuffer(&tvb, 6, i_->layout);
    std::memcpy(tvb.data, verts, sizeof(verts));

    // Model transform: rotation about (axX, axY, 0) by `angle`.
    float model[16];
    bx::Quaternion q = bx::fromAxisAngle(bx::Vec3{axX, axY, axZ}, angle);
    bx::mtxFromQuaternion(model, q);

    // Re-fit the tilted quad to the viewport: compute the projected
    // bounding box of the four corners in NDC, then prepend a 2D
    // scale+translate to the projection so the bbox is centered and
    // maximally sized without clipping.
    auto applyMtx = [](const float* m, float x, float y, float z, float w,
                       float out[4]) {
        out[0] = m[0]*x + m[4]*y + m[ 8]*z + m[12]*w;
        out[1] = m[1]*x + m[5]*y + m[ 9]*z + m[13]*w;
        out[2] = m[2]*x + m[6]*y + m[10]*z + m[14]*w;
        out[3] = m[3]*x + m[7]*y + m[11]*z + m[15]*w;
    };
    float corners[4][2] = {
        {-halfW, -halfH}, { halfW, -halfH},
        { halfW,  halfH}, {-halfW,  halfH},
    };
    float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
    for (auto& c : corners) {
        float worldP[4], viewP[4], clipP[4];
        applyMtx(model, c[0], c[1], 0.f, 1.f, worldP);
        applyMtx(view,  worldP[0], worldP[1], worldP[2], worldP[3], viewP);
        applyMtx(proj,  viewP[0],  viewP[1],  viewP[2],  viewP[3],  clipP);
        const float invW = 1.f / clipP[3];
        const float ndcX = clipP[0] * invW;
        const float ndcY = clipP[1] * invW;
        if (ndcX < minX) minX = ndcX;
        if (ndcY < minY) minY = ndcY;
        if (ndcX > maxX) maxX = ndcX;
        if (ndcY > maxY) maxY = ndcY;
    }
    const float bboxW = maxX - minX;
    const float bboxH = maxY - minY;
    const float cx    = (minX + maxX) * 0.5f;
    const float cy    = (minY + maxY) * 0.5f;
    const float fitS  = std::min(2.f / bboxW, 2.f / bboxH);
    float fit[16];
    bx::mtxIdentity(fit);
    fit[0]  = fitS;
    fit[5]  = fitS;
    fit[12] = -fitS * cx;
    fit[13] = -fitS * cy;
    float fittedProj[16];
    bx::mtxMul(fittedProj, fit, proj);
    bgfx::setViewTransform(kComposeView, view, fittedProj);

    bgfx::setTransform(model);
    bgfx::setVertexBuffer(0, &tvb, 0, 6);
    bgfx::setTexture(0, i_->sampler, i_->colorTex);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(kComposeView, i_->compose);
}

bool DirectRenderHost::shouldQuit() const {
    return i_->quit || ge::shouldQuit();
}

} // namespace ge
