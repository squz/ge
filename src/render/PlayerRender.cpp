// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include <ge/PlayerRender.h>
#include <ge/Protocol.h>

#include "../../tools/player_orientation.h"

#include <SDL3/SDL.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

namespace ge {

struct PlayerRender::Impl {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* videoTex = nullptr;
    int texW = 0, texH = 0;
    SDL_PixelFormat texFormat = SDL_PIXELFORMAT_UNKNOWN;

    uint8_t requestedOrientation = 0;

    // Desktop windows auto-size to the video's aspect on first frame (and on
    // stream-dimension change) so a landscape game gets a landscape window
    // instead of letterboxing inside the portrait default. Mobile players are
    // borderless-fullscreen; their shape is the device's.
    bool autoSizeToVideo = false;

    // Real accelerometer, if the player device has one (phone/tablet player).
    // Its events forward upstream verbatim. There is deliberately NO synthetic
    // fallback here: the player is a dumb peripheral — tilt-gesture input
    // (Shift+drag) forwards raw to the server, where the ENGINE synthesizes
    // sensor events and applies presentation tilt (🎯T94) in the game's own
    // projection. Games only ever see SDL_EVENT_SENSOR_UPDATE, real or
    // synthetic; that contract is the engine's, not the player's.
    SDL_Sensor* accelSensor = nullptr;

    // True when the window and the video disagree on which axis is longer —
    // e.g. landscape stream on a portrait tablet (system ignored our
    // orientation lock; Pixel Tablet ignoreOrientationRequest). We rotate
    // the texture 90° to fill the short axis instead of letterboxing.
    static bool needsTexWindowRotation(int ww, int wh, int tw, int th) {
        if (tw <= 0 || th <= 0 || ww <= 0 || wh <= 0) return false;
        const bool winLandscape = ww > wh;
        const bool texLandscape = tw > th;
        return winLandscape != texLandscape;
    }

    // Coordinate-map a window-pixel (sx, sy) to video-texture space.
    // Accounts for aspect-fit scaling and 90° tex↔window rotation.
    void mapToTexture(float sx, float sy, float& ox, float& oy) const {
        if (!videoTex) { ox = sx; oy = sy; return; }
        int ww, wh;
        SDL_GetWindowSizeInPixels(window, &ww, &wh);
        const bool rotated = needsTexWindowRotation(ww, wh, texW, texH);

        float visW, visH;
        if (rotated) {
            // Post-rotation on-screen size is (texH × texW) aspect.
            const float s = std::min(float(ww) / float(texH),
                                     float(wh) / float(texW));
            visW = texH * s;
            visH = texW * s;
        } else {
            const float s = std::min(float(ww) / float(texW),
                                     float(wh) / float(texH));
            visW = texW * s;
            visH = texH * s;
        }
        const float offX = (ww - visW) * 0.5f;
        const float offY = (wh - visH) * 0.5f;
        const float nx = (sx - offX) / visW;
        const float ny = (sy - offY) / visH;
        if (rotated) {
            // Inverse of +90° CW render (see render()): screen → texture.
            // Portrait window + landscape tex: rotate so tex +x maps up-screen.
            if (wh > ww) {
                ox = ny * texW;
                oy = (1.f - nx) * texH;
            } else {
                // Landscape window + portrait tex (existing path, −90°).
                ox = (1.f - ny) * texW;
                oy = nx * texH;
            }
        } else {
            ox = nx * texW;
            oy = ny * texH;
        }
    }

    // Rewrite event coordinates in-place to server-space. Relative motion
    // (xrel/yrel) stays in raw window pixels: it feeds the server-side
    // AccelSynth, whose tilt scale is calibrated in pixels of hand movement —
    // a human quantity, not a texture-space one.
    void mapEvent(SDL_Event& e) const {
        if (!videoTex) return;
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            mapToTexture(e.motion.x, e.motion.y, e.motion.x, e.motion.y);
        } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                   e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            mapToTexture(e.button.x, e.button.y, e.button.x, e.button.y);
        } else if (e.type == SDL_EVENT_FINGER_DOWN ||
                   e.type == SDL_EVENT_FINGER_UP ||
                   e.type == SDL_EVENT_FINGER_MOTION) {
            int ww, wh;
            SDL_GetWindowSizeInPixels(window, &ww, &wh);
            const float px = e.tfinger.x * ww;
            const float py = e.tfinger.y * wh;
            mapToTexture(px, py, e.tfinger.x, e.tfinger.y);
        }
    }
};

