// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "Renderer.h"
#include "Scene.h"

#include <ge/FileIO.h>
#include <ge/iap.h>
#include <ge/sprite.h>
#include <ge/svg.h>
#include <ge/transform.h>

#include <bgfx/bgfx.h>
#include <bx/math.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

// Procedurally drawn icy pond. Irregular bezier border so the puddle
// doesn't look like a rectangle; radial gradient for the icy sheen;
// stroked cracks under a clip-path so they don't escape the border.
// clipPath + gradients are exactly the SVG features SDL_image's nanosvg
// path can't render — this is the smoke test that lunasvg is doing its
// job.
constexpr std::string_view kIcyPondSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="384" height="256" viewBox="0 0 384 256">
  <defs>
    <radialGradient id="ice" cx="0.45" cy="0.4" r="0.7">
      <stop offset="0%"   stop-color="#FFFFFF"/>
      <stop offset="35%"  stop-color="#E5F4FF"/>
      <stop offset="100%" stop-color="#7AAACE"/>
    </radialGradient>
    <linearGradient id="sheen" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%"   stop-color="#FFFFFF" stop-opacity="0.55"/>
      <stop offset="100%" stop-color="#FFFFFF" stop-opacity="0"/>
    </linearGradient>
    <clipPath id="pondClip">
      <path d="M 30,90
               C 18,52 80,18 150,28
               C 220,38 258,6 320,28
               C 380,50 366,140 344,182
               C 322,224 244,254 184,238
               C 124,222 56,238 32,182
               C 6,140 42,128 30,90 Z"/>
    </clipPath>
  </defs>

  <path d="M 30,90
           C 18,52 80,18 150,28
           C 220,38 258,6 320,28
           C 380,50 366,140 344,182
           C 322,224 244,254 184,238
           C 124,222 56,238 32,182
           C 6,140 42,128 30,90 Z"
        fill="url(#ice)"/>

  <ellipse cx="180" cy="80" rx="130" ry="34" fill="url(#sheen)"
           clip-path="url(#pondClip)"/>

  <g clip-path="url(#pondClip)" stroke="#FFFFFF" stroke-opacity="0.55"
     stroke-width="1.4" fill="none" stroke-linecap="round">
    <path d="M 80,60 L 200,140 L 160,205"/>
    <path d="M 220,40 L 250,118"/>
    <path d="M 100,180 L 180,158"/>
    <path d="M 280,90 L 332,150"/>
    <path d="M 60,150 L 120,148"/>
  </g>

  <path d="M 30,90
           C 18,52 80,18 150,28
           C 220,38 258,6 320,28
           C 380,50 366,140 344,182
           C 322,224 244,254 184,238
           C 124,222 56,238 32,182
           C 6,140 42,128 30,90 Z"
        fill="none" stroke="#4A7A9C" stroke-width="2.5" stroke-opacity="0.65"/>
</svg>)SVG";

// Buy-PRO button — shown when ge::iap::owned("pro") is false. Tapping
// triggers ge::iap::buy("pro") in main.cpp's onEvent (hit-test against
// the same screen-pixel rect via tiltbuggy::proButtonScreenRect).
constexpr std::string_view kBuyProSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="80" viewBox="0 0 256 80">
  <rect x="2" y="2" width="252" height="76" rx="14" fill="#FFCC33" stroke="#222222" stroke-width="3"/>
  <text x="128" y="52" font-family="sans-serif" font-weight="bold" font-size="34" text-anchor="middle" fill="#222222">BUY PRO</text>
</svg>)SVG";

// Game title rendered as SVG <text>. Exercises the lazy default-font path
// added in T42.4 — no app-side font setup, sans-serif comes from
// ge::resolveFont("system:sans-serif") on first rasterize. Bold on macOS
// is faux-bold (Apple ships Helvetica as a TTC; lunasvg's wrapper drops
// the face index — see ge/CLAUDE.md "Apple TTC limitation").
constexpr std::string_view kTitleSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="768" height="128" viewBox="0 0 768 128">
  <defs>
    <linearGradient id="titleFill" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%"   stop-color="#FFFFFF"/>
      <stop offset="55%"  stop-color="#D8EEFF"/>
      <stop offset="100%" stop-color="#A5C8E5"/>
    </linearGradient>
  </defs>
  <text x="384" y="92"
        font-family="sans-serif" font-weight="bold" font-size="86"
        text-anchor="middle"
        fill="url(#titleFill)" stroke="#1B3A5A" stroke-width="3"
        paint-order="stroke">TILT BUGGY</text>
</svg>)SVG";

} // namespace

