// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// ge::debug overlay shader (🎯T97) — position + straight-alpha colour.
// One program drives both the line-list and triangle-list debug pipelines;
// the pipeline picks the primitive type, not the shader.
@vs vs
layout(binding=0) uniform vs_params {
    mat4 u_modelViewProj;
};

in vec3 a_position;
in vec4 a_color0;

out vec4 v_color;

void main() {
    gl_Position = u_modelViewProj * vec4(a_position, 1.0);
    v_color     = a_color0;
}
@end

@fs fs
in vec4 v_color;
out vec4 frag_color;

void main() {
    frag_color = v_color;
}
@end

@program ge_debug vs fs
