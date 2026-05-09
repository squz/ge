// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <ge/SessionHost.h>  // ge::Rect

#include <bgfx/bgfx.h>

#include <cstdint>
#include <vector>

namespace ge {

// Vertex layout for sprite/quad geometry. Consumers that build their own
// transient vertex buffers against this layout should declare:
//
//   bgfx::VertexLayout layout;
//   layout.begin()
//       .add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
//       .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
//       .add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, /*normalized=*/true)
//       .end();
//
// The layout matches ge_sprite_{vs,fs}.bin (compiled from
// src/render/shaders/ge_sprite_{vs,fs}.sc).
struct SpriteVertex {
    float    x, y, z;   // position
    float    u, v;      // texcoord (normalised 0..1)
    uint32_t abgr;      // tint colour, ABGR byte order (0xAABBGGRR)
                        // Pass 0xFFFFFFFF for no tint.
                        // The texture is expected to be premultiplied-alpha;
                        // v_color0 is multiplied with the sampled texel.
                        // Tints with alpha < 1 must themselves be
                        // premultiplied by the caller.
};

// Batched textured-quad renderer.
//
// Usage:
//   SpriteBatch batch;
//   batch.addSprite(cx, cy, hw, hh, tex, uvL, uvT, uvR, uvB);
//   batch.submit(view);
//   // next frame:
//   batch.clear();
//
// submit() is idempotent for an empty batch.
// Blend state defaults to premultiplied-alpha (ONE, INV_SRC_ALPHA).
// Call setBlendState() before submit() to override for a single flush.
//
// Textures are grouped: all sprites sharing the same bgfx::TextureHandle
// are flushed together in one draw call per (texture, view) pair.
// Ordering within a texture group follows insertion order; relative order
// across different textures is preserved by flush order (first texture seen
// draws first).
class SpriteBatch {
public:
    SpriteBatch();
    ~SpriteBatch();

    // Remove all queued sprites without submitting. Call at the start of
    // each frame.
    void clear();

    // Override the blend state for the next submit() call.
    // Resets to the premultiplied-alpha default after submit().
    void setBlendState(uint64_t state);

    // Queue a textured quad centred at (centerX, centerY) with half-extents
    // (halfW, halfH) in whatever coordinate space the caller's view+projection
    // matrix uses.
    //
    // `tex` must be a valid bgfx::TextureHandle.
    //
    // UV arguments (uvL, uvT, uvR, uvB) are normalised (0..1). The top-left
    // corner of the texture is (uvL, uvT); the bottom-right is (uvR, uvB).
    // For a full-texture quad, pass uvL=0, uvT=0, uvR=1, uvB=1.
    //
    // `color` is ABGR (0xAABBGGRR). Pass 0xFFFFFFFF for no tint.
    void addSprite(float centerX, float centerY, float halfW, float halfH,
                   bgfx::TextureHandle tex,
                   float uvL, float uvT, float uvR, float uvB,
                   uint32_t color = 0xFFFFFFFF);

    // Queue a textured quad that fills `dest` (pixel rect in render-surface
    // coordinates). `uvs` is a normalised UV rect: uvs.x/y is the top-left
    // texcoord, uvs.w/h is the width/height of the UV window (not the
    // bottom-right corner). For a full-texture quad, pass Rect{0,0,1,1}.
    //
    // `tex` must be a valid bgfx::TextureHandle.
    //
    // `color` is ABGR (0xAABBGGRR). Pass 0xFFFFFFFF for no tint.
    void addQuad(const Rect& dest, bgfx::TextureHandle tex,
                 const Rect& uvs = Rect{0, 0, 1, 1},
                 uint32_t color = 0xFFFFFFFF);

    // Flush all queued sprites to `view` using the sprite shader program.
    // Issues one bgfx::submit per distinct texture.
    // Does nothing if the batch is empty or the program fails to load.
    // Resets blend state to default after the call.
    void submit(bgfx::ViewId view);

    // Internal quad record — public so test helpers can inspect geometry
    // without bgfx initialisation.
    struct Sprite {
        bgfx::TextureHandle tex;
        SpriteVertex        verts[6];  // two triangles, CCW
    };

private:
    friend struct SpriteBatchTestAccess;

    std::vector<Sprite> sprites_;
    uint64_t            blendState_;

    // Lazy-initialised bgfx resources (detail in SpriteBatch.cpp).
    bool ensureState();
};

} // namespace ge
