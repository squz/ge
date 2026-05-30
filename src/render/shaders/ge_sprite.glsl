// T38 spike: sokol-shdc port of ge_sprite_{vs,fs}.sc.
// Annotated GLSL (Vulkan dialect) for sokol-shdc cross-compile.
@vs vs
layout(binding=0) uniform vs_params {
    mat4 u_modelViewProj;
};

in vec3 a_position;
in vec2 a_texcoord0;
in vec4 a_color0;

out vec2 v_texcoord;
out vec4 v_color;

void main() {
    gl_Position = u_modelViewProj * vec4(a_position, 1.0);
    v_texcoord  = a_texcoord0;
    v_color     = a_color0;
}
@end

@fs fs
layout(binding=0) uniform texture2D s_tex;
layout(binding=0) uniform sampler   s_smp;

in vec2 v_texcoord;
in vec4 v_color;
out vec4 frag_color;

void main() {
    vec4 tex   = texture(sampler2D(s_tex, s_smp), v_texcoord);
    frag_color = tex * v_color;
}
@end

@program sprite vs fs
