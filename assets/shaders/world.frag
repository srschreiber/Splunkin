#version 330 core
in vec3 v_normal;
in vec3 v_color;
out vec4 frag_color;
void main() {
    vec3 light_dir = normalize(vec3(0.4, 1.0, 0.3));
    float diffuse = max(dot(normalize(v_normal), light_dir), 0.0);
    float ambient = 0.35;
    float shade = ambient + (1.0 - ambient) * diffuse;
    frag_color = vec4(v_color * shade, 1.0);
}
