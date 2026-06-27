#version 330 core
in vec3 v_normal;
in vec2 v_uv;
in vec3 v_worldpos;
flat in float v_layer;
uniform sampler2DArray u_tex;
#define MAX_LIGHTS 12
uniform int  u_light_count;             // active dynamic lights
uniform vec3 u_light_pos[MAX_LIGHTS];   // world positions (torches, flamethrowers, projectiles)
uniform vec3 u_light_color[MAX_LIGHTS]; // color * intensity
uniform float u_light_radius[MAX_LIGHTS];
uniform int  u_use_solid;     // 1 = solid-color terrain floor, 0 = textured walls
uniform vec3 u_solid;         // terrain base color when u_use_solid
uniform float u_ambient;      // day/night ambient scale (bright by day, dim at night)
uniform vec3  u_cam_pos;      // camera world position (fog distance)
uniform vec3  u_fog_color;    // atmospheric horizon color
uniform float u_time;         // seconds, for animated water
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
    // Grass with natural color variation, dirt patches, and ROCK on the steeper faces.
    vec3 grass1 = u_solid;                              // base mossy green
    vec3 grass2 = u_solid * vec3(1.18, 1.14, 0.82);    // sun-bleached, yellower grass
    vec3 grass3 = u_solid * vec3(0.66, 0.82, 0.66);    // deep shaded green
    vec3 dirt   = vec3(0.40, 0.32, 0.21);              // bare earth patches
    vec3 rock   = vec3(0.40, 0.40, 0.44);              // cool grey stone
    vec3 ramp_brown = vec3(0.58, 0.46, 0.31);          // the gradient up
    vec3 top_brown  = vec3(0.42, 0.30, 0.19);          // plateau tops

    // Several noise scales: big biome patches, mid clumps, fine grain.
    float patch  = fbm2(v_worldpos.xz * 0.045);
    float patch2 = fbm2(v_worldpos.xz * 0.13 + 17.0);
    float grain  = fbm2(v_worldpos.xz * 0.6);
    float elev   = clamp(v_worldpos.y / 12.0, 0.0, 1.0);
    float slope  = clamp(1.0 - n.y, 0.0, 1.0);

    // Grass tone varies organically across the field.
    vec3 grass = mix(grass1, grass2, smoothstep(0.40, 0.72, patch));
    grass = mix(grass, grass3, smoothstep(0.45, 0.78, patch2));
    // Dirt clearings + worn earth.
    vec3 ground = mix(grass, dirt, smoothstep(0.58, 0.82, patch + 0.18 * elev));
    // Bare ROCK takes over on the steeper slopes (with grain so it isn't flat grey).
    ground = mix(ground, rock * (0.82 + 0.32 * grain), smoothstep(0.32, 0.62, slope));

    int mat = int(v_layer + 0.5);
    vec3 col = ground;
    if (mat == 1) col = mix(ramp_brown, rock, 0.45 * slope + 0.18 * grain);  // ramp -> rocky
    if (mat == 2) col = mix(top_brown,  rock, 0.30 + 0.30 * grain);          // top -> stony

    // Fine grain + a soft low-frequency darkening (fake AO in the hollows).
    col *= 0.88 + 0.20 * grain;
    col *= 0.86 + 0.16 * smoothstep(0.2, 0.85, patch2);
    return col;
}

