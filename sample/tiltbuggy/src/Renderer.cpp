// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// T38 spike: sokol_gfx port of the tiltbuggy renderer.
//
// One sokol pipeline (vertex layout pos3 + color4-unorm) drives the solid-
// color mesh pass (playfield rect + surface rects + buggy chassis). The SVG
// sprites (pond, title, buyPro) go through ge::Sprite::draw with an MVP
// composed from the orthographic projection × per-sprite frame() matrix.

#include "Renderer.h"
#include "Scene.h"

#include <ge/FileIO.h>
#include <ge/Linalg.h>
#include <ge/debug.h>
#include <ge/iap.h>
#include <ge/ortho.h>
#include <ge/sprite.h>
#include <ge/svg.h>
#include <ge/transform.h>

#include "sokol_gfx.h"

// sokol-shdc bakes the vertex+fragment shader bytecode for every backend
// into one header. The factory `simple_shader_desc(sg_query_backend())`
// returns an `sg_shader_desc` pointing at the right variant for the
// active backend; the consts `ATTR_simple_*`, `UB_vs_params`, and the
// `vs_params_t` struct are also defined there.
#include "simple.h"  // sokol-shdc generated; -I via Module.mk

#include <cmath>
#include <cstdio>
#include <cstring>
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
    float    x, y, z;
    uint32_t abgr;
};

// Stream vertex buffer sized for the renderer's per-frame mesh load.
// tiltbuggy emits ≤ 6 verts × (background + few surfaces + buggy) — far
// under 4096, but the headroom keeps future surface-density bumps cheap.
constexpr int kStreamBufferBytes = 4096 * int(sizeof(PosColorVertex));

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

// Pack 0xRRGGBB into ABGR (alpha=0xFF) for SG_VERTEXFORMAT_UBYTE4N.
constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) {
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// Hand-rolled orthographic projection matching the Metal/Vulkan NDC convention
// sokol expects: x,y in [-1,+1], z in [0,1] (NOT GL's [-1,+1] homogeneous-depth
// convention). Column-major linalg::float4x4 layout to match `ge::frame`.
//
// Maps (l..r, b..t, near..far) → ([-1,+1], [-1,+1], [0,1]).
ge::la::float4x4 orthoMetal(float l, float r, float b, float t, float zn, float zf) {
    const float rl = 1.f / (r - l);
    const float tb = 1.f / (t - b);
    const float fn = 1.f / (zf - zn);
    return ge::la::float4x4{
        { 2.f * rl,         0.f,              0.f,         0.f },
        { 0.f,               2.f * tb,        0.f,         0.f },
        { 0.f,               0.f,             -1.f * fn,   0.f },
        { -(r + l) * rl,    -(t + b) * tb,    -zn * fn,    1.f },
    };
}

} // namespace

// BUY PRO button screen rect in pt. Lives at the top-left corner of the
// UI safe rect so it never sits under the camera notch / Dynamic Island.
// Units must match the pt-space hit-test in main.cpp's onEvent and the
// pt-denominated pxToWorldX/Y math in Renderer's draw path.
ge::Rect proButtonRect(const ge::Context& c) {
    const auto safe = c.uiSafeRectInPts();
    const float pad = 16.f;
    const float bw  = 85.f * 3.0f;
    const float bh  = 27.f * 3.0f;
    return {safe.x + pad, safe.y + pad, bw, bh};
}

struct Renderer::Impl {
    sg_shader   shader   = {};
    sg_pipeline pipeline = {};
    sg_buffer   stream   = {};   // SG_USAGE_STREAM per-frame vertex buffer
    bool        ready    = false;
    ge::Sprite  pond;
    ge::Sprite  title;
    ge::Sprite  buyPro;
    float       diagSpin       = 0.f;   // 🎯T89 render-liveness accumulator
    bool        diagnosticSpin = true;  // 🎯T124 off for deterministic renders
};

Renderer::Renderer() : i_(std::make_unique<Impl>()) {}

void Renderer::setDiagnosticSpin(bool on) { i_->diagnosticSpin = on; }

Renderer::~Renderer() {
    if (i_->pipeline.id != SG_INVALID_ID) sg_destroy_pipeline(i_->pipeline);
    if (i_->stream.id   != SG_INVALID_ID) sg_destroy_buffer(i_->stream);
    if (i_->shader.id   != SG_INVALID_ID) sg_destroy_shader(i_->shader);

    auto destroySprite = [](ge::Sprite& s) {
        if (s.tex.id  != SG_INVALID_ID) sg_destroy_image(s.tex);
        if (s.view.id != SG_INVALID_ID) sg_destroy_view(s.view);
    };
    destroySprite(i_->pond);
    destroySprite(i_->title);
    destroySprite(i_->buyPro);
}

