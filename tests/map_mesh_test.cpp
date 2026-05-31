#include "engine/world/map_mesh.h"
#include "engine/world/map.h"
#include <cassert>
#include <cstdio>
#include <cstddef>

using namespace dc::world;

int main() {
    auto m = parse_map(
        "...\n"
        ".#.\n"
        "...\n");
    assert(m.has_value());
    auto verts = build_map_mesh(*m);

    const std::size_t FPV = 9, VPQ = 6, FPQ = VPQ * FPV;
    assert(verts.size() == (8 + 8 + 4) * FPQ);

    auto solid = parse_map("##\n##\n");
    assert(solid.has_value());
    assert(build_map_mesh(*solid).empty());

    auto one = parse_map(".\n");
    assert(one.has_value());
    assert(build_map_mesh(*one).size() == 2 * FPQ);

    bool saw_floor = false, saw_wall = false, saw_ceiling = false;
    for (std::size_t q = 0; q + FPQ <= verts.size(); q += FPQ) {
        const float* v0 = &verts[q + 0 * FPV];
        const float* v1 = &verts[q + 1 * FPV];
        const float* v2 = &verts[q + 2 * FPV];
        float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
        float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
        float g[3]  = { e1[1]*e2[2]-e1[2]*e2[1],
                        e1[2]*e2[0]-e1[0]*e2[2],
                        e1[0]*e2[1]-e1[1]*e2[0] };
        float dot = g[0]*v0[3] + g[1]*v0[4] + g[2]*v0[5];
        assert(dot > 0.0f);

        const float layer = v0[8];
        if (layer == 0.0f)      saw_floor = true;
        else if (layer == 1.0f) saw_wall = true;
        else if (layer == 2.0f) saw_ceiling = true;
        else assert(false);

        for (std::size_t k = 0; k < VPQ; ++k) {
            const float u = verts[q + k*FPV + 6];
            const float vv = verts[q + k*FPV + 7];
            assert(u >= 0.0f && u <= 1.0f);   // u never tiles (catches a u/v swap)
            assert(vv >= 0.0f && vv <= 1.5f);
        }
    }
    assert(saw_floor && saw_wall && saw_ceiling);

    std::printf("PASS map_mesh\n");
    return 0;
}
