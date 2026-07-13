// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Solid-color mesh draw — unlit, untextured triangle mesh with a flat colour.
//
// Generic building block: MVP × position in the vertex stage, constant colour
// in the fragment stage. Use for solid silhouettes (e.g. carousel country
// shapes), filled volumes, and any mesh that should not sample a material.
//
// Vertex buffers are position-first float3 (or any stride with position at
// offset 0). Indexed draws take a UINT16 index buffer. Premultiplied-alpha
// blend state matches ge::Sprite.
#pragma once

#include <ge/Linalg.h>

#include "sokol_gfx.h"

#include <cstdint>

namespace ge {

// Draw an indexed triangle mesh filled with a solid colour.
//
// `mvp`           — model-view-projection (clip space)
// `vbuf`          — vertex buffer; position is float3 at `vbuf_offset`
// `vbuf_offset`   — byte offset into `vbuf` for the first vertex
// `vertex_stride` — bytes per vertex (e.g. sizeof(MeshVertex) == 20)
// `ibuf`          — UINT16 index buffer
// `base_element`  — first index to draw (element index, not byte)
// `num_elements`  — number of indices
// `color`         — straight-alpha RGBA in [0,1]; converted to premultiplied
//                   in the draw path so callers match ge::Color elsewhere
// `cull`          — face cull mode for this draw (pipeline selected per cull)
//
// No-op if sokol is not ready. GPU resources are created lazily on first use.
void drawSolid(const la::float4x4& mvp,
               sg_buffer           vbuf,
               int                 vbuf_offset,
               int                 vertex_stride,
               sg_buffer           ibuf,
               int                 base_element,
               int                 num_elements,
               Color               color,
               sg_cull_mode        cull = SG_CULLMODE_BACK);

} // namespace ge