namespace tiltbuggy {

namespace {

struct PosColorVertex {
    float x, y, z;
    uint32_t abgr;
};

bgfx::VertexLayout makeLayout() {
    bgfx::VertexLayout layout;
    layout.begin()
        .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
        .add(bgfx::Attrib::Color0,   4, bgfx::AttribType::Uint8, true)
        .end();
    return layout;
}

// Read a compiled shader .bin file into bgfx memory. Uses ge::openFile so
// the lookup goes through SDL_IOStream, which transparently handles APK
// asset reads on Android, iOS bundle resources, and plain filesystem on
// desktop.
bgfx::ShaderHandle loadShader(const char* shaderDir, const char* name) {
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s.bin", shaderDir, name);
    auto stream = ge::openFile(path, /*binary=*/true);
    if (!stream || !*stream) return BGFX_INVALID_HANDLE;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(*stream)),
                              std::istreambuf_iterator<char>());
    if (data.empty()) return BGFX_INVALID_HANDLE;
    const bgfx::Memory* mem = bgfx::alloc(static_cast<uint32_t>(data.size() + 1));
    std::memcpy(mem->data, data.data(), data.size());
    mem->data[data.size()] = '\0';
    return bgfx::createShader(mem);
}

// Append two triangles forming an axis-aligned rect (world space).
void pushRect(std::vector<PosColorVertex>& verts, ge::Rect r, uint32_t abgr) {
    const float x0 = r.x;
    const float x1 = r.x + r.w;
    const float y0 = r.y;
    const float y1 = r.y + r.h;
    verts.push_back({x0, y1, 0.0f, abgr});
    verts.push_back({x1, y1, 0.0f, abgr});
    verts.push_back({x1, y0, 0.0f, abgr});
    verts.push_back({x0, y1, 0.0f, abgr});
    verts.push_back({x1, y0, 0.0f, abgr});
    verts.push_back({x0, y0, 0.0f, abgr});
}

// Append two triangles forming a rotated rect: the AABB `rect` rotated by
// `angle` around its centre.
void pushRotatedRect(std::vector<PosColorVertex>& verts,
                     ge::Rect rect, float angle, uint32_t abgr) {
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const auto  centre = rect.center();
    const float hw = rect.w * 0.5f;
    const float hh = rect.h * 0.5f;
    // Four corners in local space: (±hw, ±hh)
    const float lx[4] = { -hw,  hw,  hw, -hw };
    const float ly[4] = { -hh, -hh,  hh,  hh };
    PosColorVertex v[4];
    for (int i = 0; i < 4; ++i) {
        v[i].x    = centre.x + lx[i] * c - ly[i] * s;
        v[i].y    = centre.y + lx[i] * s + ly[i] * c;
        v[i].z    = 0.0f;
        v[i].abgr = abgr;
    }
    verts.push_back(v[0]); verts.push_back(v[1]); verts.push_back(v[2]);
    verts.push_back(v[0]); verts.push_back(v[2]); verts.push_back(v[3]);
}

// Pack 0xRRGGBB into bgfx ABGR (alpha=0xFF).
constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) {
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

} // namespace

// BUY PRO button screen rect in pt. Lives at the top-left corner of the
// UI safe rect so it never sits under the camera notch / Dynamic Island.
// Size scales with pixelsPerPt so the tap target stays at ~85 × 27 pt
// (above Apple HIG's 44 pt minimum) on every device class.
ge::Rect proButtonRect(const ge::Context& c) {
    const auto safe = c.uiSafeRectInPts();
    const float pad = 16.f * c.pixelsPerPt();
    const float bw  = 85.f * 3.0f * c.pixelsPerPt();
    const float bh  = 27.f * 3.0f * c.pixelsPerPt();
    return {safe.x + pad, safe.y + pad, bw, bh};
}

struct Renderer::Impl {
    bgfx::VertexLayout  layout;
    bgfx::ProgramHandle program = BGFX_INVALID_HANDLE;
    ge::Sprite          pond;
    ge::Sprite          title;
    ge::Sprite          buyPro;
};

Renderer::Renderer() : i_(std::make_unique<Impl>()) {}
Renderer::~Renderer() {
    if (bgfx::isValid(i_->program))   bgfx::destroy(i_->program);
    if (bgfx::isValid(i_->pond.tex))  bgfx::destroy(i_->pond.tex);
    if (bgfx::isValid(i_->title.tex)) bgfx::destroy(i_->title.tex);
    if (bgfx::isValid(i_->buyPro.tex)) bgfx::destroy(i_->buyPro.tex);
}

