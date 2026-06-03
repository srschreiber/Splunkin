#include "engine/fx/particles.h"
#include <cmath>

namespace dc::fx {

namespace {
// Deterministic LCG -> float in [0, 1). Keeps the sim testable (no global rand).
float next_unit(uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) * (1.0f / 16777216.0f);   // top 24 bits
}
float next_signed(uint32_t& s) { return next_unit(s) * 2.0f - 1.0f; }  // [-1, 1)
} // namespace

void ParticleSystem::update(float dt, const vec3 flame_pos, float spawn_scale) {
    // Age existing particles; recycle dead ones via swap-and-pop.
    for (std::size_t i = 0; i < particles.size();) {
        Particle& p = particles[i];
        p.age += dt;
        if (p.age >= p.life) {
            particles[i] = particles.back();
            particles.pop_back();
            continue;
        }
        p.pos[0] += p.vel[0] * dt;
        p.pos[1] += p.vel[1] * dt;
        p.pos[2] += p.vel[2] * dt;
        ++i;
    }

    // Spawn new particles at the (flicker-scaled) rate, capped at max_particles.
    if (spawn_scale < 0.0f) spawn_scale = 0.0f;
    spawn_accum += spawn_rate * spawn_scale * dt;
    while (spawn_accum >= 1.0f && particles.size() < max_particles) {
        spawn_accum -= 1.0f;
        Particle p;
        p.pos[0] = flame_pos[0] + next_signed(rng) * spread;
        p.pos[1] = flame_pos[1] + next_signed(rng) * spread;
        p.pos[2] = flame_pos[2] + next_signed(rng) * spread;
        p.vel[0] = next_signed(rng) * drift;
        p.vel[1] = rise_speed * (0.7f + 0.6f * next_unit(rng));
        p.vel[2] = next_signed(rng) * drift;
        p.age = 0.0f;
        p.life = life;
        particles.push_back(p);
    }
    // If nothing can spawn (full / no scale), don't let the debt run away.
    if (spawn_accum > 4.0f) spawn_accum = 4.0f;
}

float flicker(float t) {
    // Two detuned sines -> irregular wobble, remapped to ~[0.7, 1.0].
    float w = 0.5f * (std::sin(t * 11.0f) + std::sin(t * 17.3f + 1.7f));  // [-1, 1]
    return 0.85f + 0.15f * w;
}

void append_billboards(const ParticleSystem& ps, const vec3 right, const vec3 up,
                       float size, std::vector<float>& out) {
    const float hs = size * 0.5f;
    for (const Particle& p : ps.particles) {
        const float a = p.life > 0.0f ? p.age / p.life : 1.0f;
        float alpha = 1.0f - a;                  // fade out with age
        if (alpha < 0.0f) alpha = 0.0f;
        // Warm color shifting orange -> red as it rises/cools.
        const float r = 1.0f;
        const float g = 0.55f - 0.35f * a;
        const float b = 0.15f - 0.10f * a;

        // Four corners of a camera-facing quad around the particle center.
        vec3 rx = { right[0] * hs, right[1] * hs, right[2] * hs };
        vec3 uy = { up[0] * hs,    up[1] * hs,    up[2] * hs };
        vec3 bl = { p.pos[0] - rx[0] - uy[0], p.pos[1] - rx[1] - uy[1], p.pos[2] - rx[2] - uy[2] };
        vec3 br = { p.pos[0] + rx[0] - uy[0], p.pos[1] + rx[1] - uy[1], p.pos[2] + rx[2] - uy[2] };
        vec3 tr = { p.pos[0] + rx[0] + uy[0], p.pos[1] + rx[1] + uy[1], p.pos[2] + rx[2] + uy[2] };
        vec3 tl = { p.pos[0] - rx[0] + uy[0], p.pos[1] - rx[1] + uy[1], p.pos[2] - rx[2] + uy[2] };

        auto push = [&](const vec3 v) {
            out.insert(out.end(), { v[0], v[1], v[2], r, g, b, alpha });
        };
        push(bl); push(br); push(tr);   // triangle 1
        push(bl); push(tr); push(tl);   // triangle 2
    }
}

} // namespace dc::fx
