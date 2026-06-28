#version 330 core
// Procedural SKY: a horizon->zenith gradient that shifts with day/night, a bright SUN disc +
// glow by day with drifting CLOUDS, and a MOON + twinkling STARS by night. Reconstructs the
// world-space view ray per pixel from the inverse view-projection.
in vec2 v_ndc;
uniform mat4  u_inv_viewproj;
uniform vec3  u_cam_pos;
uniform float u_time;
uniform float u_ambient;   // day/night (low = night)
uniform vec3  u_fog;       // matches begin_frame's horizon haze
uniform vec3  u_sun_dir;   // traversing sun (above horizon by day)
uniform vec3  u_moon_dir;  // traversing moon (above horizon by night)
out vec4 frag_color;

float hash21(vec2 p){ p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float vnoise(vec2 p){
    vec2 i = floor(p), f = fract(p); vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash21(i), b = hash21(i + vec2(1,0)), c = hash21(i + vec2(0,1)), d = hash21(i + vec2(1,1));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
float fbm(vec2 p){ float s = 0.0, a = 0.5; for (int i = 0; i < 5; ++i){ s += a * vnoise(p); p *= 2.03; a *= 0.5; } return s; }

void main() {
    // World-space view ray from the NDC pixel.
    vec4 nh = u_inv_viewproj * vec4(v_ndc, -1.0, 1.0);
    vec4 fh = u_inv_viewproj * vec4(v_ndc,  1.0, 1.0);
    vec3 dir = normalize(fh.xyz / fh.w - nh.xyz / nh.w);
    float up = clamp(dir.y, 0.0, 1.0);

    // Day/night is driven by the SUN'S ELEVATION (it traverses the sky on the day clock), so the
    // sky goes properly dark + starry once the sun sets — independent of the gameplay ambient.
    float day = smoothstep(-0.04, 0.22, u_sun_dir.y);   // 1 = sun well up, 0 = sun below horizon
    float night = 1.0 - day;

    // --- sky gradient (horizon -> zenith), tinted by day/night ---
    vec3 dayHor = mix(u_fog, vec3(0.66, 0.78, 0.95), 0.5);
    vec3 dayZen = vec3(0.17, 0.40, 0.78);
    vec3 ntHor  = vec3(0.04, 0.06, 0.14);
    vec3 ntZen  = vec3(0.010, 0.020, 0.075);
    vec3 horizon = mix(ntHor, dayHor, day);
    vec3 zenith  = mix(ntZen, dayZen, day);
    vec3 col = mix(horizon, zenith, pow(up, 0.55));

    // --- SUN (day): a crisp disc + warm glow at its current sky position ---
    vec3 sunDir = normalize(u_sun_dir);
    float sd = max(dot(dir, sunDir), 0.0);
    col += day * vec3(1.00, 0.93, 0.75) * (pow(sd, 900.0) * 3.2 + pow(sd, 18.0) * 0.22);

    // --- MOON (night): a pale disc with subtle maria + a soft halo at its current position ---
    vec3 moonDir = normalize(u_moon_dir);
    float md = max(dot(dir, moonDir), 0.0);
    float moonDisc = smoothstep(0.9986, 0.9994, md);
    float maria = 0.82 + 0.18 * fbm(dir.xz * 55.0 + 3.0);
    col += night * vec3(0.86, 0.88, 0.96) * moonDisc * maria;
    col += night * vec3(0.55, 0.60, 0.78) * pow(md, 120.0) * 0.30;

    // --- STARS (night): sparse twinkling points, denser higher up ---
    if (dir.y > 0.04) {
        vec2 sc = dir.xz / (dir.y + 0.25);
        vec2 cell = floor(sc * 90.0);
        float n = hash21(cell);
        float n2 = hash21(cell + 7.3);
        float rate = 0.6 + n2 * 3.5;                          // each star twinkles at its OWN rate
        float twinkle = 0.84 + 0.16 * sin(u_time * rate + n * 120.0);   // subtle shimmer
        float star = smoothstep(0.9955, 0.9995, n) * twinkle;
        col += night * star * vec3(0.92, 0.95, 1.0) * smoothstep(0.04, 0.35, dir.y) * (1.0 - moonDisc);
    }

    // --- CLOUDS (day): drifting fbm sheet from the horizon up into the mid-sky ---
    if (dir.y > 0.015) {
        vec2 cp = dir.xz / (dir.y + 0.16) * 1.1 + vec2(u_time * 0.010, u_time * 0.006);
        float c = fbm(cp);
        c = smoothstep(0.52, 0.92, c) * smoothstep(0.0, 0.22, dir.y) * (1.0 - up * 0.45);
        vec3 cloudCol = mix(vec3(0.80, 0.84, 0.92), vec3(1.0, 0.99, 0.97), up);   // grey base, bright tops
        col = mix(col, cloudCol, day * c * 0.85);
    }

    frag_color = vec4(col, 1.0);
}
