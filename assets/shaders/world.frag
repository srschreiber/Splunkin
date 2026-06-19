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

// --- cheap value noise for color mottling (gives the flat-shaded ground some depth) ---
float hash21(vec2 p) {
    p = fract(p * vec2(127.1, 311.7));
    p += dot(p, p + 34.5);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i),             b = hash21(i + vec2(1, 0));
    float c = hash21(i + vec2(0, 1)), d = hash21(i + vec2(1, 1));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float fbm2(vec2 p) {   // a few octaves -> richer grain
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 3; ++i) { s += a * vnoise(p); p *= 2.0; a *= 0.5; }
    return s;
}

// Terrain albedo: blend several earthy tones by surface material (v_layer: 0 ground,
// 1 ramp, 2 plateau top), elevation and noise — so the ground reads as textured dirt
// and grass instead of one flat color. u_solid is the low-ground grass tone.
vec3 terrain_albedo(vec3 n) {
    vec3 grass = u_solid;                      // mossy green-brown (low, flat ground)
    vec3 dirt  = vec3(0.40, 0.33, 0.22);       // bare earth patches
    vec3 ramp_brown  = vec3(0.60, 0.47, 0.31); // lighter brown — the gradient up
    vec3 top_brown   = vec3(0.42, 0.30, 0.19); // darker/browner — the plateau tops

    // Two noise scales: big patches + fine grain, sampled in world XZ.
    float patch = fbm2(v_worldpos.xz * 0.06);
    float grain = fbm2(v_worldpos.xz * 0.55);

    // Open ground: grass mottled with dirt patches; bare dirt on the steeper bits.
    float elev  = clamp(v_worldpos.y / 12.0, 0.0, 1.0);
    float slope = clamp(1.0 - n.y, 0.0, 1.0);
    vec3 ground = mix(grass, dirt, smoothstep(0.45, 0.75, patch + 0.25 * elev));
    ground = mix(ground, dirt, smoothstep(0.35, 0.7, slope));   // worn earth on slopes

    // Pick by material tag (rounded — it's a flat-interpolated int stored as float).
    int mat = int(v_layer + 0.5);
    vec3 col = ground;
    if (mat == 1) col = mix(ramp_brown, dirt, 0.35 * grain);   // ramp: lighter brown
    if (mat == 2) col = mix(top_brown,  dirt, 0.30 * grain);   // top: browner

    // Fine-grain mottling across everything for a textured look (subtle).
    col *= 0.90 + 0.18 * grain;
    return col;
}

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
        base = terrain_albedo(n) * (0.96 + 0.02 * v_worldpos.y);   // blended dirt/grass + slight elevation tint
    else
        base = texture(u_tex, vec3(v_uv, v_layer)).rgb;
    frag_color = vec4(base * (shade + point), 1.0);
}
