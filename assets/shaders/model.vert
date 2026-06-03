#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec2 a_uv;
uniform mat4 u_viewproj;
uniform mat4 u_model;
out vec3 v_normal;
out vec3 v_worldpos;
void main() {
    mat3 nm = mat3(transpose(inverse(u_model)));
    v_normal = normalize(nm * a_normal);
    vec4 world = u_model * vec4(a_pos, 1.0);
    v_worldpos = world.xyz;
    gl_Position = u_viewproj * world;
}
