// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T170 SDF hand renderer — capsule skeleton on the CPU, implicit
// outline in the fragment shader (see src/render/shaders/ge_hint.glsl).
//
// The hand models below are parameter tables in "hand units" (fingertip
// at the origin, +y down, the hand mass hanging below-right), scaled by
// the tweakable hand size and emitted in point space. Chain ids group
// capsules for the shader's interior separator strokes: 0 = palm,
// 1..5 = individual digits.

#include <ge/hint_hand.h>

#include <ge/Tweak.h>
#include <ge/ortho.h>

#include "sokol_gfx.h"
#include "ge_hint.h"  // sokol-shdc generated; -I via Module.mk

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace ge::hint {
namespace {

// ── look knobs (fractions of hand size unless noted) ────────────────

tweak::Tweak<float> tweakHandSizePt{"hint.hand_size_pt", 110.0f};
tweak::Tweak<float> tweakSmoothK{"hint.smooth_k", 0.022f};
tweak::Tweak<float> tweakOutlineW{"hint.outline_w", 0.030f};
tweak::Tweak<float> tweakHaloW{"hint.halo_w", 0.028f};
tweak::Tweak<float> tweakStrokeW{"hint.stroke_w", 0.009f};
tweak::Tweak<float> tweakStrokeFade{"hint.stroke_fade", 0.055f};
tweak::Tweak<float> tweakHoverScale{"hint.hover_scale", 0.10f};

constexpr int kMaxCaps = 24;  // must match u_caps_* array size in ge_hint.glsl

struct Cap {
    la::float2 a, b;
    float      r     = 0;
    int        chain = 0;  // 0 = palm, 1..5 = digit
};

// ── GPU state (debug-overlay pattern: lazy, file-static) ────────────

struct State {
    sg_shader   shader = {};
    sg_pipeline pip    = {};
    sg_buffer   stream = {};
    bool        ready  = false;
};

State& st() {
    static State s;
    return s;
}

bool ensureState() {
    auto& s = st();
    if (s.ready) return true;
    if (!sg_isvalid()) return false;

    sg_buffer_desc bd{};
    bd.usage.vertex_buffer = true;
    bd.usage.stream_update = true;
    bd.size                = 4096;
    bd.label               = "ge.hint.stream";
    s.stream               = sg_make_buffer(&bd);

    s.shader = sg_make_shader(hint_shader_desc(sg_query_backend()));
    if (s.shader.id == SG_INVALID_ID) {
        SPDLOG_ERROR("ge::hint: shader create failed");
        return false;
    }

    sg_pipeline_desc pd{};
    pd.shader                               = s.shader;
    pd.layout.attrs[ATTR_hint_a_pos].format = SG_VERTEXFORMAT_FLOAT2;
    pd.index_type                           = SG_INDEXTYPE_NONE;
    pd.primitive_type                       = SG_PRIMITIVETYPE_TRIANGLES;
    pd.depth.compare                        = SG_COMPAREFUNC_ALWAYS;
    pd.depth.write_enabled                  = false;
    pd.colors[0].blend.enabled              = true;
    pd.colors[0].blend.src_factor_rgb       = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_rgb       = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.src_factor_alpha     = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha     = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.label                                = "ge.hint.hand";
    s.pip                                   = sg_make_pipeline(&pd);

    s.ready = true;
    return true;
}

// ── hand models ─────────────────────────────────────────────────────

// Pointing hand (index extended, three curled fingers, thumb) with the
// index fingertip at `tip`. `press` squashes the fingertip and settles
// the hover; the whole hand scales up slightly while hovering.
void pointingHand(std::vector<Cap>& out, la::float2 tip, float S, float press) {
    const float hover = 1.0f + tweakHoverScale.get() * (1.0f - press);
    const float s     = S * hover;
    auto at = [&](float x, float y) { return tip + la::float2{x * s, y * s}; };
    auto cap = [&](float ax, float ay, float bx, float by, float r, int chain) {
        out.push_back({at(ax, ay), at(bx, by), r * s, chain});
    };

    const float tipSquash = 1.0f + 0.30f * press;

    // index (chain 1): distal + proximal
    cap(0.000f, 0.048f, 0.024f, 0.180f, 0.052f * tipSquash, 1);
    cap(0.024f, 0.180f, 0.065f, 0.335f, 0.058f, 1);
    // palm mass (chain 0): ONE fat angled capsule. Deliberately a single
    // shape — every extra overlapping capsule adds iterative smooth-min
    // bias, which plateaus the |d|<outline rim band into a black blob.
    cap(0.135f, 0.465f, 0.225f, 0.580f, 0.195f, 0);
    // curled fingers (chains 2..4): knuckle scallops along the right
    // silhouette — centers sit at/over the palm edge so each bump pokes
    // out (image-1 style) and only a short separator runs interior.
    cap(0.230f, 0.330f, 0.320f, 0.380f, 0.075f, 2);
    cap(0.310f, 0.425f, 0.390f, 0.475f, 0.066f, 3);
    cap(0.360f, 0.525f, 0.425f, 0.570f, 0.056f, 4);
    // thumb (chain 5): pokes out of the left silhouette
    cap(0.020f, 0.400f, -0.105f, 0.300f, 0.070f, 5);
}

// Pinch hand: index tip at q0, thumb tip at q1, palm hanging below the
// midpoint. Orientation follows the pointer pair, so pinch-rotate spins
// the whole hand.
void pinchHand(std::vector<Cap>& out, la::float2 q0, la::float2 q1, float S, float press) {
    la::float2 axis = q1 - q0;
    float      len  = la::length(axis);
    la::float2 X    = len > 1e-5f ? axis / len : la::float2{1, 0};
    la::float2 Y{-X.y, X.x};
    if (Y.y < 0) Y = -Y;  // palm hangs toward screen-down

    const float hover = 1.0f + tweakHoverScale.get() * (1.0f - press);
    // A hand's fingers rotate apart, they don't stretch: when the tip
    // spread exceeds the hand's natural reach, grow the whole hand so
    // proportions hold at any pinch width.
    const float s   = std::max(S * hover, len * 1.05f);
    la::float2  mid = (q0 + q1) * 0.5f;

    auto cap = [&](la::float2 a, la::float2 b, float r, int chain) {
        out.push_back({a, b, r * s, chain});
    };

    // palm mass (chain 0): one wide fist capsule close under the tips, so
    // the finger V stays shallow (single shape — see pointing-hand note).
    la::float2 palmB = mid + Y * (0.30f * s) + X * (0.03f * s);
    cap(palmB - X * (0.11f * s), palmB + X * (0.11f * s), 0.165f, 0);
    // index (chain 1): slim finger, tip to the near shoulder
    la::float2 shoulderI = palmB - X * (0.08f * s) - Y * (0.10f * s);
    cap(q0, q0 + (shoulderI - q0) * 0.60f, 0.046f, 1);
    cap(q0 + (shoulderI - q0) * 0.55f, shoulderI, 0.054f, 1);
    // thumb (chain 5): shorter, thicker, hugging the palm
    la::float2 shoulderT = palmB + X * (0.09f * s) - Y * (0.06f * s);
    cap(q1, q1 + (shoulderT - q1) * 0.62f, 0.058f, 5);
    cap(q1 + (shoulderT - q1) * 0.55f, shoulderT, 0.066f, 5);
    // No curled-finger scallops at pinch proportions — they tangle with
    // the index shoulder and read as dirt; a clean mitten fist wins.
}

}  // namespace

void drawHand(const Context& ctx, const Player& player, const Rect& areaPts) {
    auto ps = player.pointers();
    if (ps.empty()) return;

    float opacity = ps[0].opacity;
    for (const PointerState& s : ps) opacity = std::min(opacity, s.opacity);
    // Fully faded (loop gap / finished) — draw nothing at all.
    if (opacity <= 0.003f) return;

    if (!ensureState()) return;

    auto toPts = [&](la::float2 u) {
        return la::float2{areaPts.x + u.x * areaPts.w, areaPts.y + u.y * areaPts.h};
    };

    const float S = tweakHandSizePt.get() * ctx.deviceUiScale();

    std::vector<Cap> caps;
    caps.reserve(kMaxCaps);
    if (ps.size() >= 2)
        pinchHand(caps, toPts(ps[0].pos), toPts(ps[1].pos), S, std::max(ps[0].press, ps[1].press));
    else
        pointingHand(caps, toPts(ps[0].pos), S, ps[0].press);

    if (caps.size() > size_t(kMaxCaps)) {
        SPDLOG_WARN("ge::hint: {} capsules exceeds shader budget {}; truncating", caps.size(),
                    kMaxCaps);
        caps.resize(kMaxCaps);
    }

    // Widths are authored as fractions of hand size; the shader works in pts.
    const float ow = tweakOutlineW.get() * S;
    const float hw = tweakHaloW.get() * S;

    // Bounding quad over every capsule, padded for rim + halo + AA slack.
    la::float2 lo{1e9f, 1e9f}, hi{-1e9f, -1e9f};
    float maxR = 0;
    for (const Cap& c : caps) {
        lo   = linalg::min(lo, linalg::min(c.a, c.b));
        hi   = linalg::max(hi, linalg::max(c.a, c.b));
        maxR = std::max(maxR, c.r);
    }
    const float pad = maxR + ow + hw + 2.0f;
    lo -= la::float2{pad, pad};
    hi += la::float2{pad, pad};

    const float quad[12] = {lo.x, lo.y, hi.x, lo.y, hi.x, hi.y,
                            lo.x, lo.y, hi.x, hi.y, lo.x, hi.y};
    auto& s = st();
    sg_range vr{quad, sizeof(quad)};
    int      offset = sg_append_buffer(s.stream, &vr);
    if (sg_query_buffer_overflow(s.stream)) {
        SPDLOG_WARN("ge::hint: vertex stream overflow; hand dropped this frame");
        return;
    }

    sg_apply_pipeline(s.pip);
    sg_bindings b{};
    b.vertex_buffers[0]        = s.stream;
    b.vertex_buffer_offsets[0] = offset;
    sg_apply_bindings(&b);

    // Point space → clip.
    const Rect full = ctx.fullRectInPts();
    la::float4x4 mvp = ortho::pixelOrtho(full.w, full.h);
    hint_vs_params_t vsp{};
    std::memcpy(vsp.u_mvp, &mvp[0][0], sizeof(vsp.u_mvp));
    sg_range vpr{&vsp, sizeof(vsp)};
    sg_apply_uniforms(UB_hint_vs_params, &vpr);

    hint_fs_params_t fsp{};
    for (size_t i = 0; i < caps.size(); ++i) {
        fsp.u_caps_ab[i][0] = caps[i].a.x;
        fsp.u_caps_ab[i][1] = caps[i].a.y;
        fsp.u_caps_ab[i][2] = caps[i].b.x;
        fsp.u_caps_ab[i][3] = caps[i].b.y;
        fsp.u_caps_rc[i][0] = caps[i].r;
        fsp.u_caps_rc[i][1] = float(caps[i].chain);
    }
    fsp.u_meta[0]   = float(caps.size());
    fsp.u_meta[1]   = tweakSmoothK.get() * S;
    fsp.u_meta[2]   = opacity;
    fsp.u_widths[0] = ow;
    fsp.u_widths[1] = hw;
    fsp.u_widths[2] = tweakStrokeW.get() * S;
    fsp.u_widths[3] = tweakStrokeFade.get() * S;
    // Sticker palette: white fill, black rim, white halo.
    fsp.u_fill[0] = 1;  fsp.u_fill[1] = 1;  fsp.u_fill[2] = 1;  fsp.u_fill[3] = 1;
    fsp.u_line[0] = 0;  fsp.u_line[1] = 0;  fsp.u_line[2] = 0;  fsp.u_line[3] = 1;
    fsp.u_halo[0] = 1;  fsp.u_halo[1] = 1;  fsp.u_halo[2] = 1;  fsp.u_halo[3] = 1;
    sg_range fpr{&fsp, sizeof(fsp)};
    sg_apply_uniforms(UB_hint_fs_params, &fpr);

    sg_draw(0, 6, 1);
}

}  // namespace ge::hint
