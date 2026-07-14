// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T137 TiltBuggy renderer — port of the 2013 game's look: tiled asphalt
// arena with ice/dirt patches and the buggy sprite. Title + flowery SVG
// border at the top exercise cmdstream recipe verbs (🎯T128.7).

#include "Renderer.h"
#include "Scene.h"

#include <ge/FontLoader.h>
#include <ge/Linalg.h>
#include <ge/Resource.h>
#include <ge/debug.h>
#include <ge/ortho.h>
#include <ge/png.h>
#include <ge/sprite.h>
#include <ge/svg.h>
#include <ge/text.h>
#include <ge/transform.h>

#include "sokol_gfx.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>

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
// Title chrome raster density (px per logical pt); draw size = pixelSize / this.
constexpr float kTitlePpp      = 2.f;

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

// Flowery rectangular frame for the title banner (viewBox 0..400 × 0..90).
// Roses / leaves at corners + vine along the border — tests MakeSvg recipe.
constexpr const char* kTitleBorderSvg = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 400 90">
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="0">
      <stop offset="0%" stop-color="#3d2914"/>
      <stop offset="50%" stop-color="#6b4423"/>
      <stop offset="100%" stop-color="#3d2914"/>
    </linearGradient>
  </defs>
  <!-- banner plate -->
  <rect x="28" y="18" width="344" height="54" rx="12" ry="12"
        fill="url(#g)" fill-opacity="0.88" stroke="#c4a574" stroke-width="2"/>
  <!-- vines -->
  <path d="M40 30 C80 8, 120 8, 160 28 S240 50, 280 28 S360 8, 360 30"
        fill="none" stroke="#2d6a3a" stroke-width="2.2"/>
  <path d="M40 60 C90 78, 150 78, 200 58 S300 38, 360 60"
        fill="none" stroke="#2d6a3a" stroke-width="2.2"/>
  <!-- corner roses -->
  <g fill="#c23b4a">
    <circle cx="36" cy="22" r="9"/><circle cx="30" cy="18" r="5"/><circle cx="42" cy="18" r="5"/>
    <circle cx="364" cy="22" r="9"/><circle cx="358" cy="18" r="5"/><circle cx="370" cy="18" r="5"/>
    <circle cx="36" cy="68" r="9"/><circle cx="30" cy="72" r="5"/><circle cx="42" cy="72" r="5"/>
    <circle cx="364" cy="68" r="9"/><circle cx="358" cy="72" r="5"/><circle cx="370" cy="72" r="5"/>
  </g>
  <!-- leaves -->
  <g fill="#3d8f4a">
    <ellipse cx="55" cy="16" rx="10" ry="5" transform="rotate(-25 55 16)"/>
    <ellipse cx="345" cy="16" rx="10" ry="5" transform="rotate(25 345 16)"/>
    <ellipse cx="55" cy="74" rx="10" ry="5" transform="rotate(25 55 74)"/>
    <ellipse cx="345" cy="74" rx="10" ry="5" transform="rotate(-25 345 74)"/>
  </g>
  <!-- centers -->
  <circle cx="36" cy="22" r="3" fill="#f0d060"/>
  <circle cx="364" cy="22" r="3" fill="#f0d060"/>
  <circle cx="36" cy="68" r="3" fill="#f0d060"/>
  <circle cx="364" cy="68" r="3" fill="#f0d060"/>
</svg>
)SVG";

} // namespace

struct Renderer::Impl {
    ge::Sprite      asphalt;   // tiled arena floor
    ge::Sprite      ice;       // tiled ice patch
    ge::Sprite      dirt;      // tiled dirt patch
    ge::Sprite      buggy;     // the car
    ge::Sprite      titleBorder; // flowery SVG frame (cmdstream MakeSvg)
    ge::Sprite      titleText;   // "TiltBuggy" (cmdstream MakeText)
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

