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
    uint32_t   abgr;
};

// ~16k vertices per frame; debug overlays in tiltbuggy / multimaze2 sit far
// below that. Overflow warns and drops the run rather than corrupting.
constexpr int   kStreamBufferBytes = 256 * 1024;
constexpr float kTextPt            = 13.0f;

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

void circle(la::float2 center, float radius, la::float4 wireColor,
            la::float4 fillColor, int segments) {
    const bool doWire = wireColor.w > 0.0f;
    const bool doFill = fillColor.w > 0.0f;
    if (!enabled() || (!doWire && !doFill)) return;
    if (segments < 3) segments = 3;
    constexpr float kTwoPi = 6.28318530718f;
    la::float2 prev{center.x + radius, center.y};  // theta = 0
    for (int i = 1; i <= segments; ++i) {
        const float t = (float(i) / float(segments)) * kTwoPi;
        const la::float2 cur{center.x + radius * std::cos(t),
                             center.y + radius * std::sin(t)};
        if (doFill) tri(center, prev, cur, fillColor);
        if (doWire) line(prev, cur, wireColor);
        prev = cur;
    }
}

void point(la::float3 pos, la::float4 color) {
    if (!enabled() || color.w <= 0.0f) return;   // alpha 0 → absent
    // Stored, not expanded here: a point is a fixed on-screen size, so the
    // quad can't be sized until flush() knows worldToClip + the surface.
    st().points.push_back({pos, packAbgr(color)});
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
}

void flush(const Context& ctx, const la::float4x4& worldToClip) {
    auto& s = st();
    if (s.lineVerts.empty() && s.triVerts.empty() && s.texts.empty() &&
        s.points.empty())
        return;
    if (!ensureState()) { clear(); return; }

    if (!s.triVerts.empty())
        drawRun(s.tris, s.triVerts.data(), int(s.triVerts.size()), worldToClip);
    if (!s.lineVerts.empty())
        drawRun(s.lines, s.lineVerts.data(), int(s.lineVerts.size()), worldToClip);

    // Fixed-perceptual-size points: project each centre through worldToClip,
    // then expand to a screen-space quad kPointSizePt pt on a side and draw it
    // with an identity transform — so the dot is a constant on-screen size no
    // matter how far worldToClip zooms. NDC spans the surface's pt extent, so
    // the half-extent in NDC is just kPointSizePt/full.{w,h} (the px-per-pt
    // factor cancels), giving a square in pixels on any aspect / density.
    if (!s.points.empty()) {
        const Rect  full = ctx.fullRectInPts();
        const float hx = full.w > 0.0f ? kPointSizePt / full.w : 0.0f;
        const float hy = full.h > 0.0f ? kPointSizePt / full.h : 0.0f;
        std::vector<DebugVertex> quads;
        quads.reserve(s.points.size() * 6);
        for (const auto& p : s.points) {
            const la::float4 clip =
                la::mul(worldToClip, la::float4{p.pos.x, p.pos.y, p.pos.z, 1.0f});
            if (clip.w <= 0.0f) continue;   // behind the camera / at infinity
            const float nx = clip.x / clip.w, ny = clip.y / clip.w,
                        nz = clip.z / clip.w;
            const DebugVertex a{nx - hx, ny - hy, nz, p.abgr};
            const DebugVertex b{nx + hx, ny - hy, nz, p.abgr};
            const DebugVertex c{nx + hx, ny + hy, nz, p.abgr};
            const DebugVertex d{nx - hx, ny + hy, nz, p.abgr};
            quads.insert(quads.end(), {a, b, c, a, c, d});
        }
        if (!quads.empty()) {
            // Corners are already in NDC, so draw with identity (no reprojection).
            constexpr la::float4x4 kIdentity{{1, 0, 0, 0}, {0, 1, 0, 0},
                                             {0, 0, 1, 0}, {0, 0, 0, 1}};
            drawRun(s.tris, quads.data(), int(quads.size()), kIdentity);
        }
    }

    if (!s.texts.empty()) {
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
            for (const auto& t : s.texts) {
                Sprite sp = rasterizeText(t.str, s.font, sizePx, t.color);
                if (sp.isNull()) continue;
                const Rect rect{t.pos.x, t.pos.y, float(sp.width), float(sp.height)};
                sp.draw(la::mul(px, frame(rect)));
                sg_destroy_view(sp.view);
                sg_destroy_image(sp.tex);
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
} // namespace testing

} // namespace ge::debug
