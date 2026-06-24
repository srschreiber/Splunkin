#version 330 core
in vec3 v_normal;
in vec3 v_worldpos;
uniform vec3 u_color;
uniform vec3 u_emissive;      // self-lit term (not attenuated)
#define MAX_LIGHTS 12
uniform int  u_light_count;
uniform vec3 u_light_pos[MAX_LIGHTS];
uniform vec3 u_light_color[MAX_LIGHTS];
uniform float u_light_radius[MAX_LIGHTS];
uniform float u_alpha;        // 1 = opaque; <1 for ghosts (dead players)
uniform float u_ambient;      // day/night ambient scale
out vec4 frag_color;
void main() {
    vec3 n = normalize(v_normal);

    // Existing flat directional light as ambient/fill.
    vec3 ldir = normalize(vec3(0.4, 1.0, 0.3));
    float diffuse = max(dot(n, ldir), 0.0);
    float shade = (0.14 + 0.30 * diffuse) * u_ambient;   // dim ambient/fill so point lights dominate

    // Sum all dynamic point lights (torches, flamethrowers, projectiles).
    vec3 point = vec3(0.0);
    for (int i = 0; i < u_light_count; ++i) {
        vec3 toL = u_light_pos[i] - v_worldpos;
        float dist = length(toL);
        float atten = clamp(1.0 - dist / u_light_radius[i], 0.0, 1.0);
        atten *= atten;
        float ndl = max(dot(n, normalize(toL)), 0.0);
        point += u_light_color[i] * ndl * atten;
    }

    vec3 lit = u_color * shade + u_color * point + u_emissive;
    frag_color = vec4(lit, u_alpha);
}
