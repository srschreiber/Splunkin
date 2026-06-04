#include "engine/entity/enemy.h"
#include "engine/world/pathfind.h"
#include "engine/world/map.h"
#include <cassert>
#include <cstdio>

using namespace dc::entity;
using namespace dc::world;

int main() {
    // A 10x5 open room.
    auto m = parse_map("..........\n..........\n..........\n..........\n..........\n");
    assert(m.has_value());

    // Player at tile (7,2); enemy starts far away at tile (1,2).
    PlayerCombat pc{};
    pc.pos[0] = (7 + 0.5f) * TILE; pc.pos[1] = 0.0f; pc.pos[2] = (2 + 0.5f) * TILE;
    pc.yaw = 0.0f; pc.blocking = false; pc.strike = false;
    pc.strike_reach = 1.8f; pc.strike_cos = 0.4f; pc.strike_damage = 12.0f;
    pc.strike_knockback = 10.0f; pc.weight = 5.0f;
    const int pcol = static_cast<int>(pc.pos[0] / TILE);
    const int prow = static_cast<int>(pc.pos[2] / TILE);

    EntityList list;
    list.spawn_enemy((1 + 0.5f) * TILE, (2 + 0.5f) * TILE);
    const float start_x = list.items[0].position[0];

    // It should pathfind toward the player and eventually attack (deal damage).
    float total = 0.0f;
    for (int i = 0; i < 3000 && !list.items.empty(); ++i) {
        FlowField f = compute_flow(*m, pcol, prow);
        total += update_enemies(list, *m, f, pc, 0.016f).damage;
    }
    assert(!list.items.empty());
    assert(list.items[0].position[0] > start_x + 5.0f);   // moved toward the player
    assert(total > 0.0f);                                  // reached and hit the player

    // Blocking soaks most of the damage.
    EntityList l2;
    l2.spawn_enemy(pc.pos[0] + 0.5f, pc.pos[2]);   // right next to the player
    PlayerCombat blk = pc; blk.blocking = true;
    float blocked = 0.0f;
    for (int i = 0; i < 200; ++i) {
        FlowField f = compute_flow(*m, pcol, prow);
        blocked += update_enemies(l2, *m, f, blk, 0.016f).damage;
    }
    assert(blocked > 0.0f && blocked < total);             // mitigated vs. unblocked run

    // A player strike knocks the enemy AWAY from the player (+x here).
    EntityList lk;
    lk.spawn_enemy(pc.pos[0] + 1.5f, pc.pos[2]);
    const float kx0 = lk.items[0].position[0];
    {
        FlowField f = compute_flow(*m, pcol, prow);
        PlayerCombat st = pc; st.strike = true;
        update_enemies(lk, *m, f, st, 0.016f);             // strike imparts knock_vel
        assert(!lk.items.empty());
        assert(lk.items[0].knock_vel[0] > 0.0f);           // pushed in +x (away from player)
        for (int i = 0; i < 30; ++i) update_enemies(lk, *m, f, pc, 0.016f);  // let it slide
        assert(lk.items[0].position[0] > kx0);             // actually moved away
    }

    // A player strike in front kills an enemy in a few hits.
    EntityList l3;
    l3.spawn_enemy(pc.pos[0] + 1.5f, pc.pos[2]);   // 1.5 ahead, +x = player's forward
    l3.items[0].stats.weight = 100.0f;             // too heavy to knock away (isolate the kill)
    int hits = 0;
    while (!l3.items.empty() && hits < 10) {
        FlowField f = compute_flow(*m, pcol, prow);
        PlayerCombat st = pc; st.strike = true;
        update_enemies(l3, *m, f, st, 0.016f);
        ++hits;
    }
    assert(l3.items.empty());
    assert(hits <= 4);   // 30 hp / 12 dmg -> 3 hits

    std::printf("PASS combat\n");
    return 0;
}
