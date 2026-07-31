// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T170 SDF hand renderer — capsule skeleton on the CPU, implicit
// outline in the fragment shader (see src/render/shaders/ge_hint.glsl).
// 🎯T180 Pointing-hand articulation: small motion is finger/wrist,
// large motion is bodily translate, overflow pivots about the wrist.
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
#include <unordered_map>
#include <vector>

namespace ge::hint {
namespace {

// ── look knobs (fractions of hand size unless noted) ────────────────

tweak::Tweak<float> tweakHandSizePt{"hint.hand_size_pt", 110.0f};
tweak::Tweak<float> tweakSmoothK{"hint.smooth_k", 0.026f};
tweak::Tweak<float> tweakOutlineW{"hint.outline_w", 0.030f};
tweak::Tweak<float> tweakHaloW{"hint.halo_w", 0.028f};
tweak::Tweak<float> tweakStrokeW{"hint.stroke_w", 0.008f};
tweak::Tweak<float> tweakStrokeFade{"hint.stroke_fade", 0.040f};
tweak::Tweak<float> tweakHoverScale{"hint.hover_scale", 0.10f};

// 🎯T180 articulation (fractions of hand size)
tweak::Tweak<float> tweakArticSoft{"hint.artic_soft", 0.10f};
tweak::Tweak<float> tweakArticHard{"hint.artic_hard", 0.55f};
tweak::Tweak<float> tweakArticFingerMax{"hint.artic_finger_max", 0.22f};

constexpr int kMaxCaps = 24;  // must match u_caps_* array size in ge_hint.glsl

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

float smoothstep01(float edge0, float edge1, float x) {
    if (edge1 <= edge0) return x >= edge1 ? 1.0f : 0.0f;
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

la::float2 rot2(la::float2 v, float ang) {
    float c = std::cos(ang), s = std::sin(ang);
    return {c * v.x - s * v.y, s * v.x + c * v.y};
}

// Per-endpoint follow weight: fingertip (near local origin on chain 1)
// stays pinned to the contact tip; palm fully takes body shift.
float followWeight(int chain, float localY) {
    if (chain != 1) return 1.0f;
    // Index: y=0 at tip → weight 0; y≈0.33 at knuckle → weight ~1.
    return std::clamp(localY / 0.30f, 0.0f, 1.0f);
}

// Pinch hand: index tip at q0, thumb tip at q1, palm hanging below the
// midpoint. Orientation follows the pointer pair, so pinch-rotate spins
// the whole hand.
void pinchHand(std::vector<Capsule>& out, la::float2 q0, la::float2 q1, float S, float press) {
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
}

// Body-home state per Player (two hands can draw in one frame).
struct BodyHomeState {
    la::float2 home{};
    bool       primed = false;
};

std::unordered_map<const Player*, BodyHomeState>& bodyHomes() {
    static std::unordered_map<const Player*, BodyHomeState> m;
    return m;
}

}  // namespace

PointingLayout layoutPointingHand(la::float2 tip,
                                  float handSize,
                                  float press,
                                  la::float2 bodyHome,
                                  la::float2* outBodyHome,
                                  PointingLayoutParams params) {
    const float S     = std::max(handSize, 1e-3f);
    const float hover = 1.0f + tweakHoverScale.get() * (1.0f - press);
    const float s     = S * hover;

    la::float2 delta = tip - bodyHome;
    float      mag   = la::length(delta);
    float      bodyT = smoothstep01(params.softReach, params.hardReach, mag / S);

    // Origin of the rest skeleton (local tip attachment) after bodily follow.
    la::float2 origin = bodyHome + delta * bodyT;
    if (outBodyHome) *outBodyHome = origin;

    // Finger residual the articulation must cover.
    la::float2 finger = tip - origin;
    float      fLen   = la::length(finger);
    float      maxF   = params.fingerMax * S;

    // Wrist pivot when residual exceeds finger reach — rotate about the
    // local wrist so the gesture reads as a finger action, not a pan.
    float wristAng = 0.0f;
    // Rest "into-hand" direction from tip (local +y-ish down-right).
    constexpr la::float2 kRestIntoHand{0.12f, 0.55f};
    if (fLen > maxF && fLen > 1e-6f) {
        la::float2 want = finger / fLen;
        la::float2 rest = kRestIntoHand / la::length(kRestIntoHand);
        wristAng = std::atan2(want.y, want.x) - std::atan2(rest.y, rest.x);
        // Soft-clip: grow smoothly past the threshold rather than a hard snap.
        float over = std::clamp((fLen - maxF) / std::max(maxF, 1e-3f), 0.0f, 2.0f);
        wristAng *= smoothstep01(0.0f, 1.0f, over);
    }

    // bodyShift applied to palm-heavy points: when bodyT=0 this equals
    // bodyHome-tip so the palm stays at bodyHome while the tip is pinned.
    la::float2 bodyShift = origin - tip;

    auto xform = [&](float lx, float ly, int chain) -> la::float2 {
        la::float2 local{lx, ly};
        // Rotate about local wrist so overflow pivots rather than pans.
        constexpr la::float2 kWrist{0.040f, 0.280f};
        la::float2 rotated = kWrist + rot2(local - kWrist, wristAng);
        float w = followWeight(chain, ly);
        return tip + rotated * s + bodyShift * w;
    };

    const float tipSquash = 1.0f + 0.30f * press;
    PointingLayout layout;
    layout.tip        = tip;
    layout.wristAngle = wristAng;
    layout.capsules.reserve(8);

    auto cap = [&](float ax, float ay, float bx, float by, float r, int chain) {
        layout.capsules.push_back(
            {xform(ax, ay, chain), xform(bx, by, chain), r * s, chain});
    };

    // Index (chain 1): distal starts at local tip (0,0) so contact tip
    // identity holds after xform (followWeight(1,0)==0 → pure tip pin).
    cap(0.000f, 0.000f, 0.022f, 0.175f, 0.050f * tipSquash, 1);
    cap(0.022f, 0.175f, 0.058f, 0.330f, 0.056f, 1);
    // Palm mass (chain 0): ONE fat angled capsule.
    cap(0.125f, 0.455f, 0.205f, 0.570f, 0.190f, 0);
    // Curled fingers (chains 2..4)
    cap(0.195f, 0.345f, 0.255f, 0.385f, 0.062f, 2);
    cap(0.265f, 0.430f, 0.320f, 0.470f, 0.056f, 3);
    cap(0.315f, 0.520f, 0.360f, 0.555f, 0.048f, 4);
    // Thumb (chain 5)
    cap(0.015f, 0.395f, -0.075f, 0.315f, 0.058f, 5);

    // Palm centroid from the palm capsule only.
    la::float2 palmSum{0, 0};
    int        palmN = 0;
    for (const Capsule& c : layout.capsules) {
        if (c.chain != 0) continue;
        palmSum += (c.a + c.b) * 0.5f;
        ++palmN;
    }
    layout.palmCentroid = palmN > 0 ? palmSum / float(palmN) : tip;

    return layout;
}

void drawHand(const Context& ctx, const Player& player, const Rect& areaPts) {
    auto ps = player.pointers();
    if (ps.empty()) return;

    float opacity = ps[0].opacity;
    for (const PointerState& s : ps) opacity = std::min(opacity, s.opacity);
    // Fully faded (loop gap / finished) — draw nothing at all; drop artic state.
    if (opacity <= 0.003f) {
        bodyHomes().erase(&player);
        return;
    }

    if (!ensureState()) return;

    auto toPts = [&](la::float2 u) {
        return la::float2{areaPts.x + u.x * areaPts.w, areaPts.y + u.y * areaPts.h};
    };

    const float S = tweakHandSizePt.get() * ctx.deviceUiScale();

    std::vector<Capsule> caps;
    caps.reserve(kMaxCaps);
    if (ps.size() >= 2) {
        pinchHand(caps, toPts(ps[0].pos), toPts(ps[1].pos), S,
                  std::max(ps[0].press, ps[1].press));
    } else {
        la::float2 tip = toPts(ps[0].pos);
        auto&      bh  = bodyHomes()[&player];
        if (!bh.primed) {
            bh.home   = tip;
            bh.primed = true;
        }
        PointingLayoutParams ap;
        ap.softReach = tweakArticSoft.get();
        ap.hardReach = tweakArticHard.get();
        ap.fingerMax = tweakArticFingerMax.get();
        auto layout  = layoutPointingHand(tip, S, ps[0].press, bh.home, &bh.home, ap);
        caps         = std::move(layout.capsules);
    }

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
    for (const Capsule& c : caps) {
        lo   = linalg::min(lo, linalg::min(c.a, c.b));
        hi   = linalg::max(hi, linalg::max(c.a, c.b));
        maxR = std::max(maxR, c.radius);
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
        fsp.u_caps_rc[i][0] = caps[i].radius;
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
