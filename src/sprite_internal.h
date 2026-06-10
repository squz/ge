// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Engine-internal sprite helpers, split out so the overflow-spreading
// logic (🎯T113) can be unit-tested headlessly — without a live sokol
// context. NOT a public header: lives in src/, included only by
// sprite.cpp and sprite_test.cpp.
#pragma once

#include <vector>

namespace ge::detail {

// One executable step of a sprite run's draw plan (see planSpriteRun).
struct SpriteRunStep {
    bool newBuffer;  // advance to the next pooled stream buffer before this step
    int  verts;      // vertices to append + draw here (always a multiple of 6)
};

// Plan how to spread one same-texture run of `totalVerts` vertices (a
// multiple of 6 — whole quads) across fixed-capacity stream buffers so
// that NO quad is silently dropped on overflow (🎯T113). Where the old
// code logged a warning and returned when a run didn't fit the single
// 256 KB per-frame buffer, the run is now split across as many pooled
// buffers as it takes.
//
//   totalVerts     vertices in this run (6 per quad)
//   firstFreeVerts whole-quad capacity left in the *current* buffer
//   fullVerts      capacity of a *fresh* buffer
//
// The returned steps sum to `totalVerts` (nothing dropped) and each
// step's vertex count fits the buffer it lands in. The first step uses
// the current buffer unless it is already full, in which case the first
// step is flagged `newBuffer`.
std::vector<SpriteRunStep> planSpriteRun(int totalVerts,
                                         int firstFreeVerts,
                                         int fullVerts);

} // namespace ge::detail
