#include "engine/world/terrain.h"
#include <cmath>

namespace dc::world {

namespace {
// Hash an integer lattice point to [0,1). Cheap, deterministic, decorrelated.
float hash01(int ix, int iz, uint32_t seed) {
    uint32_t h = seed + static_cast<uint32_t>(ix) * 0x8da6b343u
                      + static_cast<uint32_t>(iz) * 0xd8163841u;
    h ^= h >> 13; h *= 0x2545f491u; h ^= h >> 15;
    return (h & 0xFFFFFFu) * (1.0f / 16777216.0f);
}
float smooth(float t) { return t * t * (3.0f - 2.0f * t); }   // smoothstep

// Value noise: bilinear blend of the 4 surrounding lattice hashes, smoothstepped.
float vnoise(float x, float z, uint32_t seed) {
    const int x0 = static_cast<int>(std::floor(x)), z0 = static_cast<int>(std::floor(z));
    const float fx = x - x0, fz = z - z0;
    const float a = hash01(x0, z0, seed),     b = hash01(x0 + 1, z0, seed);
    const float c = hash01(x0, z0 + 1, seed), d = hash01(x0 + 1, z0 + 1, seed);
    const float u = smooth(fx), v = smooth(fz);
    const float ab = a + (b - a) * u, cd = c + (d - c) * u;
    return ab + (cd - ab) * v;   // [0,1)
}

// Fractal sum of octaves -> [0,1), concentrated around 0.5.
float fbm(float x, float z, uint32_t seed, int octaves) {
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum  += amp * vnoise(x * freq, z * freq, seed + static_cast<uint32_t>(i) * 101u);
        norm += amp; amp *= 0.5f; freq *= 2.0f;
    }
    return sum / norm;
}
} // namespace

float Terrain::height_ground(float x, float z) const {
    // Gentle undulation everywhere, in [-base_amp, base_amp].
    const float base = (fbm(x * base_freq, z * base_freq, seed, 4) - 0.5f) * 2.0f * base_amp;
    // Hills only where a separate low-freq mask clears the threshold, ramped smoothly.
    const float mask = fbm(x * hill_freq, z * hill_freq, seed ^ 0x9e3779b9u, 3);
    float hill = 0.0f;
    if (mask > hill_thresh) {
        const float t = (mask - hill_thresh) / (1.0f - hill_thresh);   // 0..1
        hill = smooth(t) * hill_amp;
    }
    // A second, more frequent mask scatters smaller raised mounds (own seed salt so
    // they're decorrelated from the big hills).
    const float mmask = fbm(x * mound_freq, z * mound_freq, seed ^ 0x85ebca6bu, 3);
    float mound = 0.0f;
    if (mmask > mound_thresh) {
        const float t = (mmask - mound_thresh) / (1.0f - mound_thresh);
        mound = smooth(t) * mound_amp;
    }
    return base + hill + mound;
}

