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

#include <algorithm>
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

// 🎯T137.4 Asphalt road tile — a dark macadam base with scattered aggregate
// speckles, tiled across the arena (orig asphalt.png, GL_REPEAT). Specks kept
// off the edges so the tiling seams stay subtle.
constexpr std::string_view kAsphaltSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <rect width="256" height="256" fill="#3b3b40"/>
  <g fill="#4a4a50">
    <circle cx="60"  cy="48"  r="6"/><circle cx="150" cy="38" r="4"/>
    <circle cx="206" cy="70"  r="7"/><circle cx="40"  cy="120" r="5"/>
    <circle cx="120" cy="110" r="8"/><circle cx="190" cy="140" r="5"/>
    <circle cx="86"  cy="170" r="6"/><circle cx="150" cy="190" r="7"/>
    <circle cx="210" cy="200" r="4"/><circle cx="58"  cy="214" r="5"/>
  </g>
  <g fill="#2f2f33">
    <circle cx="100" cy="60"  r="5"/><circle cx="176" cy="100" r="4"/>
    <circle cx="70"  cy="92"  r="4"/><circle cx="130" cy="150" r="5"/>
    <circle cx="40"  cy="180" r="4"/><circle cx="200" cy="170" r="6"/>
    <circle cx="158" cy="68"  r="3"/><circle cx="96"  cy="206" r="4"/>
  </g>
</svg>)SVG";

// 🎯T137.4 Dirt patch tile — loose brown earth with darker clods and lighter
// grit (orig dirt.png). Tiled across the dirt strip.
constexpr std::string_view kDirtSvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <rect width="256" height="256" fill="#6f4a2c"/>
  <g fill="#5a3a20">
    <ellipse cx="64"  cy="56"  rx="22" ry="14"/><ellipse cx="170" cy="44" rx="16" ry="11"/>
    <ellipse cx="206" cy="120" rx="20" ry="13"/><ellipse cx="48"  cy="150" rx="18" ry="12"/>
    <ellipse cx="128" cy="138" rx="24" ry="15"/><ellipse cx="190" cy="196" rx="17" ry="12"/>
    <ellipse cx="92"  cy="206" rx="20" ry="13"/>
  </g>
  <g fill="#86603d">
    <circle cx="110" cy="80" r="4"/><circle cx="156" cy="104" r="3"/>
    <circle cx="70"  cy="110" r="3"/><circle cx="180" cy="150" r="4"/>
    <circle cx="100" cy="170" r="3"/><circle cx="146" cy="186" r="4"/>
  </g>
</svg>)SVG";

// 🎯T137.4 Top-down buggy, facing +x (right = forward). Bodywork is white so
// SpriteBatch tints it (yellow default → cyan when `pro` is owned, the IAP
// showcase); wheels are black (tint-invariant), cockpit + nose are dark.
constexpr std::string_view kBuggySvg = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="512" height="256" viewBox="0 0 512 256">
  <g fill="#111111">
    <rect x="96"  y="18"  width="96" height="40" rx="12"/>
    <rect x="96"  y="198" width="96" height="40" rx="12"/>
    <rect x="320" y="18"  width="96" height="40" rx="12"/>
    <rect x="320" y="198" width="96" height="40" rx="12"/>
  </g>
  <path d="M 70,64 L 360,64 L 470,108 L 470,148 L 360,192 L 70,192
           Q 48,128 70,64 Z" fill="#ffffff" stroke="#222222" stroke-width="6"/>
  <rect x="150" y="92" width="120" height="72" rx="14" fill="#333842"/>
  <path d="M 360,92 L 452,118 L 452,138 L 360,164 Z" fill="#2a2a2e"/>
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

// 🎯T137.3 Phone follow-camera zoom: show this fraction of the arena half-extent
// around the buggy (orig iPhone showed half the view = 2× zoom). Tablets/desktop
// show the whole arena.
constexpr float kPhoneViewFrac = 0.5f;

// 🎯T137.4 World size of one asphalt/dirt texture tile (orig tiled ~every 2
// world units). Tiles are batched, so the count is cheap.
constexpr float kTileWorld = 2.5f;

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
    ge::Sprite  asphalt;         // 🎯T137.4 tiled road texture
    ge::Sprite  dirt;            // 🎯T137.4 dirt-patch texture
    ge::Sprite  buggy;           // 🎯T137.4 buggy sprite (tinted)
    ge::Sprite  pond;            // ice-patch texture (the icy pond SVG)
    ge::Sprite  title;
    ge::Sprite  buyPro;
    ge::SpriteBatch batch;       // 🎯T137.4 reused per frame for the tiled ground
    float       diagSpin       = 0.f;   // 🎯T89 render-liveness accumulator
    bool        diagnosticSpin = true;  // 🎯T124 off for deterministic renders
};

Renderer::Renderer() : i_(std::make_unique<Impl>()) {}

void Renderer::setDiagnosticSpin(bool on) { i_->diagnosticSpin = on; }

