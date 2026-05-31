#include "engine/world/collision.h"
#include "engine/world/map.h"
#include <cassert>
#include <cstdio>

using namespace dc::world;

int main() {
    // 3x3, center cell Solid. TILE=2 -> center solid AABB is x[2,4], z[2,4].
    auto m = parse_map(
        "...\n"
        ".#.\n"
        "...\n");
    assert(m.has_value());

    // Circle centered inside the solid cell -> hit.
    assert(circle_hits_solid(*m, 3.0f, 3.0f, 0.4f) == true);

    // Circle in the open cell above (center world (3,1)); nearest solid point is
    // (3,2), distance 1.0 > 0.4 -> no hit.
    assert(circle_hits_solid(*m, 3.0f, 1.0f, 0.4f) == false);

    // Move it close to the boundary: center (3, 1.7), nearest (3,2) dist 0.3 < 0.4 -> hit.
    assert(circle_hits_solid(*m, 3.0f, 1.7f, 0.4f) == true);

    // Far outside the grid -> out-of-bounds cells are Solid -> hit.
    assert(circle_hits_solid(*m, -5.0f, -5.0f, 0.4f) == true);

    std::printf("PASS collision\n");
    return 0;
}
