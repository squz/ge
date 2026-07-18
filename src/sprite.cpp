// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// T38 spike: sokol_gfx port of ge::Sprite + ge::SpriteBatch.
//
// One global pipeline (created lazily), one sampler, and one
// SG_USASTREAM vertex buffer fed via sg_append_buffer. sokol's
// "one update per frame, many appends per frame" model maps cleanly
// onto how SpriteBatch streams its per-frame quad-vertex runs.

#include <ge/sprite.h>
#include <ge/CmdStream.h>

#include "sokol_gfx.h"
#include "ge_sprite.h"  // sokol-shdc generated; -I via Module.mk
#include "sprite_internal.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ge {

// ── ge::Sprite RAII (🎯T135) ──────────────────────────────────────
//
// Owning move-only semantics: the destructor and move-assignment release the
// underlying sg_image + sg_view so a re-rasterized slot can't orphan its prior
// resources into sokol's small pools. Release is guarded on sg_isvalid() — a
// Sprite outliving the sokol context (post-shutdown, or a headless unit test)
// frees nothing rather than asserting inside a dead pool.

namespace detail {
namespace {
std::atomic<uint64_t> g_spriteReleases{0};
}  // namespace
uint64_t spriteReleaseCount() { return g_spriteReleases.load(std::memory_order_relaxed); }
}  // namespace detail

void Sprite::destroy() {
    if (view.id != SG_INVALID_ID || tex.id != SG_INVALID_ID) {
        detail::g_spriteReleases.fetch_add(1, std::memory_order_relaxed);
        if (sg_isvalid()) {
            if (view.id != SG_INVALID_ID) sg_destroy_view(view);
            if (tex.id  != SG_INVALID_ID) sg_destroy_image(tex);
        }
        tex = {}; view = {}; width = 0; height = 0;
    }
}

Sprite::~Sprite() { destroy(); }

Sprite::Sprite(Sprite&& o) noexcept
    : tex(o.tex), view(o.view), width(o.width), height(o.height) {
    o.tex = {}; o.view = {}; o.width = 0; o.height = 0;
}

Sprite& Sprite::operator=(Sprite&& o) noexcept {
    if (this != &o) {
        destroy();  // release this slot's prior resources before adopting o's
        tex = o.tex; view = o.view; width = o.width; height = o.height;
        o.tex = {}; o.view = {}; o.width = 0; o.height = 0;
    }
    return *this;
}

namespace {

// Per-buffer size of each pooled SG_USASTREAM vertex buffer (~1820
// quads). All Sprite::draw + SpriteBatch::submit calls in a frame share
// the pool, appending into the current buffer until it fills, then
// spilling into the next pooled buffer (🎯T113). A frame that needs
// more than one buffer's worth grows the pool rather than dropping
// quads — see drawRun + planSpriteRun.
constexpr int kStreamBufferBytes = 256 * 1024;

struct State {
    sg_pipeline pipeline = {};
    sg_sampler  sampler  = {};
    // Pool of per-frame stream buffers. `used[i]` is the byte count
    // appended to pool[i] so far this frame; `curIdx` is the buffer
    // currently being filled. The commit listener rewinds both each
    // frame (sokol independently resets each buffer's own append cursor
    // on first touch after a commit, so the two stay in lockstep).
    std::vector<sg_buffer> pool;
    std::vector<int>       used;
    std::size_t            curIdx = 0;
    bool                   ready  = false;
};

State& globalState() {
    static State s;
    return s;
}

sg_buffer makeStreamBuffer() {
    return sg_make_buffer((sg_buffer_desc){
        .size  = kStreamBufferBytes,
        .usage = (sg_buffer_usage){
            .vertex_buffer = true,
            .stream_update = true,
        },
        .label = "ge.sprite.stream",
    });
}

// Frame-boundary reset: rewind the pool cursor and zero the per-buffer
// byte counters so the next frame refills from pool[0]. Registered once
// via sg_add_commit_listener; fires on every sg_commit (game thread).
void onSpriteCommit(void* userData) {
    auto& s = *static_cast<State*>(userData);
    s.curIdx = 0;
    std::fill(s.used.begin(), s.used.end(), 0);
}

bool ensureState() {
    auto& s = globalState();
    if (s.ready) return true;
    if (!sg_isvalid()) return false;

    s.pool.push_back(makeStreamBuffer());
    s.used.push_back(0);
    s.curIdx = 0;

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

    sg_add_commit_listener((sg_commit_listener){
        .func = onSpriteCommit, .user_data = &s,
    });

    s.ready = true;
    return true;
}

inline la::float2 applyMatrix(const la::float4x4& m, float x, float y) {
    const auto v = la::mul(m, la::float4{x, y, 0.f, 1.f});
    return {v.x, v.y};
}

// Draw one same-texture run, spreading it across as many pooled stream
// buffers as it takes so nothing is dropped on overflow (🎯T113). The
// split is decided by detail::planSpriteRun; this loop just executes it:
// each step appends a chunk to the current (or next) pooled buffer and
// issues a draw.
void drawRun(sg_view view, const SpriteVertex* verts, int count,
             const la::float4x4& mvp) {
    if (count <= 0) return;
    if (!ensureState()) return;
    auto& s = globalState();

    constexpr int kVBytes   = int(sizeof(SpriteVertex));
    const int     fullVerts = kStreamBufferBytes / kVBytes;
    const int     firstFree = (kStreamBufferBytes - s.used[s.curIdx]) / kVBytes;

    const SpriteVertex* p = verts;
    for (const auto& step : detail::planSpriteRun(count, firstFree, fullVerts)) {
        if (step.newBuffer) {
            ++s.curIdx;
            if (s.curIdx >= s.pool.size()) {
                s.pool.push_back(makeStreamBuffer());
                s.used.push_back(0);
            }
        }
        const sg_buffer buf   = s.pool[s.curIdx];
        const int       bytes = step.verts * kVBytes;
        const sg_range  r{ .ptr = p, .size = size_t(bytes) };
        const int       offset = sg_append_buffer(buf, &r);
        if (sg_query_buffer_overflow(buf)) {
            // Unreachable by construction: planSpriteRun sizes every
            // step to fit its buffer. Loud, not silent, if it ever isn't.
            SPDLOG_ERROR("ge::sprite: unexpected stream overflow ({} bytes)",
                         bytes);
            return;
        }
        s.used[s.curIdx] += bytes;

        sg_apply_pipeline(s.pipeline);

        sg_bindings b{};
        b.vertex_buffers[0]        = buf;
        b.vertex_buffer_offsets[0] = offset;
        b.views[VIEW_s_tex]        = view;
        b.samplers[SMP_s_smp]      = s.sampler;
        sg_apply_bindings(&b);

        vs_params_t vsp;
        std::memcpy(vsp.u_modelViewProj, &mvp[0][0], sizeof(vsp.u_modelViewProj));
        const sg_range up{ .ptr = &vsp, .size = sizeof(vsp) };
        sg_apply_uniforms(UB_vs_params, &up);

        sg_draw(0, step.verts, 1);
        p += step.verts;
    }
}

} // namespace

