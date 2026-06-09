#include "engine/world/terrain.h"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace dc::world;

int main() {
    Terrain t;   // default params

    // Deterministic: same query -> same value, and reproducible across instances.
    Terrain t2 = t;
    for (int i = 0; i < 50; ++i) {
        float x = i * 1.3f, z = i * 0.7f;
        assert(t.height(x, z) == t.height(x, z));     // pure
        assert(t.height(x, z) == t2.height(x, z));    // same seed/params -> same world
    }

    // A different seed gives different terrain (at least somewhere).
    Terrain t3 = t; t3.seed = 9999u;
    bool differs = false;
    for (int i = 0; i < 200 && !differs; ++i)
        if (t.height(i * 2.0f, i * 1.1f) != t3.height(i * 2.0f, i * 1.1f)) differs = true;
    assert(differs);

    // Sample a grid: finite + bounded, has real variation, and is MOSTLY flat with
    // only occasional hills.
    const float MAXH = t.base_amp + t.hill_amp + 1.0f;
    float lo = 1e9f, hi = -1e9f;
    int total = 0, elevated = 0, hilly = 0;
    for (int gx = 0; gx < 120; ++gx)
        for (int gz = 0; gz < 120; ++gz) {
            const float h = t.height(gx * 1.5f, gz * 1.5f);
            assert(std::isfinite(h));
            assert(h <= MAXH && h >= -t.base_amp - 1.0f);
            lo = h < lo ? h : lo; hi = h > hi ? h : hi;
            ++total;
            if (h > t.base_amp + 1.0f) ++elevated;   // clearly above the gentle base
            if (h > t.hill_amp * 0.5f) ++hilly;      // up a real hill
        }
    assert(hi - lo > 1.0f);                          // not flat
    assert(hilly > 0);                               // at least one real hill exists
    assert(elevated * 2 < total);                    // but hills are the minority -> mostly flat

    // Normals are unit length and point generally upward.
    for (int i = 0; i < 30; ++i) {
        float n[3]; t.normal(i * 3.0f, i * 2.0f, n);
        const float len = std::sqrt(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
        assert(len > 0.99f && len < 1.01f);
        assert(n[1] > 0.0f);                         // up component positive (it's a heightfield)
    }

    std::printf("PASS terrain\n");
    return 0;
}
