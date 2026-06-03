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
#include <cstdlib>
#include <cstring>
#include <exception>
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
    uint32_t    abgr;
    std::string str;
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

inline la::float4 unpackColor(uint32_t abgr) {
    return { float(abgr & 0xFFu) / 255.0f,
             float((abgr >> 8) & 0xFFu) / 255.0f,
             float((abgr >> 16) & 0xFFu) / 255.0f,
             float((abgr >> 24) & 0xFFu) / 255.0f };
}

inline la::float3 v3(la::float2 p) { return {p.x, p.y, 0.0f}; }
inline la::float3 v3(la::float3 p) { return p; }

template <class V>
void meshImpl(const V* verts, int vertCount, const uint16_t* idx,
              int idxCount, bool fill, uint32_t abgr) {
    if (!enabled()) return;
    if (abgr == 0) abgr = fill ? kFillColor : kLineColor;  // 0 → pick by fill
    if (idxCount % 3 != 0)
        SPDLOG_WARN("ge::debug::mesh: indexCount {} not a multiple of 3; "
                    "tail ignored", idxCount);
    for (int i = 0; i + 3 <= idxCount; i += 3) {
        const uint16_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
        if (a >= vertCount || b >= vertCount || c >= vertCount) continue;
        if (fill) {
            tri(v3(verts[a]), v3(verts[b]), v3(verts[c]), abgr);
        } else {
            line(v3(verts[a]), v3(verts[b]), abgr);
            line(v3(verts[b]), v3(verts[c]), abgr);
            line(v3(verts[c]), v3(verts[a]), abgr);
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

void line(la::float3 a, la::float3 b, uint32_t abgr) {
    if (!enabled()) return;
    auto& v = st().lineVerts;
    v.push_back({a.x, a.y, a.z, abgr});
    v.push_back({b.x, b.y, b.z, abgr});
}

void line(la::float2 a, la::float2 b, uint32_t abgr) {
    line(v3(a), v3(b), abgr);
}

void tri(la::float3 a, la::float3 b, la::float3 c, uint32_t abgr) {
    if (!enabled()) return;
    auto& v = st().triVerts;
    v.push_back({a.x, a.y, a.z, abgr});
    v.push_back({b.x, b.y, b.z, abgr});
    v.push_back({c.x, c.y, c.z, abgr});
}

void tri(la::float2 a, la::float2 b, la::float2 c, uint32_t abgr) {
    tri(v3(a), v3(b), v3(c), abgr);
}

void mesh(const la::float2* verts, int vertCount, const uint16_t* idx,
          int idxCount, bool fill, uint32_t abgr) {
    meshImpl(verts, vertCount, idx, idxCount, fill, abgr);
}
void mesh(const la::float3* verts, int vertCount, const uint16_t* idx,
          int idxCount, bool fill, uint32_t abgr) {
    meshImpl(verts, vertCount, idx, idxCount, fill, abgr);
}

void text(la::float2 posPx, std::string_view str, uint32_t abgr) {
    if (!enabled()) return;
    st().texts.push_back({posPx, abgr, std::string(str)});
}

// ────────────────────────────────────────────────
// flush
// ────────────────────────────────────────────────

void clear() {
    auto& s = st();
    s.lineVerts.clear();
    s.triVerts.clear();
    s.texts.clear();
}

void flush(const Context& ctx, const la::float4x4& worldToClip) {
    auto& s = st();
    if (s.lineVerts.empty() && s.triVerts.empty() && s.texts.empty()) return;
    if (!ensureState()) { clear(); return; }

    if (!s.triVerts.empty())
        drawRun(s.tris, s.triVerts.data(), int(s.triVerts.size()), worldToClip);
    if (!s.lineVerts.empty())
        drawRun(s.lines, s.lineVerts.data(), int(s.lineVerts.size()), worldToClip);

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
                Sprite sp = rasterizeText(t.str, s.font, sizePx, unpackColor(t.abgr));
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
} // namespace testing

} // namespace ge::debug
