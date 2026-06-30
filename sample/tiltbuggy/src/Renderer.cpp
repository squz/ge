// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T137 TiltBuggy renderer — an exact-match port of the 2013 game's look:
// a tiled-asphalt arena with rectangular ice + dirt patches and the buggy
// sprite on top, drawn from the original textures (data/{asphalt,ice,dirt,
// buggy}.png). No title, no buttons, no recolour, no spin — the engine
// showcases live elsewhere (reintroduce later, 🎯T137.6).

#include "Renderer.h"
#include "Scene.h"

#include <ge/Linalg.h>
#include <ge/Resource.h>
#include <ge/debug.h>
#include <ge/ortho.h>
#include <ge/png.h>
#include <ge/sprite.h>
#include <ge/transform.h>

#include "sokol_gfx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tiltbuggy {

namespace {

// World size of one asphalt/ice/dirt texture tile (the original tiled ~every
// 2 world units; asphalt.png etc. are seamless). Tiles are batched.
constexpr float kTileWorld = 4.0f;

// 🎯T137.3 Camera (matches the 2013 ViewController). Phone: 2× zoom following
// the buggy with a 0.6 lead (orig iPhone: ortho ±VIEW_SIZE/2 × translate
// -0.6·pos), so the buggy visibly traverses the arena. Tablet/desktop: the
// whole arena (orig iPad).
constexpr float kPhoneViewFrac = 0.5f;   // show half the arena half-extent
constexpr float kFollowLead    = 0.6f;   // camera leads the buggy at this rate

// Metal/Vulkan-NDC orthographic projection: (l..r, b..t, near..far) →
// ([-1,+1], [-1,+1], [0,1]). Column-major to match ge::frame.
ge::la::float4x4 orthoMetal(float l, float r, float b, float t, float zn, float zf) {
    const float rl = 1.f / (r - l), tb = 1.f / (t - b), fn = 1.f / (zf - zn);
    return ge::la::float4x4{
        { 2.f * rl,      0.f,           0.f,       0.f },
        { 0.f,           2.f * tb,      0.f,       0.f },
        { 0.f,           0.f,          -1.f * fn,  0.f },
        { -(r + l) * rl, -(t + b) * tb, -zn * fn,  1.f },
    };
}

} // namespace

struct Renderer::Impl {
    ge::Sprite      asphalt;   // tiled arena floor
    ge::Sprite      ice;       // tiled ice patch
    ge::Sprite      dirt;      // tiled dirt patch
    ge::Sprite      buggy;     // the car
    ge::SpriteBatch batch;     // reused per frame
};

Renderer::Renderer() : i_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;  // owning ge::Sprites free themselves (🎯T135)

void Renderer::init(const char* /*shaderDir*/) {
    // ge::run sets up sokol before the factory runs, so uploadPixels is valid.
    // ge::resource bridges the path (project-root on desktop, APK assets on
    // Android, app bundle on iOS); the textures live in data/ (synced into the
    // Android APK by the gradle syncAssets task).
    i_->asphalt = ge::loadImage(ge::resource("data/asphalt.png"));
    i_->ice     = ge::loadImage(ge::resource("data/ice.png"));
    i_->dirt    = ge::loadImage(ge::resource("data/dirt.png"));
    i_->buggy   = ge::loadImage(ge::resource("data/buggy.png"));
}

void Renderer::drawFrame(const Scene& scene, const ge::Context& c,
                         float /*tiltX*/, float /*tiltY*/) {
    if (!sg_isvalid()) return;

    // 🎯T101 Open the swapchain pass — ends/commits/presents on scope exit.
    auto pass = c.swapchainPass();

    auto surf = c.fullRectInPts();
    const float aspect = (surf.h > 0)
        ? static_cast<float>(surf.w) / static_cast<float>(surf.h) : 1.0f;
    const Pose  pose   = scene.buggyPose();
    const float he     = scene.halfExtent();

    // Camera.
    const bool  followCam = c.deviceClass() == ge::DeviceClass::Phone;
    const float viewHe    = followCam ? he * kPhoneViewFrac : he;
    float orthoW, orthoH;
    if (aspect >= 1.0f) { orthoH = viewHe; orthoW = viewHe * aspect; }
    else                { orthoW = viewHe; orthoH = viewHe / aspect; }
    const float camX = followCam ? kFollowLead * pose.x : 0.0f;
    const float camY = followCam ? kFollowLead * pose.y : 0.0f;
    const ge::la::float4x4 proj = orthoMetal(
        camX - orthoW, camX + orthoW, camY - orthoH, camY + orthoH, -1.0f, 1.0f);
    // Presentation-tilt is identity on a real device (it's AccelSynth's desktop
    // surrogate); composing it keeps desktop/sim tilt working, no device effect.
    const ge::la::float4x4 mvp =
        ge::la::mul(ge::ortho::tilt(aspect, c.presentationTilt()), proj);

    // Tile a texture across a world region, clipping edge cells to the region
    // (uvSubRect) so a bounded patch doesn't overhang. Batched per texture.
    auto tileInto = [&](const ge::Sprite& spr, ge::Rect region, float tile) {
        if (spr.isNull()) return;
        const float x0 = std::floor(region.x / tile) * tile;
        const float y0 = std::floor(region.y / tile) * tile;
        for (float ty = y0; ty < region.y + region.h; ty += tile)
            for (float tx = x0; tx < region.x + region.w; tx += tile) {
                const float cx0 = std::max(tx, region.x);
                const float cx1 = std::min(tx + tile, region.x + region.w);
                const float cy0 = std::max(ty, region.y);
                const float cy1 = std::min(ty + tile, region.y + region.h);
                if (cx1 <= cx0 || cy1 <= cy0) continue;
                i_->batch.addSprite(
                    ge::frame(ge::Rect{cx0, cy0, cx1 - cx0, cy1 - cy0}), spr,
                    ge::Rect{(cx0 - tx) / tile, (cy0 - ty) / tile,
                             (cx1 - cx0) / tile, (cy1 - cy0) / tile});
            }
    };

    i_->batch.clear();

    // Asphalt — tiled across the camera's visible box (full-bleed).
    const float bgHalfW = orthoW * 1.4f, bgHalfH = orthoH * 1.4f;
    tileInto(i_->asphalt,
             ge::Rect{camX - bgHalfW, camY - bgHalfH, 2.f * bgHalfW, 2.f * bgHalfH},
             kTileWorld);

    // Ice + dirt — the real textures tiled across their (rectangular) patches.
    for (const auto& s : scene.surfaces()) {
        if (s.type == SurfaceType::Ice)  tileInto(i_->ice,  s.rect, kTileWorld);
        if (s.type == SurfaceType::Dirt) tileInto(i_->dirt, s.rect, kTileWorld);
    }

    // Buggy on top, at the chassis pose (texture faces +x = forward).
    if (!i_->buggy.isNull()) {
        const auto che = scene.chassisHalfExtents();
        i_->batch.addSprite(
            ge::frameRotated({pose.x, pose.y}, {2.f * che.x, 2.f * che.y},
                             pose.angle),
            i_->buggy);
    }

    i_->batch.submit(mvp);

    // 🎯T97 debug overlay — opt-in (GE_DEBUG_OVERLAY); a no-op while disabled.
    {
        const auto che = scene.chassisHalfExtents();
        ge::debug::box(ge::Rect{-he, -he, 2.f * he, 2.f * he});
        ge::debug::circle({pose.x, pose.y}, che.x * 1.2f);
        ge::debug::flush(c, mvp);
    }
}

} // namespace tiltbuggy
