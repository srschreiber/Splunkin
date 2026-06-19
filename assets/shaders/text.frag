#version 330 core
in vec2 v_uv;
in vec4 v_color;
out vec4 frag_color;
uniform sampler2D u_atlas;   // single-channel (R8) coverage from the baked font
void main() {
    // Atlas stores per-texel glyph coverage in .r; the tint supplies the color and the
    // overall opacity. Linear-blended by the renderer (SRC_ALPHA / ONE_MINUS_SRC_ALPHA).
    float coverage = texture(u_atlas, v_uv).r;
    frag_color = vec4(v_color.rgb, v_color.a * coverage);
}
