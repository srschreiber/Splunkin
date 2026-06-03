#include "engine/fx/particles.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace dc::fx;

int main() {
    ParticleSystem ps;
    const vec3 flame = { 1.0f, 2.0f, 3.0f };

    // From empty, stepping forward spawns particles (spawn_rate > 0, scale 1).
    ps.update(0.1f, flame, 1.0f);
    assert(!ps.particles.empty());

    // Spawning never exceeds the cap, even over a long run.
    for (int i = 0; i < 2000; ++i) ps.update(0.016f, flame, 1.0f);
    assert(ps.particles.size() <= ps.max_particles);

    // Particles rise (positive Y velocity) and recycle (cap holds, none immortal).
    for (auto& p : ps.particles) {
        assert(p.vel[1] > 0.0f);
        assert(p.age < p.life);
    }

    // spawn_scale = 0 stops new spawns; existing particles eventually all die.
    for (int i = 0; i < 200; ++i) ps.update(0.016f, flame, 0.0f);
    assert(ps.particles.empty());

    // Billboards: 6 verts/particle * 7 floats, alpha in [0,1].
    ParticleSystem ps2;
    ps2.update(0.05f, flame, 1.0f);
    const std::size_t n = ps2.particles.size();
    const vec3 right = { 1.0f, 0.0f, 0.0f };
    const vec3 up    = { 0.0f, 1.0f, 0.0f };
    std::vector<float> verts;
    append_billboards(ps2, right, up, 0.1f, verts);
    assert(verts.size() == n * 6 * 7);
    for (std::size_t i = 0; i < verts.size(); i += 7) {
        float a = verts[i + 6];
        assert(a >= 0.0f && a <= 1.0f);
    }

    // Flicker stays within a sane band over a range of times.
    for (int i = 0; i < 1000; ++i) {
        float f = flicker(i * 0.013f);
        assert(f > 0.5f && f <= 1.05f);
    }

    std::printf("PASS particles\n");
    return 0;
}
