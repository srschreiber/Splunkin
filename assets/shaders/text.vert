#version 330 core
layout(location=0) in vec2 a_pos;     // position, already in NDC (clip space)
layout(location=1) in vec2 a_uv;      // glyph atlas coords
layout(location=2) in vec4 a_color;   // tint (alpha = overall opacity)
out vec2 v_uv;
out vec4 v_color;
void main() {
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
