#include "engine/entity/spawner.h"
#include "engine/world/map.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace dc::entity;
using namespace dc::world;

int main() {
    // 10x5 fully-open room.
    auto m = parse_map("..........\n..........\n..........\n..........\n..........\n");
    assert(m.has_value());

    Spawner sp;
    sp.pos[0] = 5.0f * TILE; sp.pos[2] = 2.5f * TILE;
    sp.radius = 3.0f; sp.rate = 2.0f; sp.max_alive = 100;

    EntityList list;
    for (int i = 0; i < 500; ++i) sp.update(0.01f, list, *m);   // 5s @ 2/s ~= 10
    assert(list.items.size() >= 8 && list.items.size() <= 12);

    // Every spawn is within the radius and on an Open tile.
    for (const auto& e : list.items) {
        const float dx = e.position[0] - sp.pos[0];
        const float dz = e.position[2] - sp.pos[2];
        assert(std::sqrt(dx * dx + dz * dz) <= sp.radius + 1e-3f);
        assert(m->at(static_cast<int>(e.position[0] / TILE),
                     static_cast<int>(e.position[2] / TILE)) == Cell::Open);
    }

    // The alive cap stops further spawns.
    Spawner sp2 = sp; sp2.max_alive = 3; sp2.rng = 7;
    EntityList l2;
    for (int i = 0; i < 500; ++i) sp2.update(0.01f, l2, *m);
    assert(static_cast<int>(l2.items.size()) <= 3);

    std::printf("PASS spawner\n");
    return 0;
}
