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

    std::printf("PASS map_mesh\n");
    return 0;
}
