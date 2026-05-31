#version 330 core
layout (location = 0) in vec3 a_pos;
layout (location = 1) in vec3 a_normal;
layout (location = 2) in vec2 a_uv;
layout (location = 3) in float a_layer;
uniform mat4 u_viewproj;
out vec3 v_normal;
out vec2 v_uv;
flat out float v_layer;
void main() {
    v_normal = a_normal;
    v_uv = a_uv;
    v_layer = a_layer;
    gl_Position = u_viewproj * vec4(a_pos, 1.0);
}
