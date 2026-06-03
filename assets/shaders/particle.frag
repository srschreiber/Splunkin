#version 330 core
in vec4 v_color;
out vec4 frag_color;
void main() {
    // Additive blending is set by the renderer; alpha modulates the glow.
    frag_color = v_color;
}
