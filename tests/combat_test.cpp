#include "engine/entity/enemy.h"
#include "engine/world/pathfind.h"
#include "engine/world/map.h"
#include <cassert>
#include <cstdio>
#include <vector>

using namespace dc::entity;
using namespace dc::world;

int main() {
    // A 10x5 open room.
    auto m = parse_map("..........\n..........\n..........\n..........\n..........\n");
    assert(m.has_value());

    // Single-player convenience wrapper over the multi-player update_enemies.
    auto step1 = [&](EntityList& l, const FlowField& f, const PlayerCombat& p, float dt,
                     std::vector<float>* deaths = nullptr) -> EnemyHitPlayer {
        std::vector<PlayerCombat> ps{ p };
        std::vector<FlowField> flows{ f };   // one field, for the single player
        std::vector<EnemyHitPlayer> out;
        update_enemies(l, *m, flows, ps, out, dt, deaths);
        return out.empty() ? EnemyHitPlayer{} : out[0];
    };

    // Player at tile (7,2); enemy starts far away at tile (1,2).
    PlayerCombat pc{};
    pc.pos[0] = (7 + 0.5f) * TILE; pc.pos[1] = 0.0f; pc.pos[2] = (2 + 0.5f) * TILE;
    pc.yaw = 0.0f; pc.blocking = false; pc.strike = false;
    pc.strike_reach = 1.8f; pc.strike_cos = 0.4f; pc.strike_damage = 12.0f;
    pc.strike_knockback = 10.0f; pc.weight = 5.0f;
    pc.block_cos = 0.3f; pc.block_rate = 0.0f; pc.stamina = 0.0f;
    const int pcol = static_cast<int>(pc.pos[0] / TILE);
    const int prow = static_cast<int>(pc.pos[2] / TILE);

    EntityList list;
    list.spawn_enemy((1 + 0.5f) * TILE, (2 + 0.5f) * TILE);
    const float start_x = list.items[0].position[0];

    // It should pathfind toward the player and eventually attack (deal damage).
    float total = 0.0f;
    for (int i = 0; i < 3000 && !list.items.empty(); ++i) {
        FlowField f = compute_flow(*m, pcol, prow);
        total += step1(list, f, pc, 0.016f).damage;
    }
    assert(!list.items.empty());
    assert(list.items[0].position[0] > start_x + 5.0f);   // moved toward the player
    assert(total > 0.0f);                                  // reached and hit the player

    // Blocking with ample stamina fully negates frontal damage (at a stamina cost).
    EntityList l2;
    l2.spawn_enemy(pc.pos[0] + 0.5f, pc.pos[2]);   // right next to the player
    l2.items[0].stats.weight = 100.0f;             // don't get knocked away
    PlayerCombat blk = pc; blk.blocking = true; blk.block_cos = 0.5f; blk.block_rate = 0.5f; blk.stamina = 1000.0f;
    float blocked_dmg = 0.0f, sta_spent = 0.0f;
    for (int i = 0; i < 200; ++i) {
        FlowField f = compute_flow(*m, pcol, prow);
        EnemyHitPlayer h = step1(l2, f, blk, 0.016f);
        blocked_dmg += h.damage; sta_spent += h.stamina_cost;
    }
    assert(blocked_dmg == 0.0f);                            // fully negated (plenty of stamina)
    assert(sta_spent > 0.0f);                               // and it cost stamina to do so

    // Out of stamina: the unaffordable remainder lands. 8 dmg, rate 1, only 4 stamina
    // -> blocks 4, takes 4 (the example math: shortfall 4 / rate 1 = 4 through).
    EntityList lpb;
    lpb.spawn_enemy(pc.pos[0] + 0.5f, pc.pos[2]);
    lpb.items[0].stats.weight = 100.0f;
    PlayerCombat pbk = pc; pbk.blocking = true; pbk.block_cos = 0.5f; pbk.block_rate = 1.0f; pbk.stamina = 4.0f;
    float per_hit = -1.0f;
    for (int i = 0; i < 300 && per_hit < 0.0f; ++i) {
        FlowField f = compute_flow(*m, pcol, prow);
        EnemyHitPlayer h = step1(lpb, f, pbk, 0.016f);
        if (h.damage > 0.0f) per_hit = h.damage;
    }
    assert(per_hit > 3.9f && per_hit < 4.1f);              // 8 - (4 stamina / rate 1) = 4 gets through

    // A player strike knocks the enemy AWAY from the player (+x here).
    EntityList lk;
    lk.spawn_enemy(pc.pos[0] + 1.5f, pc.pos[2]);
    const float kx0 = lk.items[0].position[0];
    {
        FlowField f = compute_flow(*m, pcol, prow);
        PlayerCombat st = pc; st.strike = true;
        step1(lk, f, st, 0.016f);             // strike imparts knock_vel
        assert(!lk.items.empty());
        assert(lk.items[0].knock_vel[0] > 0.0f);           // pushed in +x (away from player)
        for (int i = 0; i < 30; ++i) step1(lk, f, pc, 0.016f);  // let it slide
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
        step1(l3, f, st, 0.016f);
        ++hits;
    }
    assert(l3.items.empty());
    assert(hits <= 4);   // 30 hp / 12 dmg -> 3 hits

    // Committed facing: leaving the enemy's cone/reach during its wind-up whiffs.
    EntityList ld;
    ld.spawn_enemy(pc.pos[0] + 1.0f, pc.pos[2]);   // in range -> will start a swing
    for (int i = 0; i < 200 && !ld.items[0].attacking; ++i) {
        FlowField f = compute_flow(*m, pcol, prow);
        step1(ld, f, pc, 0.016f);     // player stays put until it commits
    }
    assert(!ld.items.empty() && ld.items[0].attacking);   // swing committed
    PlayerCombat moved = pc; moved.pos[2] += 5.0f;         // sidestep far before the strike
    float whiff = 0.0f;
    for (int i = 0; i < 60; ++i) {
        FlowField f = compute_flow(*m, pcol, prow);
        whiff += step1(ld, f, moved, 0.016f).damage;
    }
    assert(whiff == 0.0f);   // the committed swing missed

    // radius_attack: hits enemies in range once per pass (the thrown sword uses this).
    EntityList lr;
    lr.spawn_enemy(10.0f, 10.0f);   // at the center
    lr.spawn_enemy(20.0f, 20.0f);   // far away
    vec3 center = { 10.0f, 0.0f, 10.0f };
    std::vector<uint32_t> rhit;
    radius_attack(lr, center, 1.5f, 5.0f, 5.0f, rhit);          // non-lethal
    assert(rhit.size() == 1);                                   // only the near one
    assert(lr.items[0].alive && lr.items[0].health < ENEMY_MAX_HEALTH);
    float h = lr.items[0].health;
    radius_attack(lr, center, 1.5f, 5.0f, 5.0f, rhit);          // already hit -> skipped
    assert(lr.items[0].health == h);

    // update_enemies reports death positions (so the caller can drop coins).
    EntityList ld2;
    ld2.spawn_enemy(pc.pos[0] + 1.0f, pc.pos[2]);
    ld2.items[0].stats.weight = 100.0f;     // don't knock it out of reach
    ld2.items[0].health = 1.0f;             // dies on the next strike
    std::vector<float> deaths;
    {
        FlowField f = compute_flow(*m, pcol, prow);
        PlayerCombat st = pc; st.strike = true;
        step1(ld2, f, st, 0.016f, &deaths);
    }
    assert(ld2.items.empty());              // it died and was removed
    assert(deaths.size() == 3);             // one death reported as an (x,y,z) triple

    // Targeting: a committed target is pursued even when another player is closer.
    {
        PlayerCombat a{}; a.id = 0;
        a.pos[0] = (2 + 0.5f) * TILE; a.pos[2] = (2 + 0.5f) * TILE;
        a.strike_reach = 1.8f; a.strike_cos = 0.4f; a.weight = 5.0f; a.block_cos = 0.3f;
        PlayerCombat b = a; b.id = 1; b.pos[0] = (8 + 0.5f) * TILE;   // far away (+x)
        std::vector<PlayerCombat> ps{ a, b };
        std::vector<FlowField> flows{ compute_flow(*m, 2, 2), compute_flow(*m, 8, 2) };
        EntityList lt;
        lt.spawn_enemy((4 + 0.5f) * TILE, (2 + 0.5f) * TILE);   // nearer to player a
        lt.items[0].target_id = 1;                              // but committed to b
        const float ex0 = lt.items[0].position[0];
        std::vector<EnemyHitPlayer> out;
        for (int i = 0; i < 200 && !lt.items.empty(); ++i)
            update_enemies(lt, *m, flows, ps, out, 0.016f);
        assert(!lt.items.empty());
        assert(lt.items[0].target_id == 1);            // stayed locked on b
        assert(lt.items[0].position[0] > ex0 + 2.0f);  // walked toward b, past the closer a
    }

    // Targeting: getting hit by another player switches aggro to that player.
    {
        const float ex = (5 + 0.5f) * TILE, ez = (2 + 0.5f) * TILE;
        PlayerCombat a{}; a.id = 0;
        a.pos[2] = ez; a.pos[0] = ex - 0.6f; a.yaw = 0.0f;          // just -x, faces +x at it
        a.strike_reach = 1.8f; a.strike_cos = 0.4f; a.strike_damage = 1.0f;
        a.strike_knockback = 0.0f; a.weight = 5.0f; a.block_cos = 0.3f;
        PlayerCombat b = a; b.id = 1; b.pos[0] = ex + 0.6f; b.yaw = 3.14159f;  // just +x, faces -x at it
        std::vector<PlayerCombat> ps{ a, b };
        std::vector<FlowField> flows{ compute_flow(*m, 5, 2), compute_flow(*m, 5, 2) };
        EntityList lt;
        lt.spawn_enemy(ex, ez);
        std::vector<EnemyHitPlayer> out;
        update_enemies(lt, *m, flows, ps, out, 0.016f);   // no strikes -> commits to nearest (a)
        assert(lt.items[0].target_id == 0);
        ps[1].strike = true;                               // b lands a blow
        update_enemies(lt, *m, flows, ps, out, 0.016f);
        assert(lt.items[0].target_id == 1);                // aggro switched to b
    }

    // Ranged enemy: advances toward its standoff, shoots, and its shots hit the player.
    {
        PlayerCombat a{}; a.id = 0;
        a.pos[0] = (8 + 0.5f) * TILE; a.pos[2] = (2 + 0.5f) * TILE;
        a.strike_reach = 1.8f; a.strike_cos = 0.4f; a.weight = 5.0f; a.block_cos = 0.3f;
        std::vector<PlayerCombat> ps{ a };
        std::vector<FlowField> flows{ compute_flow(*m, 8, 2) };
        EntityList lr2;
        lr2.spawn_enemy((2 + 0.5f) * TILE, (2 + 0.5f) * TILE, EnemyKind::Ranged);  // far (within leash)
        const float ex0 = lr2.items[0].position[0];
        std::vector<EnemyHitPlayer> out;
        bool fired = false; float total_dmg = 0.0f;
        for (int i = 0; i < 400 && !lr2.items.empty(); ++i) {
            update_enemies(lr2, *m, flows, ps, out, 0.016f);
            update_projectiles(lr2, *m, ps, out, 0.016f);
            if (!lr2.projectiles.empty()) fired = true;
            total_dmg += out[0].damage;
        }
        assert(fired);                                          // it shot at least once
        assert(total_dmg > 0.0f);                               // and a shot connected with the player
        assert(lr2.items[0].position[0] > ex0 + 1.0f);          // it advanced toward the player
        const float gap = a.pos[0] - lr2.items[0].position[0];  // same row -> x gap is the distance
        assert(gap > ENEMY_ATTACK_RANGE);                       // but kept its distance (never crowded to melee)
    }

    // Ranged enemy backs away when the target gets inside its standoff.
    {
        PlayerCombat a{}; a.id = 0;
        a.pos[0] = (5 + 0.5f) * TILE; a.pos[2] = (2 + 0.5f) * TILE;
        a.strike_reach = 1.8f; a.strike_cos = 0.4f; a.weight = 5.0f; a.block_cos = 0.3f;
        std::vector<PlayerCombat> ps{ a };
        std::vector<FlowField> flows{ compute_flow(*m, 5, 2) };
        EntityList lr3;
        lr3.spawn_enemy(a.pos[0] + 2.0f, a.pos[2], EnemyKind::Ranged);   // 2 units away (< standoff - margin)
        const float gap0 = lr3.items[0].position[0] - a.pos[0];
        std::vector<EnemyHitPlayer> out;
        for (int i = 0; i < 120; ++i) {
            update_enemies(lr3, *m, flows, ps, out, 0.016f);
            update_projectiles(lr3, *m, ps, out, 0.016f);
        }
        assert(lr3.items[0].position[0] - a.pos[0] > gap0 + 0.5f);   // it retreated
    }

    std::printf("PASS combat\n");
    return 0;
}
