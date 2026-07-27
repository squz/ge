// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::debug (🎯T97) — sokol_gfx implementation of the debug-render overlay.
//
// Two pipelines (line list + triangle list) share one program and one
// SG_USAGE_STREAM vertex buffer, mirroring ge::sprite's lazy-init + append
// model. Lines / filled tris carry per-vertex straight-alpha colour. Text is
// drawn through ge::Sprite (premultiplied) in pixel space, rasterised fresh
// each flush — a debug overlay isn't on the hot path, so we skip a cache and
// avoid the unbounded-growth trap of changing HUD strings.

#include <ge/debug.h>

#include <ge/FontLoader.h>
#include <ge/ortho.h>
#include <ge/sprite.h>
#include <ge/text.h>
#include <ge/transform.h>

#include "sokol_gfx.h"
#include "ge_debug.h"  // sokol-shdc generated; -I via Module.mk

#include <spdlog/spdlog.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <vector>

namespace ge::debug {
namespace {

struct DebugVertex {
    float    x, y, z;
    uint32_t abgr;
};

struct TextItem {
    la::float2  pos;
    la::float4  color;
    std::string str;
};

struct PointItem {
    la::float3 pos;     // world space; projected through worldToClip at flush
    la::float4 color;
};

struct CircleItem {
    la::float2 center;  // world space
    float      radius;  // world units
    la::float4 wire;
    la::float4 fill;
    float      quality; // max on-screen polygon↔circle gap, pt
    int        minVerts;
};

// ~16k vertices per frame; debug overlays in tiltbuggy / multimaze2 sit far
// below that. Overflow warns and drops the run rather than corrupting.
constexpr int   kStreamBufferBytes = 256 * 1024;
constexpr float kTextPt            = 13.0f;
constexpr float kFpsMarginPt       = 10.0f;
// Upper bound on circle() tessellation, so a pathological zoom-in on a tight
// quality can't ask for thousands of verts.
constexpr int   kMaxCircleSegments = 256;

struct State {
    bool enabled       = false;
    bool enableLatched = false;

    sg_pipeline lines  = {};
    sg_pipeline tris   = {};
    sg_buffer   stream = {};
    bool        ready  = false;

    bool    fontTried = false;
    bool    fontOk    = false;
    FontRef font;

