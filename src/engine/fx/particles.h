#pragma once
#include <cstdint>
#include <vector>
#include <cglm/cglm.h>

namespace dc::fx {

// One live flame fleck. Pure data; simulated on the CPU.
struct Particle {
    vec3  pos = {0.0f, 0.0f, 0.0f};
    vec3  vel = {0.0f, 0.0f, 0.0f};
    float age  = 0.0f;   // seconds alive
    float life = 1.0f;   // seconds until recycled
};

// A small CPU particle emitter for one torch flame. GL-free / testable: it just
// spawns, ages, and recycles particles. Rendering reads `particles` separately.
struct ParticleSystem {
    std::vector<Particle> particles;
    float    spawn_accum = 0.0f;       // fractional particles owed since last spawn
    uint32_t rng = 0x9e3779b9u;        // deterministic LCG state

    // --- tuning ---
    std::size_t max_particles = 48;
    float spawn_rate = 28.0f;          // particles/sec at spawn_scale = 1
    float life       = 0.85f;          // lifetime seconds
    float rise_speed = 1.4f;           // base upward velocity
    float spread     = 0.05f;          // horizontal spawn jitter (world units)
    float drift      = 0.25f;          // horizontal velocity jitter

    // Advance the sim by dt, emitting from `flame_pos`. `spawn_scale` (e.g. the
    // torch flicker) scales the spawn rate so the flame pulses.
    void update(float dt, const vec3 flame_pos, float spawn_scale);
};

// A warm flicker factor in roughly [0.7, 1.0] from time `t` (sum of sines).
// Drives both the light intensity and the particle spawn rate. GL-free.
float flicker(float t);

// Appends camera-facing billboard quads for every live particle to `out`, as
// 6 vertices/particle (two triangles), 7 floats each: pos(x,y,z) + rgba.
// Alpha fades with age; color is warm. `right`/`up` are the camera basis.
void append_billboards(const ParticleSystem& ps, const vec3 right, const vec3 up,
                       float size, std::vector<float>& out);

} // namespace dc::fx
