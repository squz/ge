// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// T38 spike: sokol_gfx port.
#pragma once

#include <ge/Linalg.h>
#include <ge/SessionHost.h>  // ge::Rect

#include "sokol_gfx.h"

#include <cstdint>
#include <vector>

namespace ge {

// A sokol-backed 2D image. Model space is the unit square (0..1)²
// with the source image filling it.
struct Sprite {
    sg_image tex    = {};
    sg_view  view   = {};   // texture-view wrapping `tex` (sokol's new binding model)
    int      width  = 0;
    int      height = 0;

    bool isNull() const { return tex.id == SG_INVALID_ID; }

    // Draw a unit-square quad covering this sprite's image. `mvp` is
    // the model-view-projection matrix taking the unit square through
    // the active world transform and projection to clip space.
    // Premultiplied-alpha blend state is baked into the pipeline.
    void draw(const la::float4x4& mvp) const;
};

struct SpriteVertex {
    float    x, y, z;
    float    u, v;
    uint32_t abgr;
};

// Batched sprite renderer. `addSprite` queues a quad with the
// model-to-world transform baked into the vertex positions on the CPU.
// `submit(worldToClip)` flushes runs of same-texture sprites, one
// draw call per (texture) run.
class SpriteBatch {
public:
    SpriteBatch();
    ~SpriteBatch();

    void clear();

    void addSprite(const la::float4x4& modelToWorld,
                   const Sprite&       sprite,
                   uint32_t            color = 0xFFFFFFFFu);

    void addSprite(const la::float4x4& modelToWorld,
                   const Sprite&       sprite,
                   Rect                uvSubRect,
                   uint32_t            color = 0xFFFFFFFFu);

    // Flush all queued sprites. `worldToClip` is the world-to-clip
    // matrix (vertex positions are already in world space).
    void submit(const la::float4x4& worldToClip);

    struct Quad {
        sg_image     tex;
        sg_view      view;
        SpriteVertex verts[6];
    };

private:
    std::vector<Quad> quads_;
};

} // namespace ge