    std::vector<DebugVertex> lineVerts;
    std::vector<DebugVertex> triVerts;
    std::vector<TextItem>    texts;
    std::vector<PointItem>   points;
    std::vector<CircleItem>  circles;
};

State& st() {
    static State s;
    return s;
}

bool parseBool(const char* v) {
    if (!v) return false;
    std::string s(v);
    for (auto& c : s) c = char(std::tolower((unsigned char)c));
    return s == "1" || s == "true" || s == "yes" || s == "on";
}

bool ensureState() {
    auto& s = st();
    if (s.ready) return true;
    if (!sg_isvalid()) return false;

    s.stream = sg_make_buffer((sg_buffer_desc){
        .size  = kStreamBufferBytes,
        .usage = (sg_buffer_usage){
            .vertex_buffer = true,
            .stream_update = true,
        },
        .label = "ge.debug.stream",
    });

    sg_pipeline_desc pd{};
    pd.shader = sg_make_shader(ge_debug_shader_desc(sg_query_backend()));
    pd.layout.attrs[ATTR_ge_debug_a_position].format = SG_VERTEXFORMAT_FLOAT3;
    pd.layout.attrs[ATTR_ge_debug_a_color0].format   = SG_VERTEXFORMAT_UBYTE4N;
    pd.index_type = SG_INDEXTYPE_NONE;
    pd.cull_mode  = SG_CULLMODE_NONE;
    pd.colors[0].blend = (sg_blend_state){
        .enabled          = true,
        .src_factor_rgb   = SG_BLENDFACTOR_SRC_ALPHA,
        .dst_factor_rgb   = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .src_factor_alpha = SG_BLENDFACTOR_ONE,
        .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
    };

    pd.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    pd.label = "ge.debug.tris";
    s.tris = sg_make_pipeline(&pd);

    pd.primitive_type = SG_PRIMITIVETYPE_LINES;
    pd.label = "ge.debug.lines";
    s.lines = sg_make_pipeline(&pd);

    s.ready = true;
    return true;
}

void drawRun(sg_pipeline pipe, const DebugVertex* verts, int count,
             const la::float4x4& mvp) {
    auto& s = st();
    const int bytes = int(count * sizeof(DebugVertex));
    sg_range r{ .ptr = verts, .size = size_t(bytes) };
    const int offset = sg_append_buffer(s.stream, &r);
    if (sg_query_buffer_overflow(s.stream)) {
        SPDLOG_WARN("ge::debug: stream buffer overflow ({} bytes)", bytes);
        return;
    }

    sg_apply_pipeline(pipe);

    sg_bindings b{};
    b.vertex_buffers[0]        = s.stream;
    b.vertex_buffer_offsets[0] = offset;
    sg_apply_bindings(&b);

    vs_params_t vsp;
    std::memcpy(vsp.u_modelViewProj, &mvp[0][0], sizeof(vsp.u_modelViewProj));
    sg_range up{ .ptr = &vsp, .size = sizeof(vsp) };
    sg_apply_uniforms(UB_vs_params, &up);

    sg_draw(0, count, 1);
}

// Pack straight-alpha RGBA [0,1] into 0xAABBGGRR for the DebugVertex stream
// (sokol UBYTE4N reads it back as vec4(r,g,b,a) — see ge_debug.glsl).
inline uint32_t packAbgr(la::float4 c) {
    auto byte = [](float f) -> uint32_t {
        f = f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
        return uint32_t(f * 255.0f + 0.5f);
    };
    return byte(c.x) | (byte(c.y) << 8) | (byte(c.z) << 16) | (byte(c.w) << 24);
}

inline la::float3 v3(la::float2 p) { return {p.x, p.y, 0.0f}; }
inline la::float3 v3(la::float3 p) { return p; }

// 🎯T111 The smoothed FPS is now owned by Context (fed every frame by the host's
// per-frame refresh, overlay-independent) — read it rather than keeping a second
// EMA here, so the readout and ctx.fps() are the same value.
std::string fpsLabel(const Context& ctx) {
    const float f = ctx.fps();
    char buf[32];
    if (f > 0.0f) std::snprintf(buf, sizeof(buf), "%.0f FPS", f);
    else          std::snprintf(buf, sizeof(buf), "FPS --");
    return buf;
}

// ── perf HUD strip state (🎯T173) ────────────────────────────────────
// Plain stores read at flush() — setHudPlacement is safe to call every
// frame. Enable latches GE_PERF_HUD on first query, like enabled().

struct HudState {
    int                          enabled = -1;  // -1 = env not latched yet
    HudAnchor                    anchor  = HudAnchor::TopRight;
    la::float2                   offsetPt{8.0f, 8.0f};
    std::function<std::string()> provider;
};

HudState& hud() {
    static HudState h;
    return h;
}

constexpr la::float4 kHudBackColor{0.0f, 0.0f, 0.0f, 0.55f};
constexpr float      kHudPadPt = 4.0f;  // backing-box padding around the text

template <class V>
void meshImpl(std::span<const V> verts, std::span<const uint16_t> idx,
              la::float4 wire, la::float4 fill) {
    if (!enabled()) return;
    const bool doWire = wire.w > 0.0f;   // alpha 0 → layer absent
    const bool doFill = fill.w > 0.0f;
    if (!doWire && !doFill) return;
    if (idx.size() % 3 != 0)
        SPDLOG_WARN("ge::debug::mesh: index count {} not a multiple of 3; "
                    "tail ignored", idx.size());
    for (size_t i = 0; i + 3 <= idx.size(); i += 3) {
        const uint16_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a >= verts.size() || b >= verts.size() || c >= verts.size()) continue;
        if (doFill) tri(v3(verts[a]), v3(verts[b]), v3(verts[c]), fill);
        if (doWire) {
            line(v3(verts[a]), v3(verts[b]), wire);
            line(v3(verts[b]), v3(verts[c]), wire);
            line(v3(verts[c]), v3(verts[a]), wire);
        }
    }
}

} // namespace

