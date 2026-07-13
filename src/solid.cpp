// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::drawSolid — sokol_gfx solid-color mesh fill (🎯 solid mesh primitive).

#include <ge/solid.h>

#include "sokol_gfx.h"
#include "ge_solid.h"  // sokol-shdc generated; -I via Module.mk

#include <spdlog/spdlog.h>

#include <cstring>

namespace ge {
namespace {

struct State {
    sg_shader   shader = {};
    sg_pipeline cullBack  = {};
    sg_pipeline cullFront = {};
    sg_pipeline cullNone  = {};
    bool        ready = false;
};

State& st() {
    static State s;
    return s;
}

sg_pipeline makePip(sg_shader shd, sg_cull_mode cull, const char* label) {
    sg_pipeline_desc pd{};
    pd.shader = shd;
    // Position-only attr 0; stride set at draw via buffer layout... sokol
    // fixes stride on the pipeline. Use MeshVertex-sized default stride so
    // country meshes work; callers with different stride need matching
    // pipelines later if needed. ge::MeshVertex is 20 bytes.
    pd.layout.buffers[0].stride = 20;
    pd.layout.attrs[ATTR_solid_a_position].format = SG_VERTEXFORMAT_FLOAT3;
    pd.index_type = SG_INDEXTYPE_UINT16;
    pd.primitive_type = SG_PRIMITIVETYPE_TRIANGLES;
    pd.face_winding = SG_FACEWINDING_CCW;
    pd.cull_mode = cull;
    pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
    pd.depth.write_enabled = false;
    pd.colors[0].blend.enabled = true;
    pd.colors[0].blend.src_factor_rgb = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;
    pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    pd.label = label;
    return sg_make_pipeline(&pd);
}

bool ensureState() {
    auto& s = st();
    if (s.ready) return true;
    if (!sg_isvalid()) return false;

    s.shader = sg_make_shader(solid_shader_desc(sg_query_backend()));
    if (s.shader.id == SG_INVALID_ID) {
        SPDLOG_ERROR("ge::drawSolid: shader create failed");
        return false;
    }
    s.cullBack  = makePip(s.shader, SG_CULLMODE_BACK,  "ge.solid.cullBack");
    s.cullFront = makePip(s.shader, SG_CULLMODE_FRONT, "ge.solid.cullFront");
    s.cullNone  = makePip(s.shader, SG_CULLMODE_NONE,  "ge.solid.cullNone");
    s.ready = true;
    return true;
}

sg_pipeline pipelineFor(sg_cull_mode cull) {
    auto& s = st();
    switch (cull) {
        case SG_CULLMODE_FRONT: return s.cullFront;
        case SG_CULLMODE_NONE:  return s.cullNone;
        case SG_CULLMODE_BACK:
        default:                return s.cullBack;
    }
}

} // namespace

void drawSolid(const la::float4x4& mvp,
               sg_buffer           vbuf,
               int                 vbuf_offset,
               int                 vertex_stride,
               sg_buffer           ibuf,
               int                 base_element,
               int                 num_elements,
               Color               color,
               sg_cull_mode        cull) {
    if (num_elements <= 0 || vbuf.id == SG_INVALID_ID || ibuf.id == SG_INVALID_ID)
        return;
    if (!ensureState()) return;

    // Pipeline is fixed at stride 20 (MeshVertex). Other strides still work if
    // the first three floats are position and the buffer is tightly... actually
    // sokol uses pipeline buffer stride. Require MeshVertex stride for now.
    if (vertex_stride != 20) {
        SPDLOG_WARN("ge::drawSolid: vertex_stride {} != 20 (MeshVertex); draw may be wrong",
                    vertex_stride);
    }

    sg_pipeline pip = pipelineFor(cull);
    sg_apply_pipeline(pip);

    sg_bindings b{};
    b.vertex_buffers[0] = vbuf;
    b.vertex_buffer_offsets[0] = vbuf_offset;
    b.index_buffer = ibuf;
    sg_apply_bindings(&b);

    solid_vs_params_t vsp{};
    std::memcpy(vsp.u_mvp, &mvp[0][0], sizeof(vsp.u_mvp));
    sg_range vr{&vsp, sizeof(vsp)};
    sg_apply_uniforms(UB_solid_vs_params, &vr);

    // Premultiply straight-alpha colour for ONE/ONE_MINUS_SRC_ALPHA blend.
    float a = color.w < 0.f ? 0.f : (color.w > 1.f ? 1.f : color.w);
    solid_fs_params_t fsp{};
    fsp.u_color[0] = color.x * a;
    fsp.u_color[1] = color.y * a;
    fsp.u_color[2] = color.z * a;
    fsp.u_color[3] = a;
    sg_range fr{&fsp, sizeof(fsp)};
    sg_apply_uniforms(UB_solid_fs_params, &fr);

    sg_draw(base_element, num_elements, 1);
    (void)vertex_stride;
}

} // namespace ge