Renderer::~Renderer() {
    if (i_->pipeline.id != SG_INVALID_ID) sg_destroy_pipeline(i_->pipeline);
    if (i_->stream.id   != SG_INVALID_ID) sg_destroy_buffer(i_->stream);
    if (i_->shader.id   != SG_INVALID_ID) sg_destroy_shader(i_->shader);
    // 🎯T135 pond / title / buyPro are owning ge::Sprites — they free their
    // sg_image + sg_view as i_ (the Impl) is destroyed below, while sokol is
    // still valid (the host runs sg_shutdown after the factory's State, and
    // thus this Renderer, is torn down).
}

void Renderer::init(const char* /*shaderDir*/) {
    // SVG rasterization is platform-agnostic and can run before sokol is
    // ready — uploadPixels inside svg.cpp does `sg_make_image` which needs
    // sg_isvalid(). ge::run sets up sokol before invoking the factory, so
    // by the time init() runs here, sokol is live.
    i_->asphalt = ge::rasterizeSvg(kAsphaltSvg, 256, 256);
    i_->dirt    = ge::rasterizeSvg(kDirtSvg,    256, 256);
    i_->buggy   = ge::rasterizeSvg(kBuggySvg,   512, 256);
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
    // no per-view-state equivalent of the old bgfx setViewRect / setViewClear).
    // The host's composite pass applies viewport tilt; this renderer just
    // draws a flat top-down view.

    auto surf = c.fullRectInPts();
    const float aspect = (surf.h > 0)
        ? static_cast<float>(surf.w) / static_cast<float>(surf.h) : 1.0f;

    // 🎯T137.1 Buggy pose drives both the camera and the chassis draw.
    const Pose pose = scene.buggyPose();

    // 🎯T137.3 Camera. Phones follow the buggy zoomed-in (orig iPhone: 2× +
    // translate-by-buggy-pos); tablets/desktop show the whole arena (orig iPad).
    // The scene layer (ground, surfaces, buggy) rides the follow projection; the
    // UI layer (BUY PRO, title) rides a static whole-arena projection so it
    // stays put on screen.
    const float he        = scene.halfExtent();
    const bool  followCam = c.deviceClass() == ge::DeviceClass::Phone;
    const float viewHe    = followCam ? he * kPhoneViewFrac : he;

    // Fit a world half-extent to the shorter surface axis.
    auto orthoHalf = [aspect](float halfExtent, float& ow, float& oh) {
        if (aspect >= 1.0f) { oh = halfExtent; ow = halfExtent * aspect; }
        else                { ow = halfExtent; oh = halfExtent / aspect; }
    };

    float orthoW, orthoH;                        // scene (follow) view
    orthoHalf(viewHe, orthoW, orthoH);
    const float camX = followCam ? pose.x : 0.0f;
    const float camY = followCam ? pose.y : 0.0f;
    const ge::la::float4x4 proj = orthoMetal(
        camX - orthoW, camX + orthoW, camY - orthoH, camY + orthoH, -1.0f, 1.0f);

    // 🎯T94 presentation-tilt perspective, composed onto the follow projection.
    // Nonzero only on synth platforms (desktop / sim / emu); identity on device.
    const ge::la::float4x4 projTilt =
        ge::la::mul(ge::ortho::tilt(aspect, c.presentationTilt()), proj);

    // Static whole-arena projection for screen-space UI (no follow, no tilt) so
    // the BUY PRO button and title stay anchored to the screen.
    float uiW, uiH;
    orthoHalf(he, uiW, uiH);
    const ge::la::float4x4 uiProj = orthoMetal(-uiW, uiW, -uiH, uiH, -1.0f, 1.0f);
    const float uiPxToWorldX = (surf.w > 0) ? (2.0f * uiW / float(surf.w)) : 0.0f;
    const float uiPxToWorldY = (surf.h > 0) ? (2.0f * uiH / float(surf.h)) : 0.0f;

    const bool proOwned = ge::iap::owned("pro");
    const auto che      = scene.chassisHalfExtents();
    const float hw = che.x, hh = che.y;  // chassis half-extents (debug overlay)

    // Camera's visible world box, inflated past the presentation-tilt bleed.
    const float bgM     = 1.4f;
    const float bgHalfW = orthoW * bgM, bgHalfH = orthoH * bgM;
    const ge::Rect cameraBox{camX - bgHalfW, camY - bgHalfH,
                             2.0f * bgHalfW, 2.0f * bgHalfH};

    // ── Solid base fill (🎯T137.4) ──────────────────────────────────────
    // One dark rect under the tiles via the simple pipeline — insurance against
    // gaps or a null sprite. Everything else is textured below.
    {
        std::vector<PosColorVertex> base;
        pushRect(base, cameraBox, rgb(0x3b, 0x3b, 0x40));
        sg_apply_pipeline(i_->pipeline);
        sg_range vr{ .ptr = base.data(), .size = base.size() * sizeof(PosColorVertex) };
        const int offset = sg_append_buffer(i_->stream, &vr);
        sg_bindings b{};
        b.vertex_buffers[0]        = i_->stream;
        b.vertex_buffer_offsets[0] = offset;
        sg_apply_bindings(&b);
        vs_params_t vsp;
        std::memcpy(vsp.u_modelViewProj, &projTilt[0][0], sizeof(vsp.u_modelViewProj));
        sg_range up{ .ptr = &vsp, .size = sizeof(vsp) };
        sg_apply_uniforms(UB_vs_params, &up);
        sg_draw(0, static_cast<int>(base.size()), 1);
    }

    // ── Textured scene: one SpriteBatch, layered by insertion order (🎯T137.4)
    // asphalt tiles → dirt tiles → ice pond → buggy on top. Replaces the old
    // solid-colour rects. SpriteBatch flushes per-texture runs in order, so the
    // layering is preserved with one submit.
    auto tileInto = [&](const ge::Sprite& spr, ge::Rect region, float tile) {
        if (spr.isNull()) return;
        const float x0 = std::floor(region.x / tile) * tile;
        const float y0 = std::floor(region.y / tile) * tile;
        for (float ty = y0; ty < region.y + region.h; ty += tile)
            for (float tx = x0; tx < region.x + region.w; tx += tile) {
                // Clip each cell to the region so a bounded patch (dirt) doesn't
                // overhang onto the asphalt; uvSubRect keeps the texture aligned.
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

    // Asphalt floor — tiled across the whole visible box.
    tileInto(i_->asphalt, cameraBox, kTileWorld);

    // Surfaces — always shown (🎯T137.4 ungates terrain for parity; the original
    // always drew ice/dirt. The IAP showcase is now the BUY PRO button + the
    // buggy recolour, not the terrain). Dirt is tiled; ice is one irregular pond.
    for (const auto& s : scene.surfaces())
        if (s.type == SurfaceType::Dirt) tileInto(i_->dirt, s.rect, kTileWorld);

    if (!i_->pond.isNull()) {
        for (const auto& s : scene.surfaces()) {
            if (s.type != SurfaceType::Ice) continue;
            // Inflate 25% so the irregular bezier border overhangs the
            // (rectangular) collision area. y-up: rect.y is the bottom edge, so
            // frame() takes the TOP (bottom + height) with h NEGATIVE.
            const float pw = s.rect.w * 0.125f, ph = s.rect.h * 0.125f;
            i_->batch.addSprite(ge::frame(ge::Rect{
                s.rect.x - pw, s.rect.y + s.rect.h + ph,
                s.rect.w + 2.0f * pw, -(s.rect.h + 2.0f * ph)}), i_->pond);
        }
    }

    // Buggy on top of the surfaces, at the chassis pose. Pro entitlement
    // (🎯T65.7) tints it from default yellow to chromed cyan — the IAP demo's
    // visible gate. 🎯T89 render-liveness spin (off for deterministic renders).
    const uint32_t buggyColor = proOwned
        ? rgb(0x66, 0xF0, 0xFF)   // pro: chromed cyan
        : rgb(0xFF, 0xCC, 0x33);  // default: yellow
    if (i_->diagnosticSpin) i_->diagSpin += 0.01f;
    if (!i_->buggy.isNull()) {
        i_->batch.addSprite(
            ge::frameRotated({pose.x, pose.y}, {2.0f * hw, 2.0f * hh},
                             pose.angle + i_->diagSpin),
            i_->buggy, buggyColor);
    }

    i_->batch.submit(projTilt);

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
        // 🎯T137.3 Title rides the static UI projection so it stays anchored to
        // the screen top under the follow-camera, not scrolling with the scene.
        i_->title.draw(ge::la::mul(uiProj, model));
    }

    // BUY PRO button — only when pro isn't owned. Positioned in screen-
    // pixel coordinates inside the UI safe rect; tiltbuggy::proButtonRect
    // returns the same screen rect that main.cpp hit-tests in onEvent.
    if (!proOwned && !i_->buyPro.isNull()) {
        const auto btn = proButtonRect(c);
        // Screen → world conversion uses the same pxToWorldX/Y as the
        // background math above. World is y-up (frame() takes y=top with
        // h NEGATIVE to flip the basis); convert each axis accordingly.
        // 🎯T137.3 Screen-space, on the static UI projection so it never moves
        // with the follow camera — keeping it aligned to its pt-space hit-test.
        const float wL =  -uiW + btn.x         * uiPxToWorldX;
        const float wT =   uiH - btn.y         * uiPxToWorldY;  // y=top in y-up
        const float wW =          btn.w        * uiPxToWorldX;
        const float wH =          btn.h        * uiPxToWorldY;
        const auto model = ge::frame(ge::Rect{wL, wT, wW, -wH});
        i_->buyPro.draw(ge::la::mul(uiProj, model));
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

        // Arena border via box(), buggy position marker via circle() — the
        // convenience shapes, wire-only (default colour). Border = the walls
        // (±halfExtent), so it rides the follow camera with the scene.
        ge::debug::box(ge::Rect{-he, -he, 2.0f * he, 2.0f * he});
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
