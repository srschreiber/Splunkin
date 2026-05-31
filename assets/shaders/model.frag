#version 330 core
in vec3 v_normal;
uniform vec3 u_color;
out vec4 frag_color;
void main() {
    vec3 light_dir = normalize(vec3(0.4, 1.0, 0.3));
    float diffuse = max(dot(normalize(v_normal), light_dir), 0.0);
    float shade = 0.35 + 0.65 * diffuse;
    frag_color = vec4(u_color * shade, 1.0);
}
