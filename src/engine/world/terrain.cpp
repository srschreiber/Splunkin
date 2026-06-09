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

float Terrain::height(float x, float z) const {
    // Gentle undulation everywhere, in [-base_amp, base_amp].
    const float base = (fbm(x * base_freq, z * base_freq, seed, 4) - 0.5f) * 2.0f * base_amp;
    // Hills only where a separate low-freq mask clears the threshold, ramped smoothly.
    const float mask = fbm(x * hill_freq, z * hill_freq, seed ^ 0x9e3779b9u, 3);
    float hill = 0.0f;
    if (mask > hill_thresh) {
        const float t = (mask - hill_thresh) / (1.0f - hill_thresh);   // 0..1
        hill = smooth(t) * hill_amp;
    }
    return base + hill;
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

} // namespace dc::world