    // 🎯T128.7 test chrome — SVG border + FreeType title (recipe verbs on stream).
    const int borderW = static_cast<int>(400 * kTitlePpp);
    const int borderH = static_cast<int>(90 * kTitlePpp);
    i_->titleBorder = ge::rasterizeSvg(kTitleBorderSvg, borderW, borderH);
    try {
        ge::FontRef font = ge::resolveFont("system:sans-serif-bold");
        i_->titleText = ge::rasterizeText(
            "TiltBuggy", font, 36.f * kTitlePpp,
            ge::la::float4{0.98f, 0.92f, 0.78f, 1.f});
    } catch (const std::exception& e) {
        std::fprintf(stderr, "tiltbuggy: title font unavailable (%s)\n", e.what());
    }
}

void Renderer::drawFrame(const Scene& scene, const ge::Context& c,
                         float /*tiltX*/, float /*tiltY*/) {
    if (!sg_isvalid()) return;

    // 🎯T101 Open the swapchain pass — ends/commits/presents on scope exit.
    auto pass = c.swapchainPass();

    // Playfield metrics: same accessors on direct (local OS) and stream
    // (player DeviceInfo → content). No modality branch.
    //
    // drawSafeRect is the playfield edge (cutouts only). Under immersive with
    // no cutouts it is the full surface — that alone takes dirt/walls to the
    // glass edge of the content. Camera frames drawSafe; arena is built to the
    // same aspect (main.cpp). Title is content chrome → drawSafe too.
    const ge::Rect full = c.fullRectInPts();
    const ge::Rect draw = c.drawSafeRectInPts();
    const float aspect = (draw.h > 0.f)
        ? draw.w / draw.h
        : ((full.h > 0.f) ? full.w / full.h : 1.0f);
    const Pose  pose   = scene.buggyPose();
    const float he     = scene.halfExtent();
    const float hwid   = scene.halfWidth();

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
        if (spr.isNull() || region.w <= 0.f || region.h <= 0.f) return;
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

    // Ice + dirt — physics-surface rects (walls sized to drawSafe aspect).
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

    // ── Title (screen pts, y-down) — top of *draw*-safe ───────────────
    // Banner is content/decoration, not interactive chrome: it does not
    // need uiSafe clearance from system bars or gesture zones. Pin to
    // drawSafe (cutouts only; full surface on a Pixel).
    if (!i_->titleBorder.isNull() || !i_->titleText.isNull()) {
        const float bannerW = std::min(draw.w * 0.72f, 420.f);
        const float bannerH = bannerW * (90.f / 400.f);
        const float bx = draw.x + (draw.w - bannerW) * 0.5f;
        const float by = draw.y + 8.f;
        const ge::la::float4x4 titleMvp =
            orthoMetal(0.f, full.w, full.h, 0.f, -1.f, 1.f);

        i_->batch.clear();
        if (!i_->titleBorder.isNull()) {
            i_->batch.addSprite(ge::frame(ge::Rect{bx, by, bannerW, bannerH}),
                                i_->titleBorder);
        }
        if (!i_->titleText.isNull()) {
            // Rasterized at kTitlePpp density; draw at logical pt size.
            const float drawW = static_cast<float>(i_->titleText.width) / kTitlePpp;
            const float drawH = static_cast<float>(i_->titleText.height) / kTitlePpp;
            const float tx = bx + (bannerW - drawW) * 0.5f;
            const float ty = by + (bannerH - drawH) * 0.5f;
            i_->batch.addSprite(ge::frame(ge::Rect{tx, ty, drawW, drawH}),
                                i_->titleText);
        }
        i_->batch.submit(titleMvp);
    }

    // 🎯T97 debug overlay — opt-in (GE_DEBUG_OVERLAY); a no-op while disabled.
    {
        const auto che = scene.chassisHalfExtents();
        ge::debug::box(ge::Rect{-hwid, -he, 2.f * hwid, 2.f * he});
        ge::debug::circle({pose.x, pose.y}, che.x * 1.2f);
        ge::debug::flush(c, mvp);
    }
}

} // namespace tiltbuggy
