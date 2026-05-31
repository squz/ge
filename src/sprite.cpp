// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// T38 spike: sokol_gfx port of ge::Sprite + ge::SpriteBatch.
//
// One global pipeline (created lazily), one sampler, and one
// SG_USAGE_STREAM vertex buffer fed via sg_append_buffer. sokol's
// "one update per frame, many appends per frame" model maps cleanly
// onto how SpriteBatch sliced its bgfx::TransientVertexBuffer
// allocations.

#include <ge/sprite.h>

#include "sokol_gfx.h"
#include "ge_sprite.h"  // sokol-shdc generated; -I via Module.mk

#include <spdlog/spdlog.h>

#include <cstring>

namespace ge {

namespace {

// Stream vertex buffer sized for ~5500 sprite quads per frame. The
// active scenes in tiltbuggy / multimaze2 are well under 100 quads;
// this leaves room for future titles without re-tuning.
constexpr int kStreamBufferBytes = 256 * 1024;

struct State {
    sg_pipeline pipeline = {};
    sg_sampler  sampler  = {};
    sg_buffer   stream   = {};
    bool        ready    = false;
};

State& globalState() {
    static State s;
    return s;
}

bool ensureState() {
    auto& s = globalState();
    if (s.ready) return true;
    if (!sg_isvalid()) return false;

    s.stream = sg_make_buffer((sg_buffer_desc){
        .size  = kStreamBufferBytes,
        .usage = (sg_buffer_usage){
            .vertex_buffer = true,
            .stream_update = true,
        },
        .label = "ge.sprite.stream",
    });

    s.sampler = sg_make_sampler((sg_sampler_desc){
        .min_filter = SG_FILTER_LINEAR,
        .mag_filter = SG_FILTER_LINEAR,
        .wrap_u     = SG_WRAP_CLAMP_TO_EDGE,
        .wrap_v     = SG_WRAP_CLAMP_TO_EDGE,
        .label      = "ge.sprite.sampler",
    });

    sg_pipeline_desc pd{};
    pd.shader = sg_make_shader(sprite_shader_desc(sg_query_backend()));
    pd.layout.attrs[ATTR_sprite_a_position].format  = SG_VERTEXFORMAT_FLOAT3;
    pd.layout.attrs[ATTR_sprite_a_texcoord0].format = SG_VERTEXFORMAT_FLOAT2;
    pd.layout.attrs[ATTR_sprite_a_color0].format    = SG_VERTEXFORMAT_UBYTE4N;
    pd.primitive_type  = SG_PRIMITIVETYPE_TRIANGLES;
    pd.index_type      = SG_INDEXTYPE_NONE;
    pd.cull_mode       = SG_CULLMODE_NONE;
    pd.colors[0].blend = (sg_blend_state){
        .enabled          = true,
        .src_factor_rgb   = SG_BLENDFACTOR_ONE,
        .dst_factor_rgb   = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        .src_factor_alpha = SG_BLENDFACTOR_ONE,
        .dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
    };
    pd.label = "ge.sprite.pipeline";
    s.pipeline = sg_make_pipeline(&pd);

    s.ready = true;
    return true;
}

inline la::float2 applyMatrix(const la::float4x4& m, float x, float y) {
    const auto v = la::mul(m, la::float4{x, y, 0.f, 1.f});
    return {v.x, v.y};
}

void drawRun(sg_view view, const SpriteVertex* verts, int count,
             const la::float4x4& mvp) {
    if (!ensureState()) return;
    auto& s = globalState();

    const int bytes = int(count * sizeof(SpriteVertex));
    sg_range r{ .ptr = verts, .size = size_t(bytes) };
    const int offset = sg_append_buffer(s.stream, &r);
    if (sg_query_buffer_overflow(s.stream)) {
        SPDLOG_WARN("ge::sprite: stream buffer overflow ({} bytes)", bytes);
        return;
    }

    sg_apply_pipeline(s.pipeline);

    sg_bindings b{};
    b.vertex_buffers[0]        = s.stream;
    b.vertex_buffer_offsets[0] = offset;
    b.views[VIEW_s_tex]        = view;
    b.samplers[SMP_s_smp]      = s.sampler;
    sg_apply_bindings(&b);

    vs_params_t vsp;
    std::memcpy(vsp.u_modelViewProj, &mvp[0][0], sizeof(vsp.u_modelViewProj));
    sg_range up{ .ptr = &vsp, .size = sizeof(vsp) };
    sg_apply_uniforms(UB_vs_params, &up);

    sg_draw(0, count, 1);
}

} // namespace

// ────────────────────────────────────────────────
// Sprite
// ────────────────────────────────────────────────

void Sprite::draw(const la::float4x4& mvp) const {
    if (isNull()) return;
    constexpr uint32_t kWhite = 0xFFFFFFFFu;
    const SpriteVertex verts[6] = {
        {0.f, 0.f, 0.f, 0.f, 0.f, kWhite},
        {1.f, 0.f, 0.f, 1.f, 0.f, kWhite},
        {1.f, 1.f, 0.f, 1.f, 1.f, kWhite},
        {0.f, 0.f, 0.f, 0.f, 0.f, kWhite},
        {1.f, 1.f, 0.f, 1.f, 1.f, kWhite},
        {0.f, 1.f, 0.f, 0.f, 1.f, kWhite},
    };
    drawRun(view, verts, 6, mvp);
}

// ────────────────────────────────────────────────
// SpriteBatch
// ────────────────────────────────────────────────

SpriteBatch::SpriteBatch() = default;
SpriteBatch::~SpriteBatch() = default;

void SpriteBatch::clear() { quads_.clear(); }

void SpriteBatch::addSprite(const la::float4x4& m,
                            const Sprite&       sprite,
                            uint32_t            color) {
    addSprite(m, sprite, Rect{0.f, 0.f, 1.f, 1.f}, color);
}

void SpriteBatch::addSprite(const la::float4x4& m,
                            const Sprite&       sprite,
                            Rect                uvSubRect,
                            uint32_t            color) {
    if (sprite.isNull()) return;

    const auto p00 = applyMatrix(m, 0.f, 0.f);
    const auto p10 = applyMatrix(m, 1.f, 0.f);
    const auto p11 = applyMatrix(m, 1.f, 1.f);
    const auto p01 = applyMatrix(m, 0.f, 1.f);

    const float uvL = uvSubRect.x;
    const float uvT = uvSubRect.y;
    const float uvR = uvSubRect.x + uvSubRect.w;
    const float uvB = uvSubRect.y + uvSubRect.h;

    Quad q;
    q.tex  = sprite.tex;
    q.view = sprite.view;
    q.verts[0] = {p00.x, p00.y, 0.f, uvL, uvT, color};
    q.verts[1] = {p10.x, p10.y, 0.f, uvR, uvT, color};
    q.verts[2] = {p11.x, p11.y, 0.f, uvR, uvB, color};
    q.verts[3] = {p00.x, p00.y, 0.f, uvL, uvT, color};
    q.verts[4] = {p11.x, p11.y, 0.f, uvR, uvB, color};
    q.verts[5] = {p01.x, p01.y, 0.f, uvL, uvB, color};
    quads_.push_back(q);
}

void SpriteBatch::submit(const la::float4x4& worldToClip) {
    if (quads_.empty()) return;
    if (!ensureState()) return;

    std::size_t i = 0;
    while (i < quads_.size()) {
        sg_view curView = quads_[i].view;
        std::size_t j = i + 1;
        while (j < quads_.size() && quads_[j].view.id == curView.id) {
            ++j;
        }
        const std::size_t count = j - i;
        const int verts = int(count * 6);
        std::vector<SpriteVertex> packed(verts);
        for (std::size_t k = 0; k < count; ++k) {
            std::memcpy(&packed[k * 6], quads_[i + k].verts,
                        6 * sizeof(SpriteVertex));
        }
        drawRun(curView, packed.data(), verts, worldToClip);
        i = j;
    }
}

} // namespace ge
