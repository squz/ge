// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// 🎯T170 Gesture-hint SDF hand — capsule-scene renderer.
//
// The hand is modelled implicitly: a list of capsules (palm blob +
// finger segments) combined with a smooth-min union. The fragment
// stage re-derives the outline from that skeleton every frame, so
// fingers merge smoothly into the palm as they move — no textures,
// no atlas, resolution-independent at any zoom.
//
// Bands of the one distance field give the sticker look:
//   fill  : d < 0                      (white)
//   line  : |d| < outline              (black rim)
//   halo  : d < outline + halo         (white sticker edge)
// Interior finger-separator strokes are each finger chain's own
// outline, clipped to the union interior and faded by depth inside
// the palm field — lines that stop short, image-2 style.
//
// Coordinates are point space (matching the capsule uniforms); AA via
// fwidth so any on-screen scale stays crisp. Output is premultiplied
// RGBA (same blend convention as ge::Sprite).

@vs vs
layout(binding=0) uniform hint_vs_params {
    mat4 u_mvp;   // pts -> clip
};

in vec2 a_pos;    // pts
out vec2 v_pos;

void main() {
    v_pos = a_pos;
    gl_Position = u_mvp * vec4(a_pos, 0.0, 1.0);
}
@end

@fs fs
layout(binding=1) uniform hint_fs_params {
    vec4 u_caps_ab[24];  // xy = endpoint a, zw = endpoint b (pts)
    vec4 u_caps_rc[24];  // x = radius, y = chain id (0 = palm), zw unused
    vec4 u_meta;         // x = capsule count, y = smooth-min k, z = opacity
    vec4 u_widths;       // x = outline, y = halo, z = stroke halfwidth, w = stroke fade depth
    vec4 u_fill;         // straight-alpha colours
    vec4 u_line;
    vec4 u_halo;
};

in vec2 v_pos;
out vec4 frag_color;

float sdCapsule(vec2 p, vec2 a, vec2 b, float r) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / max(dot(ba, ba), 1e-8), 0.0, 1.0);
    return length(pa - ba * h) - r;
}

float smin(float a, float b, float k) {
    float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
    return mix(b, a, h) - k * h * (1.0 - h);
}

void main() {
    vec2  p     = v_pos;
    int   n     = int(u_meta.x);
    float k     = u_meta.y;
    float d     = 1e6;
    float dPalm = 1e6;
    float dch[8];
    for (int c = 0; c < 8; ++c) dch[c] = 1e6;

    for (int i = 0; i < 24; ++i) {
        if (i >= n) break;
        float di = sdCapsule(p, u_caps_ab[i].xy, u_caps_ab[i].zw, u_caps_rc[i].x);
        d = smin(d, di, k);
        int c = int(u_caps_rc[i].y);
        dch[c] = min(dch[c], di);
        if (c == 0) dPalm = min(dPalm, di);
    }

    float aa = fwidth(d);
    float ow = u_widths.x, hw = u_widths.y, sw = u_widths.z, fadeDepth = u_widths.w;

    float mFill = 1.0 - smoothstep(-aa, aa, d);
    float mLine = 1.0 - smoothstep(ow - aa, ow + aa, abs(d));
    float mHalo = 1.0 - smoothstep(ow + hw - aa, ow + hw + aa, d);

    // Interior separator strokes (finger chains only, chain id >= 1).
    float mStroke = 0.0;
    for (int c = 1; c < 8; ++c) {
        float dc = dch[c];
        if (dc > 1e5) continue;
        float aac    = fwidth(dc);
        float line   = 1.0 - smoothstep(sw - aac, sw + aac, abs(dc));
        float inside = 1.0 - smoothstep(-2.0 * aa, 0.0, d + ow);
        float depth  = 1.0 - smoothstep(0.0, fadeDepth, -dPalm);
        mStroke = max(mStroke, line * inside * depth);
    }

    // Premultiplied composite, bottom-up: halo, fill, then line work.
    vec4 acc   = vec4(u_halo.rgb, 1.0) * (u_halo.a * mHalo);
    vec4 fillL = vec4(u_fill.rgb, 1.0) * (u_fill.a * mFill);
    acc = fillL + acc * (1.0 - fillL.a);
    vec4 lineL = vec4(u_line.rgb, 1.0) * (u_line.a * max(mLine, mStroke));
    acc = lineL + acc * (1.0 - lineL.a);

    frag_color = acc * u_meta.z;
}
@end

@program hint vs fs
