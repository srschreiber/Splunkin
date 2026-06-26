#include "engine/entity/spawner.h"
#include <cmath>

namespace dc::entity {

namespace {
float rand01(uint32_t& s) { s = s * 1664525u + 1013904223u; return (s >> 8) * (1.0f / 16777216.0f); }
} // namespace

void Spawner::update(float dt, EntityList& list, const dc::world::Map& map) {
    accum += rate * dt;
    while (accum >= 1.0f) {
        accum -= 1.0f;
        if (static_cast<int>(list.items.size()) >= max_alive) { accum = 0.0f; break; }
        // Try a few random points in the disc; spawn at the first that lands on an
        // Open floor tile (uniform over the disc via sqrt on the radius).
        for (int tries = 0; tries < 8; ++tries) {
            const float ang = rand01(rng) * 6.2831853f;
            const float r   = std::sqrt(rand01(rng)) * radius;
            const float x   = pos[0] + std::cos(ang) * r;
            const float z   = pos[2] + std::sin(ang) * r;
            const int col = static_cast<int>(x / dc::world::TILE);
            const int row = static_cast<int>(z / dc::world::TILE);
            if (map.at(col, row) == dc::world::Cell::Open) {
                const float roll = rand01(rng);
                const float f2 = flying_fraction + bat_fraction;
                const float f3 = f2 + troll_fraction + demon_fraction;
                // The leftover share becomes the melee role: a Skeleton if that model is
                // enabled (skeleton_fraction > 0), otherwise the plain Melee enemy.
                const EnemyKind melee_kind = skeleton_fraction > 0.0f ? EnemyKind::Skeleton : EnemyKind::Melee;
                const float f4 = f3 + ranged_fraction + flame_fraction;
                const float f5 = f4 + insulter_fraction;
                const EnemyKind kind =
                      (roll < flying_fraction) ? EnemyKind::Flying
                    : (roll < f2) ? EnemyKind::Bat
                    : (roll < f2 + troll_fraction) ? EnemyKind::Troll
                    : (roll < f3) ? EnemyKind::Demon
                    : (roll < f3 + ranged_fraction) ? EnemyKind::Ranged
                    : (roll < f4) ? EnemyKind::Flamethrower
                    : (roll < f5) ? EnemyKind::Insulter
                    : (roll < f5 + slime_fraction) ? EnemyKind::Slime
                    : melee_kind;
                const bool elite = rand01(rng) < elite_fraction;   // rare golden bruiser (any kind)
                Entity& e = list.spawn_enemy(x, z, kind, elite);
                if (stat_mult != 1.0f) {   // escalation: tougher + hits harder as the run wears on
                    e.stats.max_health *= stat_mult; e.health = e.stats.max_health;
                    e.stats.attack_damage *= stat_mult;
                }
                break;
            }
        }
    }
}

} // namespace dc::entity
