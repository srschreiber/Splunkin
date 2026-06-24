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
                const EnemyKind kind =
                      (roll < flying_fraction) ? EnemyKind::Flying
                    : (roll < flying_fraction + ranged_fraction) ? EnemyKind::Ranged
                    : (roll < flying_fraction + ranged_fraction + flame_fraction) ? EnemyKind::Flamethrower
                    : EnemyKind::Melee;
                list.spawn_enemy(x, z, kind);
                break;
            }
        }
    }
}

} // namespace dc::entity
