#include "engine/entity/enemy.h"
#include "engine/world/collision.h"
#include <cmath>

namespace dc::entity {

Entity& EntityList::spawn_enemy(float x, float z) {
    Entity e;
    e.id = next_id++;
    e.type = EntityType::Enemy;
    e.position[0] = x; e.position[1] = 0.0f; e.position[2] = z;
    e.stats = { ENEMY_MAX_HEALTH, ENEMY_ATTACK_DAMAGE, ENEMY_KNOCKBACK, ENEMY_WEIGHT };
    e.health = e.stats.max_health;
    e.alive = true;
    items.push_back(e);
    return items.back();
}

namespace {
int   tile_of(float v)     { return static_cast<int>(v / dc::world::TILE); }
float tile_center(int t)   { return (t + 0.5f) * dc::world::TILE; }
// Knockback delivered = max(0, attacker knockback - target weight).
float knock_amount(float attacker_kb, float target_weight) {
    const float a = attacker_kb - target_weight;
    return a > 0.0f ? a : 0.0f;
}
} // namespace

EnemyHitPlayer update_enemies(EntityList& list, const dc::world::Map& map,
                              const dc::world::FlowField& flow, const PlayerCombat& pc, float dt) {
    EnemyHitPlayer out;
    const float fwd_x = std::cos(pc.yaw), fwd_z = std::sin(pc.yaw);   // player forward (xz)
    const float kdamp = std::exp(-KNOCK_DAMP * dt);

    for (auto& e : list.items) {
        if (!e.alive || e.type != EntityType::Enemy) continue;

        if (e.hit_flash > 0.0f) e.hit_flash -= dt;

        // --- Player's strike (one hit per swing): in reach AND within the cone ---
        if (pc.strike) {
            const float tx = e.position[0] - pc.pos[0];
            const float tz = e.position[2] - pc.pos[2];
            const float d = std::sqrt(tx * tx + tz * tz);
            if (d <= pc.strike_reach && d > 1e-4f) {
                const float dot = (tx / d) * fwd_x + (tz / d) * fwd_z;
                if (dot >= pc.strike_cos) {
                    e.health -= pc.strike_damage;
                    e.hit_flash = FLASH_TIME;                       // red flash
                    const float kb = knock_amount(pc.strike_knockback, e.stats.weight);
                    e.knock_vel[0] += (tx / d) * kb;                // shove away from the player
                    e.knock_vel[2] += (tz / d) * kb;
                    if (e.health <= 0.0f) { e.alive = false; continue; }
                }
            }
        }

        // --- Integrate knockback (decays), per-axis wall slide ---
        const float kdx = e.knock_vel[0] * dt, kdz = e.knock_vel[2] * dt;
        if (!dc::world::circle_hits_solid(map, e.position[0] + kdx, e.position[2], ENEMY_RADIUS)) e.position[0] += kdx;
        else e.knock_vel[0] = 0.0f;
        if (!dc::world::circle_hits_solid(map, e.position[0], e.position[2] + kdz, ENEMY_RADIUS)) e.position[2] += kdz;
        else e.knock_vel[2] = 0.0f;
        const float kspeed = std::sqrt(e.knock_vel[0] * e.knock_vel[0] + e.knock_vel[2] * e.knock_vel[2]);
        e.knock_vel[0] *= kdamp; e.knock_vel[2] *= kdamp;
        if (kspeed > ENEMY_STAGGER_SPEED) { e.attacking = false; e.anim_time = 0.0f; continue; }  // reeling: no AI

        // --- Distance to player (xz) ---
        const float dx = pc.pos[0] - e.position[0];
        const float dz = pc.pos[2] - e.position[2];
        const float dist = std::sqrt(dx * dx + dz * dz);
        if (dist > 1e-4f) e.yaw = std::atan2(dz, dx);   // face the player

        if (dist <= ENEMY_ATTACK_RANGE) {
            // In range: attack on a cooldown; the hit lands mid-swing.
            e.anim_time = 0.0f;
            if (e.attack_cd > 0.0f) e.attack_cd -= dt;
            if (!e.attacking && e.attack_cd <= 0.0f) { e.attacking = true; e.attack_time = 0.0f; }
            if (e.attacking) {
                e.attack_time += dt;
                if (e.attack_time >= ENEMY_ATTACK_WINDUP) {
                    float dmg = e.stats.attack_damage;
                    float kb  = knock_amount(e.stats.knockback, pc.weight);
                    // Directional shield block: only when blocking AND the attacker
                    // is within the shield's frontal cone. Subtracts block_power
                    // from the damage and soaks most of the knockback.
                    bool blocked = false;
                    if (pc.blocking && dist > 1e-4f) {
                        const float front = (-dx / dist) * fwd_x + (-dz / dist) * fwd_z;
                        if (front >= pc.block_cos) {
                            dmg -= pc.block_power; if (dmg < 0.0f) dmg = 0.0f;
                            kb  *= BLOCK_KNOCK_ABSORB;
                            blocked = true;
                        }
                    }
                    out.damage += dmg;
                    if (blocked) out.blocked = true;     // shield ate it -> no red flash
                    else         out.hit = true;         // took it on the chin -> red flash
                    if (dist > 1e-4f) {                  // shove the player AWAY from this enemy
                        out.knock[0] += (dx / dist) * kb;
                        out.knock[2] += (dz / dist) * kb;
                    }
                    e.attacking = false;
                    e.attack_cd = ENEMY_ATTACK_INTERVAL;
                }
            }
        } else {
            // Out of range: descend the flow field toward the player.
            e.attacking = false;
            const int cc = tile_of(e.position[0]);
            const int cr = tile_of(e.position[2]);
            // Refresh the committed next cell once we arrive (avoids per-frame jitter).
            if (e.tgt_col < 0 || (cc == e.tgt_col && cr == e.tgt_row)) {
                int nc, nr;
                if (dc::world::flow_step(flow, cc, cr, list.rng, nc, nr)) { e.tgt_col = nc; e.tgt_row = nr; }
                else { e.tgt_col = -1; e.tgt_row = -1; }
            }
            float gx, gz;
            if (e.tgt_col >= 0) { gx = tile_center(e.tgt_col); gz = tile_center(e.tgt_row); }
            else                { gx = pc.pos[0]; gz = pc.pos[2]; }   // no path: head straight
            float mx = gx - e.position[0], mz = gz - e.position[2];
            const float ml = std::sqrt(mx * mx + mz * mz);
            if (ml > 1e-4f) {
                mx /= ml; mz /= ml;
                const float step = ENEMY_SPEED * dt;
                const float npx = e.position[0] + mx * step;
                const float npz = e.position[2] + mz * step;
                if (!dc::world::circle_hits_solid(map, npx, e.position[2], ENEMY_RADIUS)) e.position[0] = npx;
                if (!dc::world::circle_hits_solid(map, e.position[0], npz, ENEMY_RADIUS)) e.position[2] = npz;
                e.anim_time += dt;
            }
        }
    }

    // Drop dead enemies (swap-pop; order doesn't matter).
    for (std::size_t i = 0; i < list.items.size();) {
        if (!list.items[i].alive) { list.items[i] = list.items.back(); list.items.pop_back(); }
        else ++i;
    }
    return out;
}

} // namespace dc::entity
