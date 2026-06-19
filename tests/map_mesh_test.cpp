#include "engine/world/map_mesh.h"
#include "engine/world/map.h"
#include "engine/world/terrain.h"
#include <cassert>
#include <cstdio>
#include <cstddef>

using namespace dc::world;

int main() {
    Terrain flat; flat.base_amp = 0.0f; flat.hill_amp = 0.0f; flat.mound_amp = 0.0f;   // height()==0 -> flat ground/walls

    auto m = parse_map(
        "...\n"
        ".#.\n"
        "...\n");
    assert(m.has_value());
    auto walls = build_map_mesh(*m, flat);

    const std::size_t FPV = 9, VPQ = 6, FPQ = VPQ * FPV, TRI = 3 * FPV;
    // Only the single Solid cell (1,1); it borders 4 Open cells -> 4 wall faces.
    // (Floor is the terrain mesh now; no ceiling -> open top.)
    assert(walls.size() == 4 * FPQ);

    // Every wall quad: layer 1 (textured), consistent outward winding.
    for (std::size_t q = 0; q + FPQ <= walls.size(); q += FPQ) {
        const float* v0 = &walls[q]; const float* v1 = &walls[q + FPV]; const float* v2 = &walls[q + 2 * FPV];
        const float e1[3] = { v1[0]-v0[0], v1[1]-v0[1], v1[2]-v0[2] };
        const float e2[3] = { v2[0]-v0[0], v2[1]-v0[1], v2[2]-v0[2] };
        const float g[3]  = { e1[1]*e2[2]-e1[2]*e2[1], e1[2]*e2[0]-e1[0]*e2[2], e1[0]*e2[1]-e1[1]*e2[0] };
        assert(g[0]*v0[3] + g[1]*v0[4] + g[2]*v0[5] > 0.0f);   // stored normal agrees with winding
        assert(v0[8] == 1.0f);                                  // LAYER_WALL
    }

    // All-solid: no Open neighbours -> no walls.
    auto solid = parse_map("##\n##\n");
    assert(solid.has_value());
    assert(build_map_mesh(*solid, flat).empty());

    // A single Open cell: no Solid cells -> no walls, but a terrain floor patch.
    auto one = parse_map(".\n");
    assert(one.has_value());
    assert(build_map_mesh(*one, flat).empty());

    auto floor = build_terrain_mesh(*one, flat);
    assert(!floor.empty());
    assert(floor.size() % TRI == 0);                 // whole triangles
    for (std::size_t t = 0; t + TRI <= floor.size(); t += TRI)
        assert(floor[t + 4] > 0.9f);                 // flat ground -> facets face straight up (+Y)

    // Hills tilt the floor: a non-flat terrain produces some non-vertical facets.
    Terrain hilly; hilly.base_amp = 2.0f; hilly.base_freq = 0.25f;   // strong relief over the patch
    auto floor2 = build_terrain_mesh(*one, hilly);
    bool tilted = false;
    for (std::size_t t = 0; t + TRI <= floor2.size(); t += TRI)
        if (floor2[t + 4] < 0.999f) { tilted = true; break; }
    assert(tilted);

    std::printf("PASS map_mesh\n");
    return 0;
}
