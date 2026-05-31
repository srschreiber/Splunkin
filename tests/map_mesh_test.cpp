#include "engine/world/map_mesh.h"
#include "engine/world/map.h"
#include <cassert>
#include <cstdio>

using namespace dc::world;

int main() {
    // 3x3 with a single Solid cell in the center, surrounded by Open.
    auto m = parse_map(
        "...\n"
        ".#.\n"
        "...\n");
    assert(m.has_value());

    auto verts = build_map_mesh(*m);

    // 8 Open cells -> 8 floor + 8 ceiling quads. Center Solid has 4 open
    // neighbors -> 4 wall quads. Total 20 quads * 6 verts * 9 floats.
    const std::size_t expected = (8 + 8 + 4) * 6 * 9;
    assert(verts.size() == expected);

    // A fully solid map produces no geometry (no open cells, no exposed faces
    // since every solid neighbor is solid; out-of-bounds is solid too).
    auto solid = parse_map("##\n##\n");
    assert(solid.has_value());
    assert(build_map_mesh(*solid).empty());

    // A single Open cell surrounded by out-of-bounds (1x1 ".") -> 1 floor +
    // 1 ceiling, and 0 walls (no solid cells exist). 2 quads.
    auto one = parse_map(".\n");
    assert(one.has_value());
    assert(build_map_mesh(*one).size() == 2 * 6 * 9);

    // Every quad's geometric winding must agree with its stored normal.
    {
        auto mm = parse_map("...\n.#.\n...\n");
        assert(mm.has_value());
        auto vv = build_map_mesh(*mm);
        const int FPV = 9, VPQ = 6;          // floats/vertex, verts/quad
        for (std::size_t q = 0; q + VPQ * FPV <= vv.size(); q += VPQ * FPV) {
            const float* v0 = &vv[q + 0 * FPV];
            const float* v1 = &vv[q + 1 * FPV];
            const float* v2 = &vv[q + 2 * FPV];
            float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
            float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
            float g[3]  = { e1[1]*e2[2]-e1[2]*e2[1],
                            e1[2]*e2[0]-e1[0]*e2[2],
                            e1[0]*e2[1]-e1[1]*e2[0] };
            float dot = g[0]*v0[3] + g[1]*v0[4] + g[2]*v0[5];   // v0[3..5] = normal
            assert(dot > 0.0f);
        }
    }

    std::printf("PASS map_mesh\n");
    return 0;
}