// ────────────────────────────────────────────────
// enable flag
// ────────────────────────────────────────────────

bool enabled() {
    auto& s = st();
    if (!s.enableLatched) {
        s.enabled       = parseBool(std::getenv("GE_DEBUG_OVERLAY"));
        s.enableLatched = true;
    }
    return s.enabled;
}

void setEnabled(bool on) {
    auto& s = st();
    s.enabled       = on;
    s.enableLatched = true;
}

// ────────────────────────────────────────────────
// accumulation
// ────────────────────────────────────────────────

void line(la::float3 a, la::float3 b, la::float4 color) {
    if (!enabled()) return;
    const uint32_t abgr = packAbgr(color);
    auto& v = st().lineVerts;
    v.push_back({a.x, a.y, a.z, abgr});
    v.push_back({b.x, b.y, b.z, abgr});
}

void line(la::float2 a, la::float2 b, la::float4 color) {
    line(v3(a), v3(b), color);
}

void tri(la::float3 a, la::float3 b, la::float3 c, la::float4 color) {
    if (!enabled()) return;
    const uint32_t abgr = packAbgr(color);
    auto& v = st().triVerts;
    v.push_back({a.x, a.y, a.z, abgr});
    v.push_back({b.x, b.y, b.z, abgr});
    v.push_back({c.x, c.y, c.z, abgr});
}

void tri(la::float2 a, la::float2 b, la::float2 c, la::float4 color) {
    tri(v3(a), v3(b), v3(c), color);
}

void mesh(std::span<const la::float2> verts, std::span<const uint16_t> idx,
          la::float4 wireColor, la::float4 fillColor) {
    meshImpl(verts, idx, wireColor, fillColor);
}
void mesh(std::span<const la::float3> verts, std::span<const uint16_t> idx,
          la::float4 wireColor, la::float4 fillColor) {
    meshImpl(verts, idx, wireColor, fillColor);
}

void box(Rect r, la::float4 wireColor, la::float4 fillColor) {
    const bool doWire = wireColor.w > 0.0f;
    const bool doFill = fillColor.w > 0.0f;
    if (!enabled() || (!doWire && !doFill)) return;
    const la::float2 c0{r.x, r.y};
    const la::float2 c1{r.x + r.w, r.y};
    const la::float2 c2{r.x + r.w, r.y + r.h};
    const la::float2 c3{r.x, r.y + r.h};
    if (doFill) {
        tri(c0, c1, c2, fillColor);
        tri(c0, c2, c3, fillColor);
    }
    if (doWire) {
        line(c0, c1, wireColor);
        line(c1, c2, wireColor);
        line(c2, c3, wireColor);
        line(c3, c0, wireColor);
    }
}

int segmentsForQuality(float radiusPt, float qualityPt, int minVerts) {
    const int lo = minVerts > 3 ? minVerts : 3;   // a polygon needs ≥ 3 verts
    if (radiusPt <= 0.0f || qualityPt <= 0.0f) return lo;
    // Balanced n-gon: with vertices ~q outside the circle and edge midpoints ~q
    // inside, the small-angle solution of the equal-deviation condition is
    // n ≈ (π/2)·√(r/q). Simple, and the clamps own the tails — sub-pixel
    // exactness there is invisible on a debug overlay.
    int n = int(std::ceil(1.57079633f * std::sqrt(radiusPt / qualityPt)));
    if (n < lo) n = lo;
    if (n > kMaxCircleSegments) n = kMaxCircleSegments;
    return n;
}