void Renderer::init(const char* /*shaderDir*/) {
    // SVG rasterization is platform-agnostic and can run before sokol is
    // ready — uploadPixels inside svg.cpp does `sg_make_image` which needs
    // sg_isvalid(). ge::run sets up sokol before invoking the factory, so
    // by the time init() runs here, sokol is live.
    i_->pond   = ge::rasterizeSvg(kIcyPondSvg, 384, 256);
    i_->title  = ge::rasterizeSvg(kTitleSvg,   768, 128);
    i_->buyPro = ge::rasterizeSvg(kBuyProSvg,  256, 80);

    // Pipeline / stream buffer construction is deferred to first draw —
    // sg_isvalid() guard there keeps headless-test / pre-context-init
    // paths safe. Equivalent to src/sprite.cpp's ensureState pattern.
}

void Renderer::drawFrame(const Scene& scene, const ge::Context& c,
                         float /*tiltX*/, float /*tiltY*/) {
    if (!sg_isvalid()) return;

    // 🎯T101 Open this frame's swapchain pass — held for the rest of drawFrame;
    // its destructor (sg_end_pass + commit + present) runs when this returns.
    // All draws below (solid mesh, sprites, ge::debug flush) land inside it.
    auto pass = c.swapchainPass();

    // Lazy pipeline + stream buffer init (mirrors src/sprite.cpp's
    // ensureState). One-shot per renderer instance.
    if (!i_->ready) {
        i_->shader = sg_make_shader(simple_shader_desc(sg_query_backend()));

        sg_pipeline_desc pd{};
        pd.shader = i_->shader;
        pd.layout.attrs[ATTR_simple_a_position].format = SG_VERTEXFORMAT_FLOAT3;
        pd.layout.attrs[ATTR_simple_a_color0].format   = SG_VERTEXFORMAT_UBYTE4N;
        pd.primitive_type           = SG_PRIMITIVETYPE_TRIANGLES;
        pd.index_type               = SG_INDEXTYPE_NONE;
        pd.cull_mode                = SG_CULLMODE_NONE;
        pd.colors[0].blend.enabled  = false;  // solid mesh, no alpha
        pd.label                    = "tiltbuggy.simple.pipeline";
        i_->pipeline = sg_make_pipeline(&pd);

        sg_buffer_desc bd{};
        bd.size                  = kStreamBufferBytes;
        bd.usage.vertex_buffer   = true;
        bd.usage.stream_update   = true;
        bd.label                 = "tiltbuggy.simple.stream";
        i_->stream = sg_make_buffer(&bd);

        i_->ready = true;
    }

    // The host's beginFrame/endFrame handles viewport + clear (sokol has
    // no per-view-state equivalent of bgfx::setViewRect / setViewClear).
    // The host's composite pass applies viewport tilt; this renderer just
    // draws a flat top-down view.

    auto surf = c.fullRectInPts();

    // Orthographic projection: fit [-he,+he] in the shorter axis of
    // the FULL surface, so world-space coords map uniformly across the
    // whole backbuffer (including chrome).
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

    const ge::la::float4x4 proj = orthoMetal(
        -orthoW, orthoW,
        -orthoH, orthoH,
        -1.0f, 1.0f);

    // 🎯T94 Tilt the *playfield* in perspective as a surrogate for physical
    // device tilt. presentationTilt() is AccelSynth's contribution — nonzero
    // only on synth platforms (desktop / sim / emu), {0,0} on a real device
    // (whose screen is already physically tilted, so this is identity there).
    // Composed onto the base projection; the GPU's w-divide does the
    // foreshortening. Applied to the scene (field, buggy, pond, title) but NOT
    // the BUY PRO button — that stays axis-aligned so it lines up with its
    // screen-space hit-test in main.cpp.
    const ge::la::float4x4 projTilt =
        ge::la::mul(ge::ortho::tilt(aspect, c.presentationTilt()), proj);

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
    // 🎯T89 render-liveness indicator: a constant slow spin advanced once
    // per rendered frame. If the buggy stops turning, the render loop has
    // stalled (e.g. a Metal command-buffer wedge) — an at-a-glance signal
    // that beats reattaching a logger. Purely visual; does not touch physics.
    if (i_->diagnosticSpin) i_->diagSpin += 0.01f;  // ~one rev/~10s at 60fps
    pushRotatedRect(verts,
        ge::Rect{pose.x - hw, pose.y - hh, 2.f * hw, 2.f * hh},
        pose.angle + i_->diagSpin,
        buggyColor);

    // --- Submit the solid-color mesh ---
    // sokol pattern: append into the stream buffer, bind, set uniforms, draw.
    // No per-frame buffer alloc (the stream is reused; sg_append_buffer
    // returns the byte offset of this slice).
    const int vertCount = static_cast<int>(verts.size());
    if (vertCount > 0) {
        sg_apply_pipeline(i_->pipeline);

        sg_range vr{ .ptr = verts.data(), .size = size_t(vertCount) * sizeof(PosColorVertex) };
        const int offset = sg_append_buffer(i_->stream, &vr);

        sg_bindings b{};
        b.vertex_buffers[0]        = i_->stream;
        b.vertex_buffer_offsets[0] = offset;
        sg_apply_bindings(&b);

        // Identity model matrix — vertices already in world space — so the
        // MVP collapses to just the projection.
        vs_params_t vsp;
        std::memcpy(vsp.u_modelViewProj, &projTilt[0][0], sizeof(vsp.u_modelViewProj));
        sg_range up{ .ptr = &vsp, .size = sizeof(vsp) };
        sg_apply_uniforms(UB_vs_params, &up);

        sg_draw(0, vertCount, 1);
    }

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
            const auto model = ge::frame(ge::Rect{
                s.rect.x - pw,
                s.rect.y + s.rect.h + ph,        // top in y-up = bottom + height
                s.rect.w + 2.f * pw,
                -(s.rect.h + 2.f * ph),           // negative for y-up
            });
            i_->pond.draw(ge::la::mul(projTilt, model));
        }
    }

    // Title — sits above the pond, between iceT (0.375) and the top of the
    // playfield (halfExtent ~ 0.625). 6:1 aspect to match the SVG viewBox.
    if (!i_->title.isNull()) {
        const float titleHe     = scene.halfExtent();
        const float titleTop    = titleHe - 0.04f * titleHe;  // small margin from top wall
        const float titleHeight = 0.18f * titleHe;
        const float titleWidth  = titleHeight * 6.0f;          // 6:1 aspect
        const auto model = ge::frame(ge::Rect{
            -titleWidth * 0.5f,
            titleTop,                                 // y = top in y-up
            titleWidth,
            -titleHeight,                             // h NEGATIVE for y-up
        });
        i_->title.draw(ge::la::mul(projTilt, model));
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
        const auto model = ge::frame(ge::Rect{wL, wT, wW, -wH});
        i_->buyPro.draw(ge::la::mul(proj, model));
    }

    // ── 🎯T97 debug-overlay demo ────────────────────────────────────────
    // Opt in with GE_DEBUG_OVERLAY=1 (or ge::debug::setEnabled(true)). While
    // off, every call below is a no-op and flush() does nothing, so this stays
    // unconditional — the flag toggles the whole overlay at runtime with no
    // change to the draw code above. Exercises all three ge::debug surfaces:
    //   • fillMesh + wireMesh — translucent fill + outline over the buggy chassis
    //   • line     — playfield border outline
    //   • text     — a pixel-space HUD line
    {
        const uint16_t quad[6] = {0, 1, 2, 0, 2, 3};

        // Buggy chassis quad — same rotated quad the solid-colour pass drew.
        const float a = pose.angle + i_->diagSpin;
        const float ca = std::cos(a), sa = std::sin(a);
        auto corner = [&](float ox, float oy) {
            return ge::la::float2{pose.x + ox * ca - oy * sa,
                                  pose.y + ox * sa + oy * ca};
        };
        const ge::la::float2 chassis[4] = {
            corner(-hw, -hh), corner(hw, -hh), corner(hw, hh), corner(-hw, hh),
        };
        // Magenta wireframe + translucent fill, scoped to the buggy — the
        // classic "debug shape" look, both layers in one mesh() call.
        ge::debug::mesh(chassis, quad, ge::debug::kWireColor, ge::debug::kFillColor);

        // Playfield border via box(), buggy position marker via circle() —
        // the convenience shapes, wire-only (default colour).
        ge::debug::box(ge::Rect{bgL, bgB, bgR - bgL, bgT - bgB});
        ge::debug::circle({pose.x, pose.y}, hw * 1.6f);

        const auto t = c.presentationTilt();
        char hud[64];
        std::snprintf(hud, sizeof hud, "tiltbuggy debug  tilt=(%.2f, %.2f)",
                      t.x, t.y);
        ge::debug::text({12.0f, 12.0f}, hud);

        // World primitives ride the same projTilt as the scene; pixel-space
        // text uses the surface size from `c`.
        ge::debug::flush(c, projTilt);
    }
}

} // namespace tiltbuggy
