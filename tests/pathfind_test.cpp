#include "engine/world/pathfind.h"
#include "engine/world/map.h"
#include <cassert>
#include <cstdio>

using namespace dc::world;

int main() {
    // Open 3x3 room: distances are Manhattan from the goal corner.
    auto m = parse_map("...\n...\n...\n");
    assert(m.has_value());
    FlowField f = compute_flow(*m, 0, 0);
    assert(f.at(0, 0) == 0);
    assert(f.at(1, 0) == 1);
    assert(f.at(0, 1) == 1);
    assert(f.at(2, 2) == 4);

    // A walled-off cell is unreachable (-1). Open column between two walls:
    auto m2 = parse_map("#.#\n#.#\n#.#\n");
    FlowField f2 = compute_flow(*m2, 1, 0);
    assert(f2.at(1, 0) == 0);
    assert(f2.at(1, 2) == 2);
    assert(f2.at(0, 0) == -1);   // wall

    // flow_step returns a strictly-closer neighbor; none at the goal.
    uint32_t rng = 1;
    int nc, nr;
    assert(flow_step(f, 2, 2, rng, nc, nr));
    assert(f.at(nc, nr) == f.at(2, 2) - 1);
    assert(!flow_step(f, 0, 0, rng, nc, nr));   // already at goal

    std::printf("PASS pathfind\n");
    return 0;
}
