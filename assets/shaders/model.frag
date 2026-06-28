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
uniform vec3  u_cam_pos;      // camera world position (rim + fog)
uniform vec3  u_fog_color;    // atmospheric horizon color (matches the sky)
uniform vec3  u_sun_dir;      // traversing sun direction (shared with the sky/terrain)
out vec4 frag_color;

void main() {
    vec3 n = normalize(v_normal);
    vec3 V = normalize(u_cam_pos - v_worldpos);
    float a = clamp(u_ambient, 0.0, 1.6);

    // Atmospheric HEMISPHERE ambient: cool sky from above, warm bounce from below.
    vec3 sky  = vec3(0.46, 0.56, 0.68);
    vec3 grnd = vec3(0.34, 0.29, 0.24);
    vec3 ambient = mix(grnd, sky, n.y * 0.5 + 0.5) * (0.42 * a + 0.06);

    // Warm directional SUN with a soft half-Lambert wrap (kinder to low-poly facets).
    vec3 sun_dir = normalize(u_sun_dir);
    vec3 sun_col = vec3(1.18, 1.04, 0.82);
    float ndl = dot(n, sun_dir);
    float wrap = ndl * 0.5 + 0.5; wrap *= wrap;
    vec3 sun = sun_col * wrap * (0.55 * a);

    // Soft Blinn specular sheen on the sunlit side.
    vec3 H = normalize(sun_dir + V);
    float spec = pow(max(dot(n, H), 0.0), 24.0) * max(ndl, 0.0) * 0.22 * a;

    // Subtle sky-tinted FRESNEL rim for that atmospheric edge glow.
    float fres = pow(1.0 - max(dot(n, V), 0.0), 3.0);
    vec3 rim = sky * fres * 0.40 * a;

    vec3 lit = u_color * (ambient + sun) + u_color * rim + vec3(spec);

    // Dynamic point lights (torches, flamethrowers, glowing projectiles).
    for (int i = 0; i < u_light_count; ++i) {
        vec3 toL = u_light_pos[i] - v_worldpos;
        float dist = length(toL);
        float atten = clamp(1.0 - dist / u_light_radius[i], 0.0, 1.0); atten *= atten;
        float p = max(dot(n, normalize(toL)), 0.0);
        lit += u_color * u_light_color[i] * p * atten;
    }
    lit += u_emissive;

    // Distance FOG -> melts into the sky/horizon (the big atmosphere win).
    float d = length(u_cam_pos - v_worldpos);
    float fog = clamp((d - 55.0) / (190.0 - 55.0), 0.0, 1.0) * 0.6;
    lit = mix(lit, u_fog_color, fog);

    frag_color = vec4(lit, u_alpha);
}
