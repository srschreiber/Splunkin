#pragma once
#include <cstdint>
#include <vector>

namespace dc::world {

// A placed plateau: a flat raised top (rect) with SHEER cliff edges, plus one ramp
// extending from a chosen edge that gradients up from ground to the top — the only
// walkable way up. Deterministic (built from the terrain seed), GL-free.
struct Plateau {
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;   // flat-top rectangle in world space
    float top = 0.0f;                        // plateau height (above ground level)
    int   ramp_side = 0;                     // 0:+x 1:-x 2:+z 3:-z (edge the ramp extends from)
    float ramp_len = 0.0f;                   // ramp run length (longer = gentler)
    float ramp_center = 0.0f;                // path center along its edge (z for sides 0/1, x for 2/3)
    float ramp_half = 3.0f;                  // path half-width — a narrow stair, not the whole face
};

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
    float    hill_amp    = 6.0f;    // gentle rolling hills (placed plateaus are the dramatic relief now)
    float    hill_freq   = 0.018f;  // hill-placement frequency (lower = bigger, rarer hills)
    float    hill_thresh = 0.58f;   // mask must exceed this (0..1) before a hill rises
    // A second, more frequent layer of smaller raised patches ("mounds") on top of the
    // base + big hills, so the ground has scattered sections that sit a bit higher.
    float    mound_amp    = 2.4f;   // extra height at a mound peak
    float    mound_freq   = 0.05f;  // mound-placement frequency (higher = smaller, more of them)
    float    mound_thresh = 0.60f;  // mask threshold before a mound rises (keeps them scattered)

    std::vector<Plateau> plateaus;   // placed sheer-cliff plateaus (override the noise where higher)

    // Optional RIVER carve: erodes a channel into the ground so the lane's river sits in a
    // LOWERED bed with sloping banks (the ground drops off into the water). set_river() turns
    // it on; carve = 0 disables. Mirrors the river's centerline/width so water + bed align.
    bool  river_on    = false;
    float river_z     = 0.0f;   // base centerline Z
    float river_amp   = 0.0f;   // winding amplitude
    float river_windf = 1.6f;   // winding frequency along the lane
    float river_x0 = 0.0f, river_x1 = 0.0f;   // lane span the river runs across
    float river_half  = 3.2f;   // water half-width at the narrow stretch
    float river_basin = 10.0f;  // extra half-width in the mid-lane basin
    float river_carve = 0.0f;   // max depth carved at the channel center
    float river_bank  = 4.5f;   // how far past the water the banks slope up
    void  set_river(float z, float amp, float windf, float x0, float x1,
                    float half, float basin, float carve, float bank);
    float river_carve_at(float x, float z) const;   // depth (>=0) to drop the ground here

    // Noise ground height (base + hills + mounds), WITHOUT any placed plateaus.
    float height_ground(float x, float z) const;
    // Ground height at a world position (noise ground, raised by any plateau on top).
    float height(float x, float z) const;
    // Which surface (x,z) sits on, for coloring: 0 = open ground, 1 = a plateau ramp,
    // 2 = a plateau top. (The tallest contributor wins, same as height().)
    int   surface_kind(float x, float z) const;
    // Unit surface normal at (x,z) (finite differences), for flat/smooth shading.
    void  normal(float x, float z, float out[3]) const;
    // Second map-gen pass: scatter `count` plateaus inside [0,worldW]x[0,worldH], away
    // from (spawnX,spawnZ), each with a ramp facing the map interior. Deterministic.
    void  place_plateaus(int count, float worldW, float worldH, float spawnX, float spawnZ);
};

} // namespace dc::world