float Terrain::height(float x, float z) const {
    float ground = height_ground(x, z);

    // Placed plateaus: a flat top (rect) at `top`, a gentle ramp up one edge, and a
    // steep-but-continuous cliff skirt around the rest of the perimeter (an aggressive
    // drop-off, not a vertical wall — roughed up with noise so it reads as natural rock).
    // The plateau OVERRIDES the noise where it's higher. Take the tallest contributor.
    const float CLIFF_RUN = 5.0f;   // horizontal run of the cliff face (shorter = steeper)
    float pl = 0.0f;
    for (const Plateau& p : plateaus) {
        if (x >= p.x0 && x <= p.x1 && z >= p.z0 && z <= p.z1) {        // on the (near-)flat top
            // Subtle rolling so the top isn't a dead-flat angular slab. Fades out near the
            // rim (within `edge` units) so the sheer cliff edge stays crisp.
            const float dedge = std::fmin(std::fmin(x - p.x0, p.x1 - x), std::fmin(z - p.z0, p.z1 - z));
            const float edge  = 4.0f;
            const float fade  = dedge < edge ? smooth(dedge / edge) : 1.0f;
            const float roll  = (fbm(x * 0.18f, z * 0.18f, seed ^ 0x5A5Au, 3) - 0.5f) * 2.0f * 1.2f;
            const float th = p.top + roll * fade;
            if (th > pl) pl = th;
            continue;
        }
        // The ramp is a NARROW path (|coord - ramp_center| <= ramp_half) carved into one
        // face — like a stair cut in a cliff — not the whole edge. Everything else on that
        // edge stays sheer cliff.
        float t = -1.0f;   // ramp progress (0 at the foot, 1 at the plateau edge)
        switch (p.ramp_side) {
            case 0: if (std::fabs(z - p.ramp_center) <= p.ramp_half && x > p.x1 && x <= p.x1 + p.ramp_len) t = 1.0f - (x - p.x1) / p.ramp_len; break;
            case 1: if (std::fabs(z - p.ramp_center) <= p.ramp_half && x < p.x0 && x >= p.x0 - p.ramp_len) t = 1.0f - (p.x0 - x) / p.ramp_len; break;
            case 2: if (std::fabs(x - p.ramp_center) <= p.ramp_half && z > p.z1 && z <= p.z1 + p.ramp_len) t = 1.0f - (z - p.z1) / p.ramp_len; break;
            default:if (std::fabs(x - p.ramp_center) <= p.ramp_half && z < p.z0 && z >= p.z0 - p.ramp_len) t = 1.0f - (p.z0 - z) / p.ramp_len; break;
        }
        if (t > 0.0f) {
            float rh = t * p.top;                       // perfect gradient first...
            // ...then dirty it up: a second pass of high-freq noise makes the ramp
            // rough/bumpy instead of a clean plane. A window that's 0 at both ends keeps
            // the joins clean (foot meets ground, head meets the flat top edge).
            const float win  = 4.0f * t * (1.0f - t);   // 0 at foot & top, 1 mid-ramp
            const float bump = (fbm(x * 0.5f, z * 0.5f, seed ^ 0xC0FFEEu, 3) - 0.5f) * 2.0f;
            rh += bump * 0.32f * win;                    // gentle roughness mid-ramp (keep it walkable)
            if (rh > pl) pl = rh;
        }

        // Cliff skirt: steep drop from the rim down to ground over CLIFF_RUN. Distance to
        // the rect (0 inside, handled above). A sqrt falloff makes it steepest right at the
        // rim and ease out near the base, like a real talus slope. The ramp out-climbs this
        // on its own edge (it's far higher at the same distance), so the path stays open.
        const float ox = std::fmax(std::fmax(p.x0 - x, x - p.x1), 0.0f);
        const float oz = std::fmax(std::fmax(p.z0 - z, z - p.z1), 0.0f);
        const float od = std::sqrt(ox * ox + oz * oz);
        if (od > 1e-4f && od < CLIFF_RUN) {
            const float ct = od / CLIFF_RUN;              // 0 at rim, 1 at the foot
            float ch = ground + (p.top - ground) * (1.0f - std::sqrt(ct));
            const float win = 4.0f * ct * (1.0f - ct);   // 0 at rim & foot, 1 mid-face
            ch += (fbm(x * 0.4f, z * 0.4f, seed ^ 0xC11FF5u, 3) - 0.5f) * 2.0f * 1.6f * win;  // rocky dirtiness
            if (ch > pl) pl = ch;
        }
    }
    return pl > ground ? pl : ground;
}

