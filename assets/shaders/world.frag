#version 330 core
in vec3 v_normal;
in vec2 v_uv;
in vec3 v_worldpos;
flat in float v_layer;
uniform sampler2DArray u_tex;
uniform vec3 u_light_pos;     // nearest torch flame position
uniform vec3 u_light_color;   // torch color * flicker intensity (0 = no torch)
uniform float u_light_radius; // falloff distance
uniform int  u_use_solid;     // 1 = solid-color terrain floor, 0 = textured walls
uniform vec3 u_solid;         // terrain base color when u_use_solid
out vec4 frag_color;
void main() {
    vec3 n = normalize(v_normal);

    // Key light angled toward the horizon so slopes read (a top-down light would make
    // a heightfield look flat). Ramp cool-shadow -> warm-light by the diffuse term: a
    // low-poly trick that makes faceted relief pop without any shadow-mapping pass.
    vec3 ldir = normalize(vec3(0.55, 0.45, 0.7));
    float diffuse = max(dot(n, ldir), 0.0);
    vec3 cool = vec3(0.45, 0.52, 0.72);   // shadowed-side tint (also the ambient floor)
    vec3 warm = vec3(1.10, 1.04, 0.90);   // lit-side tint
    vec3 shade = mix(cool * 0.7, warm, diffuse);

    // Point light from the nearest torch, with smooth radial falloff.
    vec3 toL = u_light_pos - v_worldpos;
    float dist = length(toL);
    float atten = clamp(1.0 - dist / u_light_radius, 0.0, 1.0);
    atten *= atten;
    float ndl = max(dot(n, normalize(toL)), 0.0);
    vec3 point = u_light_color * ndl * atten;

    vec3 base;
    if (u_use_solid != 0)
        base = u_solid * (0.92 + 0.025 * v_worldpos.y);   // subtle elevation tint for readability
    else
        base = texture(u_tex, vec3(v_uv, v_layer)).rgb;
    frag_color = vec4(base * (shade + point), 1.0);
}
