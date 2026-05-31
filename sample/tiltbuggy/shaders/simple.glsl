// T38 spike: sokol-shdc port of tiltbuggy simple_{vs,fs}.sc.
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

@program simple vs fs
