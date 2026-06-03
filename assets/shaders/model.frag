#version 330 core
in vec3 v_normal;
in vec3 v_worldpos;
uniform vec3 u_color;
uniform vec3 u_emissive;      // self-lit term (not attenuated)
uniform vec3 u_light_pos;     // nearest torch flame position
uniform vec3 u_light_color;   // torch color * flicker intensity (0 = no torch)
uniform float u_light_radius; // falloff distance
out vec4 frag_color;
void main() {
    vec3 n = normalize(v_normal);

    // Existing flat directional light as ambient/fill.
    vec3 ldir = normalize(vec3(0.4, 1.0, 0.3));
    float diffuse = max(dot(n, ldir), 0.0);
    float shade = 0.35 + 0.65 * diffuse;

    // Point light from the nearest torch, with smooth radial falloff.
    vec3 toL = u_light_pos - v_worldpos;
    float dist = length(toL);
    float atten = clamp(1.0 - dist / u_light_radius, 0.0, 1.0);
    atten *= atten;
    float ndl = max(dot(n, normalize(toL)), 0.0);
    vec3 point = u_light_color * ndl * atten;

    vec3 lit = u_color * shade + u_color * point + u_emissive;
    frag_color = vec4(lit, 1.0);
}
