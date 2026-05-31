#version 330 core
in vec3 v_normal;
in vec2 v_uv;
flat in float v_layer;
uniform sampler2DArray u_tex;
out vec4 frag_color;
void main() {
    vec3 light_dir = normalize(vec3(0.4, 1.0, 0.3));
    float diffuse = max(dot(normalize(v_normal), light_dir), 0.0);
    float shade = 0.35 + 0.65 * diffuse;
    vec4 tex = texture(u_tex, vec3(v_uv, v_layer));
    frag_color = vec4(tex.rgb * shade, 1.0);
}