namespace detail {

std::vector<SpriteRunStep> planSpriteRun(int totalVerts,
                                         int firstFreeVerts,
                                         int fullVerts) {
    std::vector<SpriteRunStep> steps;
    if (totalVerts <= 0) return steps;

    // Round both capacities down to whole quads (6 verts) so a chunk
    // boundary never splits a triangle.
    const int fullCap = fullVerts > 0 ? fullVerts - fullVerts % 6 : 0;
    int cap     = firstFreeVerts > 0 ? firstFreeVerts - firstFreeVerts % 6 : 0;
    int remaining = totalVerts;
    bool advance  = false;  // first step uses the current buffer...

    while (remaining > 0) {
        if (cap < 6) {                 // ...unless it can't hold even one quad
            if (fullCap < 6) {         // degenerate: no buffer fits a quad —
                steps.push_back({advance, remaining});  // emit rest; drawRun
                break;                                  // overflow-guards it
            }
            cap     = fullCap;
            advance = true;            // this step starts a fresh buffer
        }
        const int take = std::min(remaining, cap);
        steps.push_back({advance, take});
        remaining -= take;
        cap       -= take;
        advance    = false;           // stay in this buffer until it fills
    }
    return steps;
}

} // namespace detail

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
    // Local GPU path (server display / direct). When cmdstream LiveCapture is
    // armed we still draw locally AND serialise runs for the player.
    const bool gpu = ensureState();
    if (!gpu && !cmdstream::liveCapture()) return;

    std::size_t i = 0;
    while (i < quads_.size()) {
        sg_view curView = quads_[i].view;
        sg_image curTex = quads_[i].tex;
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
        if (gpu) drawRun(curView, packed.data(), verts, worldToClip);

        // 🎯T128 live sprite capture — pass-through + cache (recipe preferred).
        if (auto* cap = cmdstream::liveCapture()) {
            if (cmdstream::lookupImageRecipe(curTex.id)) {
                cap->noteRegisteredImage(curTex.id);
            } else {
                uint16_t iw = 0, ih = 0;
                const std::vector<uint8_t>* px = nullptr;
                if (cmdstream::lookupImagePixels(curTex.id, iw, ih, px) && px) {
                    cap->noteImage(curTex.id, iw, ih, px->data(), px->size());
                }
            }
            // worldToClip is column-major float4x4 — same layout as la::float4x4.
            float mvp[16];
            std::memcpy(mvp, &worldToClip, sizeof(mvp));
            if (verts > 0 && verts <= 65535) {
                cap->spriteRun(curTex.id, packed.data(),
                               static_cast<uint16_t>(verts), mvp);
            }
        }
        i = j;
    }
}

} // namespace ge
