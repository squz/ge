// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Solid-color mesh fill — generic MVP vertex transform + flat fragment colour.
// No textures, lighting, or normals. For unlit silhouettes, debug volumes, etc.
//
// Colour is premultiplied RGBA (same blend convention as ge::Sprite).

@vs vs
layout(binding=0) uniform solid_vs_params {
    mat4 u_mvp;
};

in vec3 a_position;

void main() {
    gl_Position = u_mvp * vec4(a_position, 1.0);
}
@end

@fs fs
layout(binding=1) uniform solid_fs_params {
    vec4 u_color;  // premultiplied RGBA
};

out vec4 frag_color;

void main() {
    frag_color = u_color;
}
@end

@program solid vs fs