PlayerRender::PlayerRender(const Config& cfg)
    : i_(std::make_unique<Impl>()) {
    i_->requestedOrientation = cfg.orientation;
    i_->autoSizeToVideo = !cfg.borderless;

    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (cfg.borderless) flags |= SDL_WINDOW_BORDERLESS;

    i_->window = SDL_CreateWindow("GE Player",
                                  cfg.initialW, cfg.initialH, flags);
    if (!i_->window) {
        SPDLOG_ERROR("PlayerRender: SDL_CreateWindow failed: {}", SDL_GetError());
        return;
    }
    playerForceOrientation(i_->requestedOrientation);

    i_->renderer = SDL_CreateRenderer(i_->window, nullptr);
    if (!i_->renderer) {
        SPDLOG_ERROR("PlayerRender: SDL_CreateRenderer failed: {}", SDL_GetError());
    }
}

PlayerRender::~PlayerRender() {
    if (i_->accelSensor) SDL_CloseSensor(i_->accelSensor);
    if (i_->videoTex)    SDL_DestroyTexture(i_->videoTex);
    if (i_->renderer)    SDL_DestroyRenderer(i_->renderer);
    if (i_->window)      SDL_DestroyWindow(i_->window);
}

SDL_Window* PlayerRender::window() const { return i_->window; }

void PlayerRender::enableAccelerometer() {
    // Open a real sensor if the player device has one; its events forward
    // upstream. No sensor is fine — Shift+drag forwards raw to the server,
    // whose engine-side AccelSynth synthesizes (see Impl::accelSensor note).
    int count = 0;
    SDL_SensorID* sensors = SDL_GetSensors(&count);
    if (sensors) {
        for (int k = 0; k < count; k++) {
            if (SDL_GetSensorTypeForID(sensors[k]) == SDL_SENSOR_ACCEL) {
                i_->accelSensor = SDL_OpenSensor(sensors[k]);
                if (i_->accelSensor) {
                    SPDLOG_INFO("PlayerRender: opened real accelerometer");
                    break;
                }
            }
        }
        SDL_free(sensors);
    }
    if (!i_->accelSensor) {
        SPDLOG_INFO("PlayerRender: no local accelerometer — tilt gestures "
                    "(Shift+drag) forward to the server's synthesizer");
    }
}

void PlayerRender::getDeviceDimensions(int& w, int& h, int& pixelRatio) const {
    const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    w  = dm ? dm->w : 1080;
    h  = dm ? dm->h : 2400;
    pixelRatio = (dm && dm->pixel_density > 0) ? int(dm->pixel_density) : 1;

    const bool wantPortrait  = (i_->requestedOrientation == wire::kOrientationPortrait ||
                                i_->requestedOrientation == wire::kOrientationPortraitFlipped);
    // AnyLandscape is a landscape lock (either side); DeviceInfo must report
    // landscape dims so the server never treats a portrait tablet as the
    // game's intended surface.
    const bool wantLandscape = (i_->requestedOrientation == wire::kOrientationLandscape ||
                                i_->requestedOrientation == wire::kOrientationLandscapeFlipped ||
                                i_->requestedOrientation == wire::kOrientationAnyLandscape);
    if (wantPortrait  && w > h) std::swap(w, h);
    if (wantLandscape && h > w) std::swap(w, h);
}