void main() {
    vec3 n = normalize(v_normal);
    float a = clamp(u_ambient, 0.0, 1.6);
    vec3 V = normalize(u_cam_pos - v_worldpos);

    // ---- REFLECTIVE WATER (use_solid == 3): animated ripple normal, sky reflection by
    // fresnel, sharp moving SUN GLINTS, point-light sparkle. ----
    if (u_use_solid == 3) {
        vec2 p = v_worldpos.xz; float t = u_time;
        // Wave normal = sum of several DIRECTIONAL waves (different dir/freq/speed) at two scales
        // + fine noise ripples -> a rich, non-repeating surface (Gerstner-ish slope field).
        vec2 g = vec2(0.0);
        g += vec2( 0.95, 0.22) * 0.090 * sin(dot(p, vec2( 0.30,  0.10)) + t * 1.05);
        g += vec2(-0.30, 0.88) * 0.090 * sin(dot(p, vec2(-0.12,  0.34)) + t * 0.85);
        g += vec2( 0.62,-0.55) * 0.055 * sin(dot(p, vec2( 0.70, -0.50)) + t * 1.70);
        g += vec2( 0.20, 0.72) * 0.055 * sin(dot(p, vec2( 0.22,  0.86)) + t * 1.55);
        g += (vec2(fbm2(p * 0.9 + t * 0.20), fbm2(p * 0.9 - t * 0.20 + 7.0)) - 0.5) * 0.24;
        vec3 wn = normalize(vec3(-g.x, 1.0, -g.y));

        // Reflect the view across the surface and sample a SKY GRADIENT (hazy horizon -> blue zenith).
        vec3 Rv = reflect(-V, wn);
        float up = clamp(Rv.y, 0.0, 1.0);
        vec3 horizon = u_fog_color * 1.08;
        vec3 zenith  = vec3(0.24, 0.42, 0.68);
        vec3 skyref  = mix(horizon, zenith, pow(up, 0.6));

        // Reflected SUN: a sharp disc (the bright glitter streak) + a softer surrounding shimmer.
        vec3 sun_dir = normalize(vec3(0.55, 0.6, 0.55));
        vec3 sun_col = vec3(1.5, 1.36, 1.05);
        float rs = max(dot(Rv, sun_dir), 0.0);
        skyref += sun_col * (pow(rs, 240.0) * 2.2 + pow(rs, 26.0) * 0.22) * a;

        // Schlick FRESNEL (water F0 ~= 0.02): mostly transmissive looking down, mirror at grazing.
        float fres = 0.02 + 0.98 * pow(1.0 - max(dot(wn, V), 0.0), 5.0);
        vec3 deep = u_solid * (0.42 + 0.5 * a);          // absorbed body color
        vec3 col = mix(deep, skyref, clamp(fres, 0.0, 1.0));

        for (int i = 0; i < u_light_count; ++i) {        // torches glitter on the ripples
            vec3 toL = u_light_pos[i] - v_worldpos; float dist = length(toL);
            float atten = clamp(1.0 - dist / u_light_radius[i], 0.0, 1.0); atten *= atten;
            vec3 Hl = normalize(normalize(toL) + V);
            col += u_light_color[i] * (0.20 * max(dot(wn, normalize(toL)), 0.0) + pow(max(dot(wn, Hl), 0.0), 80.0)) * atten;
        }
        float d = length(u_cam_pos - v_worldpos);
        float fog = clamp((d - 55.0) / (190.0 - 55.0), 0.0, 1.0) * 0.62;
        col = mix(col, u_fog_color, fog);
        frag_color = vec4(col, 0.96);
        return;
    }

    // ---- EMISSIVE ORB (use_solid == 4): glowing energy with spherical DEPTH — a hot near-white
    // core, the body color through the middle, and a brighter fresnel CORONA at the rim. ----
    if (u_use_solid == 4) {
        float facing = clamp(dot(n, V), 0.0, 1.0);          // 1 at center, 0 at the silhouette
        vec3 core = u_solid * 1.35 + vec3(0.35);            // hot white-tinted center
        vec3 col  = mix(u_solid, core, pow(facing, 1.6));   // center hotter than the body
        col += u_solid * pow(1.0 - facing, 3.0) * 0.6;      // glowing rim corona
        float d = length(u_cam_pos - v_worldpos);
        float fog = clamp((d - 55.0) / (190.0 - 55.0), 0.0, 1.0) * 0.62;
        frag_color = vec4(mix(col, u_fog_color, fog), 1.0);
        return;
    }

    // Micro-relief: perturb the GROUND normal by the gradient of a fine noise field, so the
    // flat heightfield catches the sun with subtle grassy/rocky bumps instead of reading glassy.
    if (u_use_solid == 1) {
        float e = 0.25, str = 0.55;
        float hL = fbm2((v_worldpos.xz - vec2(e, 0.0)) * 1.0);
        float hR = fbm2((v_worldpos.xz + vec2(e, 0.0)) * 1.0);
        float hD = fbm2((v_worldpos.xz - vec2(0.0, e)) * 1.0);
        float hU = fbm2((v_worldpos.xz + vec2(0.0, e)) * 1.0);
        n = normalize(n + vec3(hL - hR, 0.0, hD - hU) * str * clamp(n.y, 0.0, 1.0));
    }

    // Atmospheric hemisphere ambient (cool sky above, warm earth bounce below) + a warm
    // wrapped directional SUN angled toward the horizon so slopes read on the heightfield.
    vec3 sky  = vec3(0.46, 0.56, 0.68);
    vec3 grnd = vec3(0.36, 0.30, 0.23);
    vec3 ambient = mix(grnd, sky, n.y * 0.5 + 0.5) * (0.40 * a + 0.07);

    vec3 sun_dir = normalize(vec3(0.55, 0.6, 0.55));
    vec3 sun_col = vec3(1.18, 1.06, 0.84);
    float ndl = dot(n, sun_dir);
    float wrap = ndl * 0.5 + 0.5; wrap *= wrap;
    vec3 shade = ambient + sun_col * wrap * (0.55 * a);

    // Sum every dynamic point light (torches, flamethrowers, glowing projectiles).
    vec3 point = vec3(0.0);
    for (int i = 0; i < u_light_count; ++i) {
        vec3 toL = u_light_pos[i] - v_worldpos;
        float dist = length(toL);
        float atten = clamp(1.0 - dist / u_light_radius[i], 0.0, 1.0);
        atten *= atten;
        float nl = max(dot(n, normalize(toL)), 0.0);
        point += u_light_color[i] * nl * atten;
    }

    vec3 base;
    if (u_use_solid == 2)
        base = u_solid;                                            // flat solid color (props: flyers, pillars)
    else if (u_use_solid != 0)
        base = terrain_albedo(n) * (0.96 + 0.02 * v_worldpos.y);   // blended dirt/grass + slight elevation tint
    else
        base = texture(u_tex, vec3(v_uv, v_layer)).rgb;

    vec3 lit = base * (shade + point);

    // Distance FOG -> the ground melts into the horizon haze far down the lane.
    float d = length(u_cam_pos - v_worldpos);
    float fog = clamp((d - 55.0) / (190.0 - 55.0), 0.0, 1.0) * 0.62;
    lit = mix(lit, u_fog_color, fog);

    frag_color = vec4(lit, 1.0);
}