void circle(la::float2 center, float radius, la::float4 wireColor,
            la::float4 fillColor, float quality, int minVerts) {
    const bool doWire = wireColor.w > 0.0f;
    const bool doFill = fillColor.w > 0.0f;
    if (!enabled() || (!doWire && !doFill)) return;
    // Deferred, like point(): the vertex count needs the on-screen radius,
    // known only once flush() has worldToClip.
    st().circles.push_back(
        {center, radius, wireColor, fillColor, quality, minVerts});
}

void point(la::float3 pos, la::float4 color) {
    if (!enabled() || color.w <= 0.0f) return;   // alpha 0 → absent
    // Stored, not expanded here: a point is a fixed on-screen size, so the
    // disc can't be sized until flush() knows worldToClip + the surface.
    st().points.push_back({pos, color});
}

void point(la::float2 pos, la::float4 color) {
    point(v3(pos), color);
}

void text(la::float2 posPx, std::string_view str, la::float4 color) {
    if (!enabled()) return;
    st().texts.push_back({posPx, color, std::string(str)});
}

// ────────────────────────────────────────────────
// flush
// ────────────────────────────────────────────────

void clear() {
    auto& s = st();
    s.lineVerts.clear();
    s.triVerts.clear();
    s.texts.clear();
    s.points.clear();
    s.circles.clear();
}

namespace {

// Tessellate a world-space circle into the shared tri/line streams: a balanced
// n-gon whose vertex count comes from its on-screen radius (projected against
// worldToClip), so the pt quality holds at any zoom. Shared by circle() and
// point() — a point is just a tiny, fixed-on-screen-size circle. `full` is the
// surface size in pt.
void expandCircle(la::float3 center, float radius, la::float4 wire,
                  la::float4 fill, float quality, int minVerts,
                  const la::float4x4& worldToClip, Rect full) {
    const bool doWire = wire.w > 0.0f;
    const bool doFill = fill.w > 0.0f;
    if (!doWire && !doFill) return;
    // On-screen radius in pt: project the centre and a +x edge point.
    const la::float4 pc = la::mul(
        worldToClip, la::float4{center.x, center.y, center.z, 1.0f});
    const la::float4 pe = la::mul(
        worldToClip, la::float4{center.x + radius, center.y, center.z, 1.0f});
    float radiusPt = 0.0f;
    if (pc.w > 0.0f && pe.w > 0.0f) {
        const float dx = (pe.x / pe.w - pc.x / pc.w) * full.w * 0.5f;
        const float dy = (pe.y / pe.w - pc.y / pc.w) * full.h * 0.5f;
        radiusPt = std::sqrt(dx * dx + dy * dy);
    }
    const int   n     = segmentsForQuality(radiusPt, quality, minVerts);
    const float step  = 6.28318530718f / float(n);
    const float half  = step * 0.5f;                 // π/n
    const float denom = 1.0f + std::cos(half);
    // Balanced radius Rv = 2r/(1+cos(π/n)): vertices ~quality outside the
    // circle, edge midpoints ~quality inside — equal worst-case error.
    const float rv = denom > 0.0f ? radius * 2.0f / denom : radius;
    la::float3 prev{center.x + rv, center.y, center.z};   // angle 0
    for (int i = 1; i <= n; ++i) {
        const float t = float(i) * step;
        const la::float3 cur{center.x + rv * std::cos(t),
                             center.y + rv * std::sin(t), center.z};
        if (doFill) tri(center, prev, cur, fill);
        if (doWire) line(prev, cur, wire);
        prev = cur;
    }
}

} // namespace

bool hudEnabled() {
    auto& h = hud();
    if (h.enabled < 0) h.enabled = parseBool(std::getenv("GE_PERF_HUD")) ? 1 : 0;
    return h.enabled == 1;
}

void setHudEnabled(bool on) { hud().enabled = on ? 1 : 0; }