int Terrain::surface_kind(float x, float z) const {
    // Match height(): a plateau surface only "wins" where it sits above the noise ground.
    const float ground = height_ground(x, z);
    int   kind = 0;
    float best = ground;
    for (const Plateau& p : plateaus) {
        if (x >= p.x0 && x <= p.x1 && z >= p.z0 && z <= p.z1) {        // flat top
            if (p.top > best) { best = p.top; kind = 2; }
            continue;
        }
        float t = -1.0f;
        switch (p.ramp_side) {
            case 0: if (std::fabs(z - p.ramp_center) <= p.ramp_half && x > p.x1 && x <= p.x1 + p.ramp_len) t = 1.0f - (x - p.x1) / p.ramp_len; break;
            case 1: if (std::fabs(z - p.ramp_center) <= p.ramp_half && x < p.x0 && x >= p.x0 - p.ramp_len) t = 1.0f - (p.x0 - x) / p.ramp_len; break;
            case 2: if (std::fabs(x - p.ramp_center) <= p.ramp_half && z > p.z1 && z <= p.z1 + p.ramp_len) t = 1.0f - (z - p.z1) / p.ramp_len; break;
            default:if (std::fabs(x - p.ramp_center) <= p.ramp_half && z < p.z0 && z >= p.z0 - p.ramp_len) t = 1.0f - (p.z0 - z) / p.ramp_len; break;
        }
        if (t > 0.0f) { const float rh = t * p.top; if (rh > best) { best = rh; kind = 1; } }
    }
    return kind;
}

void Terrain::normal(float x, float z, float out[3]) const {
    const float e = 0.5f;   // sample step for the gradient
    const float hl = height(x - e, z), hr = height(x + e, z);
    const float hd = height(x, z - e), hu = height(x, z + e);
    // Surface gradient -> normal = (-dH/dx, 1, -dH/dz), then normalized.
    float nx = (hl - hr), ny = 2.0f * e, nz = (hd - hu);
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    const float inv = len > 1e-6f ? 1.0f / len : 0.0f;
    out[0] = nx * inv; out[1] = ny * inv; out[2] = nz * inv;
}

void Terrain::place_plateaus(int count, float worldW, float worldH, float spawnX, float spawnZ) {
    plateaus.clear();
    uint32_t s = seed ^ 0x91A7EA0u;
    auto nf = [&](float lo, float hi) {
        s = s * 1664525u + 1013904223u;
        return lo + (hi - lo) * ((s >> 8) * (1.0f / 16777216.0f));
    };
    const float margin = 28.0f;          // keep tops + ramps off the border walls
    const float spacing = 70.0f;         // min gap between plateaus (keep them well apart)
    for (int i = 0; i < count; ++i) {
        for (int tries = 0; tries < 40; ++tries) {
            const float hx = nf(18.0f, 32.0f), hz = nf(18.0f, 32.0f);   // big footprints
            const float cx = nf(margin + hx, worldW - margin - hx);
            const float cz = nf(margin + hz, worldH - margin - hz);
            // Keep clear of spawn.
            if (std::fabs(cx - spawnX) < hx + 24.0f && std::fabs(cz - spawnZ) < hz + 24.0f) continue;
            // Don't place plateaus close together.
            bool overlap = false;
            for (const Plateau& q : plateaus)
                if (cx + hx > q.x0 - spacing && cx - hx < q.x1 + spacing &&
                    cz + hz > q.z0 - spacing && cz - hz < q.z1 + spacing) { overlap = true; break; }
            if (overlap) continue;
            Plateau p;
            p.x0 = cx - hx; p.x1 = cx + hx; p.z0 = cz - hz; p.z1 = cz + hz;
            p.top = nf(9.0f, 16.0f);
            // Ramp faces the map interior (toward center) so its foot lands on open floor.
            const float dx = worldW * 0.5f - cx, dz = worldH * 0.5f - cz;
            p.ramp_side = (std::fabs(dx) > std::fabs(dz)) ? (dx > 0 ? 0 : 1) : (dz > 0 ? 2 : 3);
            p.ramp_len = p.top * nf(1.9f, 2.5f);   // a little steeper than before (run >> rise, but less so)
            p.ramp_half = 3.0f;                    // a ~6-wide path (narrow stair, not the whole face)
            // Center the path somewhere along its edge (kept clear of the corners).
            if (p.ramp_side <= 1) p.ramp_center = nf(p.z0 + p.ramp_half, p.z1 - p.ramp_half);
            else                  p.ramp_center = nf(p.x0 + p.ramp_half, p.x1 - p.ramp_half);
            plateaus.push_back(p);
            break;
        }
    }
}

} // namespace dc::world