void Renderer::init(const char* shaderDir) {
    i_->layout = makeLayout();

    bgfx::ShaderHandle vs = loadShader(shaderDir, "simple_vs");
    bgfx::ShaderHandle fs = loadShader(shaderDir, "simple_fs");
    i_->program = bgfx::createProgram(vs, fs, /*destroyShaders=*/true);

    // Rasterize the icy-pond SVG to a bgfx texture sized at 384×256 pixels
    // (matching the SVG viewBox and the ice patch's 1.5:1 world aspect).
    i_->pond = ge::rasterizeSvg(kIcyPondSvg, 384, 256);

    // Rasterize the "TILT BUGGY" title SVG. 768x128 = 6:1 aspect; we'll
    // place it in a similarly proportioned world rect above the pond.
    i_->title = ge::rasterizeSvg(kTitleSvg, 768, 128);

    // Rasterize the BUY PRO button (256x80). Drawn only when the pro
    // entitlement isn't owned; tap triggers ge::iap::buy("pro") in
    // main.cpp's onEvent.
    i_->buyPro = ge::rasterizeSvg(kBuyProSvg, 256, 80);
}

void Renderer::drawFrame(const Scene& scene, const ge::Context& c,
                         float /*tiltX*/, float /*tiltY*/) {
    // The host's composite pass applies viewport tilt (when synthesized
    // tilt is non-zero). The game just renders a flat top-down view.
    auto surf = c.fullRectInPts();
    bgfx::setViewRect(0, 0, 0,
                      static_cast<uint16_t>(surf.w * c.pixelsPerPt()),
                      static_cast<uint16_t>(surf.h * c.pixelsPerPt()));
    // Black outside the playfield so device chrome bleeds onto a clean
    // background rather than stale bgfx backbuffer. The safe rect just
    // tells us where to put the brown maze; effects that want to reach
    // into chrome (glows, particles) are still free to draw there.
    bgfx::setViewClear(0,
        BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
        0x000000ff, 1.0f, 0);

    // Orthographic projection: fit [-he,+he] in the shorter axis of
    // the FULL surface, so world-space coords map uniformly across the
    // whole bgfx backbuffer (including chrome).
    const float he     = scene.halfExtent();
    const float aspect = static_cast<float>(surf.w) / static_cast<float>(surf.h);
    float orthoW, orthoH;
    if (aspect >= 1.0f) {
        orthoH = he;
        orthoW = he * aspect;
    } else {
        orthoW = he;
        orthoH = he / aspect;
    }

    float proj[16];
    bx::mtxOrtho(proj,
        -orthoW, orthoW,
        -orthoH, orthoH,
        -1.0f, 1.0f, 0.0f,
        bgfx::getCaps()->homogeneousDepth);

    float view[16];
    bx::mtxIdentity(view);
    bgfx::setViewTransform(0, view, proj);

    std::vector<PosColorVertex> verts;
    verts.reserve(6 * (2 + scene.surfaces().size()));

    // Brown playfield placed at c.drawSafeRectInPts() — tiltbuggy is
    // touch-free, so the maze can extend into gesture zones (the wider
    // rect). World maps [-orthoW, +orthoW] to [0, surf.w] in pts.
    auto safeRect = c.drawSafeRectInPts();
    const float pxToWorldX = (surf.w > 0) ? (2.f * orthoW / float(surf.w)) : 0.f;
    const float pxToWorldY = (surf.h > 0) ? (2.f * orthoH / float(surf.h)) : 0.f;
    const float bgL = -orthoW + safeRect.x                * pxToWorldX;
    const float bgR = -orthoW + (safeRect.x + safeRect.w) * pxToWorldX;
    const float bgB =  orthoH - (safeRect.y + safeRect.h) * pxToWorldY;
    const float bgT =  orthoH - safeRect.y                * pxToWorldY;
    pushRect(verts, ge::Rect{bgL, bgB, bgR - bgL, bgT - bgB}, rgb(0xAA, 0x66, 0x44));

    // 2. Surface rects (excluding ice — drawn as a textured sprite below).
    // Both the dirt patch and the icy pond are gated behind the `pro`
    // IAP (🎯T65.7) — without it the buggy slides on featureless
    // asphalt; buying pro materialises the terrain features.
    const bool proOwned = ge::iap::owned("pro");
    if (proOwned) {
        for (const auto& s : scene.surfaces()) {
            uint32_t color;
            switch (s.type) {
                case SurfaceType::Ice:     continue;  // see ice-sprite pass below
                case SurfaceType::Dirt:    color = rgb(0xAA, 0x66, 0x44); break;
                case SurfaceType::Asphalt: continue;
                default:                   continue;
            }
            pushRect(verts, s.rect, color);
        }
    }

    // 3. Buggy — sized in proportion to the world (kept at 1/20 of the
    // arena half-extent to match the Box2D shape, so when the world is
    // shrunk to speed up physics, the rendered chassis tracks it).
    // Pro entitlement (🎯T65.7) flips the chassis paint from default
    // yellow to a chromed cyan — the visible gate for the IAP demo.
    const Pose pose = scene.buggyPose();
    const float hw = scene.halfExtent() * 0.05f;
    const float hh = hw * 0.5f;
    const uint32_t buggyColor = ge::iap::owned("pro")
        ? rgb(0x00, 0xE0, 0xFF)   // pro: chromed cyan
        : rgb(0xFF, 0xCC, 0x33);  // default: yellow
    pushRotatedRect(verts,
        ge::Rect{pose.x - hw, pose.y - hh, 2.f * hw, 2.f * hh},
        pose.angle,
        buggyColor);

    // --- Submit ---
    const uint32_t numVerts = static_cast<uint32_t>(verts.size());
    bgfx::TransientVertexBuffer tvb;
    bgfx::allocTransientVertexBuffer(&tvb, numVerts, i_->layout);
    std::memcpy(tvb.data, verts.data(), numVerts * sizeof(PosColorVertex));

    float identity[16];
    bx::mtxIdentity(identity);
    bgfx::setTransform(identity);

    bgfx::setVertexBuffer(0, &tvb, 0, numVerts);
    bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
    bgfx::submit(0, i_->program);

    // Ice-sprite pass — drawn after the color batch so the pond sits on top
    // of the dirt/background. Each ice surface gets a copy of the same
    // SVG-rasterized texture, inflated 25% past the collision rect so the
    // irregular bezier border has visible overhang past where the friction
    // actually changes.
    //
    // World is y-up (gravity points -y), so the rect's `y` is the rect's
    // TOP (larger y) and `h` is NEGATIVE, flipping the y basis so the
    // sprite's source-image top lands at world top.
    if (proOwned && !i_->pond.isNull()) {
        for (const auto& s : scene.surfaces()) {
            if (s.type != SurfaceType::Ice) continue;
            // Inflate 25% so the irregular bezier border has visible overhang
            // past the (rectangular) collision area.
            const float pw = s.rect.w * 0.125f;
            const float ph = s.rect.h * 0.125f;
            // y-up: rect.y is the bottom edge in scene's y-up world. To put the
            // sprite top-down upright on screen, frame() needs y at the TOP
            // (larger world y) and h NEGATIVE (basis points toward smaller y).
            const auto m = ge::frame(ge::Rect{
                s.rect.x - pw,
                s.rect.y + s.rect.h + ph,        // top in y-up = bottom + height
                s.rect.w + 2.f * pw,
                -(s.rect.h + 2.f * ph),           // negative for y-up
            });
            bgfx::setTransform(&m[0][0]);
            i_->pond.draw(0);
        }
    }

    // Title — sits above the pond, between iceT (0.375) and the top of the
    // playfield (halfExtent ~ 0.625). 6:1 aspect to match the SVG viewBox.
    if (!i_->title.isNull()) {
        const float he = scene.halfExtent();
        const float titleTop    = he - 0.04f * he;     // small margin from the top wall
        const float titleHeight = 0.18f * he;
        const float titleWidth  = titleHeight * 6.0f;  // 6:1 aspect
        const auto m = ge::frame(ge::Rect{
            -titleWidth * 0.5f,
            titleTop,                                 // y = top in y-up
            titleWidth,
            -titleHeight,                             // h NEGATIVE for y-up
        });
        bgfx::setTransform(&m[0][0]);
        i_->title.draw(0);
    }

    // BUY PRO button — only when pro isn't owned. Positioned in screen-
    // pixel coordinates inside the UI safe rect; tiltbuggy::proButtonRect
    // returns the same screen rect that main.cpp hit-tests in onEvent.
    if (!proOwned && !i_->buyPro.isNull()) {
        const auto btn = proButtonRect(c);
        // Screen → world conversion uses the same pxToWorldX/Y as the
        // background math above. World is y-up (frame() takes y=top with
        // h NEGATIVE to flip the basis); convert each axis accordingly.
        const float wL =  -orthoW + btn.x                  * pxToWorldX;
        const float wT =   orthoH - btn.y                  * pxToWorldY;  // y=top in y-up
        const float wW =                btn.w              * pxToWorldX;
        const float wH =                btn.h              * pxToWorldY;
        const auto m = ge::frame(ge::Rect{wL, wT, wW, -wH});
        bgfx::setTransform(&m[0][0]);
        i_->buyPro.draw(0);
    }
}

} // namespace tiltbuggy
