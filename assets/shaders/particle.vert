#version 330 core
layout(location=0) in vec3 a_pos;     // pre-billboarded world position
layout(location=1) in vec4 a_color;   // rgba (alpha fades with age)
uniform mat4 u_viewproj;
out vec4 v_color;
void main() {
    v_color = a_color;
    gl_Position = u_viewproj * vec4(a_pos, 1.0);
}