void PlayerRender::updateVideoTexture(const VideoFrame& frame) {
    if (!i_->renderer) return;

    SDL_PixelFormat sdlFormat = SDL_PIXELFORMAT_UNKNOWN;
    switch (frame.format) {
    case VideoFrame::Format::BGRA: sdlFormat = SDL_PIXELFORMAT_BGRA32; break;
    case VideoFrame::Format::NV12: sdlFormat = SDL_PIXELFORMAT_NV12;   break;
    case VideoFrame::Format::IYUV: sdlFormat = SDL_PIXELFORMAT_IYUV;   break;
    }
    if (sdlFormat == SDL_PIXELFORMAT_UNKNOWN) return;

    if (!i_->videoTex || i_->texW != frame.width ||
        i_->texH != frame.height || i_->texFormat != sdlFormat) {
        if (i_->videoTex) SDL_DestroyTexture(i_->videoTex);
        i_->videoTex = SDL_CreateTexture(i_->renderer, sdlFormat,
            SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
        i_->texW = frame.width;
        i_->texH = frame.height;
        i_->texFormat = sdlFormat;
        SPDLOG_INFO("PlayerRender: video texture created {}x{} format={}",
                    frame.width, frame.height, SDL_GetPixelFormatName(sdlFormat));

        // Match the desktop window to the video's shape: points = video pixels
        // scaled down by the window's pixel density (a 2048×1536 stream from a
        // 2× HiDPI server displays 1:1 at 1024×768 points), capped to 85% of
        // the desktop so big streams still fit on small displays.
        if (i_->autoSizeToVideo) {
            const float pd = std::max(1.0f, SDL_GetWindowPixelDensity(i_->window));
            float w = frame.width / pd;
            float h = frame.height / pd;
            const SDL_DisplayMode* dm =
                SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
            if (dm && dm->w > 0 && dm->h > 0) {
                const float fit = std::min(1.0f,
                    std::min(0.85f * dm->w / w, 0.85f * dm->h / h));
                w *= fit;
                h *= fit;
            }
            SDL_SetWindowSize(i_->window, int(w), int(h));
            SDL_SetWindowPosition(i_->window,
                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
    }

    switch (frame.format) {
    case VideoFrame::Format::BGRA:
        SDL_UpdateTexture(i_->videoTex, nullptr, frame.planes[0], frame.strides[0]);
        break;
    case VideoFrame::Format::NV12:
        // Y plane + interleaved UV plane.
        SDL_UpdateNVTexture(i_->videoTex, nullptr,
                            frame.planes[0], frame.strides[0],
                            frame.planes[1], frame.strides[1]);
        break;
    case VideoFrame::Format::IYUV:
        // Separate Y, U, V planes.
        SDL_UpdateYUVTexture(i_->videoTex, nullptr,
                             frame.planes[0], frame.strides[0],
                             frame.planes[1], frame.strides[1],
                             frame.planes[2], frame.strides[2]);
        break;
    }
}

PlayerRender::PumpResult PlayerRender::pumpEvents() {
    PumpResult r;
    SDL_Event e;
    SDL_Event lastMotion{};
    bool hasMotion = false;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) { r.quit = true; continue; }

        switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_FINGER_MOTION:
            i_->mapEvent(e);
            // Coalesce to one motion per pump, but SUM the relative deltas —
            // the server-side AccelSynth accumulates xrel/yrel, so dropping
            // intermediate deltas would under-rotate the tilt.
            if (hasMotion && e.type == SDL_EVENT_MOUSE_MOTION &&
                lastMotion.type == SDL_EVENT_MOUSE_MOTION) {
                e.motion.xrel += lastMotion.motion.xrel;
                e.motion.yrel += lastMotion.motion.yrel;
            }
            lastMotion = e;
            hasMotion = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_FINGER_DOWN:
        case SDL_EVENT_FINGER_UP:
            i_->mapEvent(e);
            r.upstreamEvents.push_back(e);
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_SENSOR_UPDATE:
            r.upstreamEvents.push_back(e);
            break;
        }
    }
    if (hasMotion) r.upstreamEvents.push_back(lastMotion);
    return r;
}

PlayerRender::RenderStats PlayerRender::render() {
    RenderStats s;
    if (!i_->renderer) return s;

    const uint64_t tDrainStart = SDL_GetPerformanceCounter();

    SDL_SetRenderDrawColor(i_->renderer, 0, 0, 0, 255);
    SDL_RenderClear(i_->renderer);

    if (i_->videoTex) {
        int ww, wh;
        SDL_GetWindowSizeInPixels(i_->window, &ww, &wh);
        const bool needsRotation =
            Impl::needsTexWindowRotation(ww, wh, i_->texW, i_->texH);

        if (needsRotation) {
            // SDL_RenderTextureRotated rotates about the dest rect centre.
            // Dest is pre-rotation tex size, scaled so the post-rotation
            // axis-aligned bbox fills the window (contain).
            const float scale = std::min(float(ww) / float(i_->texH),
                                         float(wh) / float(i_->texW));
            const float dstW = i_->texW * scale;
            const float dstH = i_->texH * scale;
            SDL_FRect dst{ (ww - dstW) * 0.5f, (wh - dstH) * 0.5f,
                           dstW, dstH };
            // Portrait window + landscape stream: +90° fills the short axis
            // (system letterboxing on tablets that ignore orientation locks).
            // Landscape window + portrait stream: −90° (original path).
            const double deg = (wh > ww) ? 90.0 : -90.0;
            SDL_RenderTextureRotated(i_->renderer, i_->videoTex,
                                     nullptr, &dst,
                                     deg, nullptr, SDL_FLIP_NONE);
        } else {
            const float scale = std::min(float(ww) / float(i_->texW),
                                         float(wh) / float(i_->texH));
            const float visW = i_->texW * scale;
            const float visH = i_->texH * scale;
            SDL_FRect dst{ (ww - visW) * 0.5f, (wh - visH) * 0.5f,
                           visW, visH };
            SDL_RenderTexture(i_->renderer, i_->videoTex, nullptr, &dst);
        }
    }

    const uint64_t tPresentStart = SDL_GetPerformanceCounter();
    SDL_RenderPresent(i_->renderer);
    const uint64_t tEnd = SDL_GetPerformanceCounter();
    const uint64_t freq = SDL_GetPerformanceFrequency();
    s.drainMs  = float(tPresentStart - tDrainStart) * 1000.f / float(freq);
    s.renderMs = float(tEnd - tPresentStart) * 1000.f / float(freq);
    return s;
}

} // namespace ge
