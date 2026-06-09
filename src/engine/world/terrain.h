#pragma once
#include <cstdint>

namespace dc::world {

// Procedural terrain height — a deterministic, closed-form function of world (x,z)
// plus a seed. Pure / GL-free, so host and clients generate identical relief from a
// shared seed with zero replication. The shape is "mostly flat with occasional
// hills": a gentle low-amplitude base everywhere, plus hills that only rise where a
// separate low-frequency mask crosses a threshold (so they're large and sparse, not
// uniform bumpiness). Walls and gameplay live on the tile map; this only shapes the
// open floor's elevation.
struct Terrain {
    uint32_t seed        = 1337u;
    float    base_amp    = 0.6f;    // gentle undulation amplitude (± world units)
    float    base_freq   = 0.07f;   // undulation frequency (cycles per world unit)
    float    hill_amp    = 6.0f;    // extra height at the peak of a full hill
    float    hill_freq   = 0.02f;   // hill-placement frequency (lower = bigger, rarer hills)
    float    hill_thresh = 0.62f;   // mask must exceed this (0..1) before a hill rises

    // Ground height at a world position.
    float height(float x, float z) const;
    // Unit surface normal at (x,z) (finite differences), for flat/smooth shading.
    void  normal(float x, float z, float out[3]) const;
};

} // namespace dc::world
