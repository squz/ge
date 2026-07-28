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

// A sokol-backed 2D image that OWNS its GPU resources (🎯T135). Move-only:
// the destructor frees the underlying sg_image + sg_view, and move-assignment
// frees the slot's previous resources before taking the new ones — so the
// common re-rasterize pattern `m->badge = rasterizeSvg(...)` releases the old
// image/view instead of orphaning them. Orphaning one image+view per reassign
// eventually exhausts sokol's small pools (image 128 / view 256) and aborts in
// sg_make_image / sg_make_view — that was multimaze2's tap-correlated iPhone-13
// SIGABRT. Copying is deleted to prevent a double-free of the shared handles;
// take a Sprite by `const&` (draw / SpriteBatch::addSprite do) and std::move to
// transfer ownership.
//
// Release is guarded on sg_isvalid(), so a Sprite destroyed after sg_shutdown
// (or in a headless unit test with no sokol context) is a safe no-op rather
// than a call into a dead pool.
//
// Model space is the unit square (0..1)² with the source image filling it.
struct Sprite {
    sg_image tex    = {};
    sg_view  view   = {};   // texture-view wrapping `tex` (sokol's new binding model)
    int      width  = 0;
    int      height = 0;

    Sprite() = default;
    ~Sprite();
    Sprite(Sprite&&) noexcept;
    Sprite& operator=(Sprite&&) noexcept;
    Sprite(const Sprite&)            = delete;
    Sprite& operator=(const Sprite&) = delete;

    bool isNull() const { return tex.id == SG_INVALID_ID; }

    // Free this sprite's GPU resources now and reset to null. Invoked by the
    // destructor and by move-assignment; consumers rarely call it directly
    // (RAII covers the common cases) but it's available for explicit early
    // release. Safe to call repeatedly and on a null sprite.
    void destroy();

    // Draw a unit-square quad covering this sprite's image. `mvp` is
    // the model-view-projection matrix taking the unit square through
    // the active world transform and projection to clip space.
    // Premultiplied-alpha blend state is baked into the pipeline.
    void draw(const la::float4x4& mvp) const;
};

namespace detail {
// 🎯T135 Test / diagnostic seam: cumulative count of non-null Sprite GPU-handle
// releases (destructor + move-assignment of an occupied slot). A headless test
// (no live sokol context to read pool occupancy) uses this to prove that
// re-rasterizing one slot N times triggers N releases — i.e. no orphaning.
uint64_t spriteReleaseCount();
}  // namespace detail

struct SpriteVertex {
    float    x, y, z;
    float    u, v;
    uint32_t abgr;
};

// Batched sprite renderer. `addSprite` queues a quad with the
// model-to-world transform baked into the vertex positions on the CPU.
// `submit(worldToClip)` flushes runs of same-texture sprites, one
// draw call per (texture) run.
// 🎯T176 Form a Sprite from raw RGBA8 pixels (premultiplied alpha), the
// upload path rasterizeText/rasterizeSvg use internally — promoted so
// background-baked pixels (rasterizeTextToPixels on a worker thread)
// have a first-class way onto the GPU. GAME-THREAD ONLY, like every
// sg_* call: bake pixels anywhere, form the Sprite on the game thread.
// Null Sprite when sokol is not ready or the input is empty.
Sprite spriteFromRgba(int width, int height, const uint8_t* rgba,
                      const char* label = "ge.sprite.rgba");

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
    friend struct SpriteBatchTestAccess;  // unit-test access to quads_
    std::vector<Quad> quads_;
};

} // namespace ge