void setHudPlacement(HudAnchor anchor, la::float2 offsetPt) {
    hud().anchor   = anchor;
    hud().offsetPt = offsetPt;
}

void setHudProvider(std::function<std::string()> provider) {
    hud().provider = std::move(provider);
}

std::string hudLabel(float dtSeconds, float fps, std::string_view extra) {
    char buf[64];
    if (dtSeconds > 0.0f)
        std::snprintf(buf, sizeof(buf), "dt %.1f ms  ", dtSeconds * 1000.0f);
    else
        std::snprintf(buf, sizeof(buf), "dt --  ");
    std::string out = buf;
    if (fps > 0.0f)
        std::snprintf(buf, sizeof(buf), "%.1f fps", fps);
    else
        std::snprintf(buf, sizeof(buf), "fps --");
    out += buf;
    if (!extra.empty()) {
        out += "  ";
        out += extra;
    }
    return out;
}

void flush(const Context& ctx, const la::float4x4& worldToClip) {
    auto& s = st();
    const bool showHud = hudEnabled();
    // Legacy bare FPS readout: overlay on, HUD not claiming the corner.
    const bool showFps = enabled() && !showHud;
    const std::string fps = showFps ? fpsLabel(ctx) : std::string{};
    if (s.lineVerts.empty() && s.triVerts.empty() && s.texts.empty() &&
        s.points.empty() && s.circles.empty() && !showFps && !showHud)
        return;
    if (!ensureState()) { clear(); return; }

    // Circles and points both tessellate into the world-space tri/line streams
    // (a point is just a tiny fixed-on-screen-size circle), so they must run
    // before the streams are drawn below.
    if (!s.circles.empty() || !s.points.empty()) {
        const Rect full = ctx.fullRectInPts();
        for (const auto& c : s.circles)
            expandCircle({c.center.x, c.center.y, 0.0f}, c.radius, c.wire,
                         c.fill, c.quality, c.minVerts, worldToClip, full);
        // A point projects to a fixed kPointSizePt pt on screen: find the world
        // radius that does so (pt-per-world ≈ a projected unit-x offset), then
        // run it through the same tessellator — fill only, ≥8-gon so it reads
        // round even tiny.
        for (const auto& p : s.points) {
            const la::float4 pc = la::mul(
                worldToClip, la::float4{p.pos.x, p.pos.y, p.pos.z, 1.0f});
            const la::float4 pe = la::mul(
                worldToClip, la::float4{p.pos.x + 1.0f, p.pos.y, p.pos.z, 1.0f});
            if (pc.w <= 0.0f || pe.w <= 0.0f) continue;
            const float sx = (pe.x / pe.w - pc.x / pc.w) * full.w * 0.5f;
            const float sy = (pe.y / pe.w - pc.y / pc.w) * full.h * 0.5f;
            const float scale = std::sqrt(sx * sx + sy * sy);  // pt per world unit
            if (scale <= 0.0f) continue;
            expandCircle(p.pos, (kPointSizePt * 0.5f) / scale, /*wire=*/{},
                         p.color, kCircleQualityPt, /*minVerts=*/8,
                         worldToClip, full);
        }
    }

    if (!s.triVerts.empty())
        drawRun(s.tris, s.triVerts.data(), int(s.triVerts.size()), worldToClip);
    if (!s.lineVerts.empty())
        drawRun(s.lines, s.lineVerts.data(), int(s.lineVerts.size()), worldToClip);

    if (!s.texts.empty() || showFps || showHud) {
        if (!s.fontTried) {
            s.fontTried = true;
            try {
                s.font   = resolveFont("system:monospace");
                s.fontOk = true;
            } catch (const std::exception& e) {
                SPDLOG_WARN("ge::debug: no monospace font for text overlay: {}",
                            e.what());
            }
        }
        if (s.fontOk) {
            const float ppt  = ctx.pixelsPerPt();
            const Rect  full = ctx.fullRectInPts();
            const la::float4x4 px = ortho::pixelOrtho(full.w * ppt, full.h * ppt);
            const float sizePx = kTextPt * ppt;
            auto drawText = [&](la::float2 pos, const std::string& str, la::float4 color) {
                Sprite sp = rasterizeText(str, s.font, sizePx, color);
                if (sp.isNull()) return;
                const Rect rect{pos.x, pos.y, float(sp.width), float(sp.height)};
                sp.draw(la::mul(px, frame(rect)));
                // 🎯T135 sp frees its sg_image + sg_view here at scope end.
            };
            for (const auto& t : s.texts) {
                drawText(t.pos, t.str, t.color);
            }
            if (showFps) {
                Sprite sp = rasterizeText(fps, s.font, sizePx, kTextColor);
                if (!sp.isNull()) {
                    const float margin = kFpsMarginPt * ppt;
                    const float surfaceW = full.w * ppt;
                    float x = surfaceW - float(sp.width) - margin;
                    if (x < margin) x = margin;
                    const Rect rect{x, margin, float(sp.width), float(sp.height)};
                    sp.draw(la::mul(px, frame(rect)));
                    // 🎯T135 sp frees its sg_image + sg_view here at scope end.
                }
            }

            // 🎯T173 perf HUD strip: dt/fps + game-supplied segments over a
            // translucent backing box, at the imperative placement (which
            // the game may have re-set this very frame).
            if (showHud) {
                auto& h = hud();
                const std::string label =
                    hudLabel(ctx.frameTime(), ctx.fps(),
                             h.provider ? h.provider() : std::string{});
                Sprite sp = rasterizeText(label, s.font, sizePx, kTextColor);
                if (!sp.isNull()) {
                    const float padPx = kHudPadPt * ppt;
                    const float boxW  = float(sp.width) + 2.0f * padPx;
                    const float boxH  = float(sp.height) + 2.0f * padPx;
                    const float surW  = full.w * ppt;
                    const float surH  = full.h * ppt;
                    const la::float2 offPx = h.offsetPt * ppt;
                    la::float2 boxPos;  // top-left of the backing box, px
                    switch (h.anchor) {
                        case HudAnchor::TopLeft:     boxPos = offPx; break;
                        case HudAnchor::TopRight:    boxPos = {surW - boxW - offPx.x, offPx.y}; break;
                        case HudAnchor::BottomLeft:  boxPos = {offPx.x, surH - boxH - offPx.y}; break;
                        case HudAnchor::BottomRight: boxPos = {surW - boxW - offPx.x, surH - boxH - offPx.y}; break;
                        case HudAnchor::Custom:      boxPos = offPx; break;
                    }
                    const uint32_t bg = packAbgr(kHudBackColor);
                    const float x0 = boxPos.x, y0 = boxPos.y;
                    const float x1 = x0 + boxW, y1 = y0 + boxH;
                    const DebugVertex box[6] = {
                        {x0, y0, 0, bg}, {x1, y0, 0, bg}, {x1, y1, 0, bg},
                        {x0, y0, 0, bg}, {x1, y1, 0, bg}, {x0, y1, 0, bg},
                    };
                    drawRun(s.tris, box, 6, px);
                    const Rect rect{x0 + padPx, y0 + padPx,
                                    float(sp.width), float(sp.height)};
                    sp.draw(la::mul(px, frame(rect)));
                }
            }
        }
    }

    clear();
}

// ────────────────────────────────────────────────
// test introspection
// ────────────────────────────────────────────────

namespace testing {
int lineVertexCount() { return int(st().lineVerts.size()); }
int triVertexCount()  { return int(st().triVerts.size()); }
int textItemCount()   { return int(st().texts.size()); }
int pointItemCount()  { return int(st().points.size()); }
int circleItemCount() { return int(st().circles.size()); }
} // namespace testing

} // namespace ge::debug
