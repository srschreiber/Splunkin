#include "engine/entity/enemy.h"
#include "engine/world/collision.h"
#include <cmath>

namespace dc::entity {

Entity& EntityList::spawn_enemy(float x, float z, EnemyKind kind) {
    Entity e;
    e.id = next_id++;
    e.type = EntityType::Enemy;
    e.kind = kind;
    e.position[0] = x; e.position[1] = 0.0f; e.position[2] = z;
    e.stats = { ENEMY_MAX_HEALTH, ENEMY_ATTACK_DAMAGE, ENEMY_KNOCKBACK, ENEMY_WEIGHT };
    e.health = e.stats.max_health;
    if (kind == EnemyKind::Flying) {
        e.position[1] = FLY_HOVER;     // hovers (relative height); reachable only by jumping
        e.health = 1.0f;               // fragile: one jump-hit or thrown sword drops it
    }
    e.alive = true;
    items.push_back(e);
    return items.back();
}

namespace {
constexpr float RETARGET_LOCK = 0.6f;   // min seconds between hit-driven target switches
int   tile_of(float v)     { return static_cast<int>(v / dc::world::TILE); }
float tile_center(int t)   { return (t + 0.5f) * dc::world::TILE; }
// Knockback delivered = max(0, attacker knockback - target weight).
float knock_amount(float attacker_kb, float target_weight) {
    const float a = attacker_kb - target_weight;
    return a > 0.0f ? a : 0.0f;
}
// Which player INDEX this enemy pursues: its committed target if that player is
// still present, alive, and within max_dist, otherwise the nearest LIVING player in
// range. Dead players (ghosts) and players beyond max_dist are ignored (the latter
// lets ranged enemies drop a target that flees out of range). Returns -1 if no
// valid target. target_id is rewritten elsewhere (on hit / when a target leaves).
int pick_target(const Entity& e, const std::vector<PlayerCombat>& players, float max_dist) {
    const float max_d2 = max_dist * max_dist;
    int nearest = -1, committed = -1;
    float best = 1e30f;
    for (std::size_t i = 0; i < players.size(); ++i) {
        if (!players[i].alive) continue;
        const float dx = players[i].pos[0] - e.position[0];
        const float dz = players[i].pos[2] - e.position[2];
        const float d2 = dx * dx + dz * dz;
        if (d2 > max_d2) continue;                       // out of range -> unavailable
        if (players[i].id == e.target_id) committed = static_cast<int>(i);
        if (d2 < best) { best = d2; nearest = static_cast<int>(i); }
    }
    return committed >= 0 ? committed : nearest;
}
// Step one tick toward the target along its flow field (committing to a next cell so
// it doesn't jitter), falling back to a straight line at (fx,fz) when off the field.
// Shared by melee chase and ranged approach. Advances the walk clock when it moves.
void flow_advance(Entity& e, const dc::world::Map& map, const dc::world::FlowField& flow,
                  uint32_t& rng, float fx, float fz, float dt) {
    const int cc = tile_of(e.position[0]), cr = tile_of(e.position[2]);
    if (e.tgt_col < 0 || (cc == e.tgt_col && cr == e.tgt_row)) {
        int nc, nr;
        if (dc::world::flow_step(flow, cc, cr, rng, nc, nr)) { e.tgt_col = nc; e.tgt_row = nr; }
        else { e.tgt_col = -1; e.tgt_row = -1; }
    }
    float gx, gz;
    if (e.tgt_col >= 0) { gx = tile_center(e.tgt_col); gz = tile_center(e.tgt_row); }
    else                { gx = fx; gz = fz; }   // no path: head straight
    float mx = gx - e.position[0], mz = gz - e.position[2];
    const float ml = std::sqrt(mx * mx + mz * mz);
    if (ml > 1e-4f) {
        mx /= ml; mz /= ml;
        const float step = ENEMY_SPEED * dt;
        const float npx = e.position[0] + mx * step, npz = e.position[2] + mz * step;
        if (!dc::world::circle_hits_solid(map, npx, e.position[2], ENEMY_RADIUS)) e.position[0] = npx;
        if (!dc::world::circle_hits_solid(map, e.position[0], npz, ENEMY_RADIUS)) e.position[2] = npz;
        e.anim_time += dt;
    } else {
        e.anim_time = 0.0f;
    }
}
} // namespace

void update_enemies(EntityList& list, const dc::world::Map& map,
                    const std::vector<dc::world::FlowField>& flows, const std::vector<PlayerCombat>& players,
                    std::vector<EnemyHitPlayer>& out, float dt,
                    std::vector<float>* deaths) {
    out.assign(players.size(), EnemyHitPlayer{});
    // Remaining block stamina per player this tick, drawn down as the shield negates
    // hits (so several attackers in one frame can't each spend the same stamina).
    std::vector<float> block_sta(players.size());
    for (std::size_t i = 0; i < players.size(); ++i) block_sta[i] = players[i].stamina;
    const float kdamp = std::exp(-KNOCK_DAMP * dt);
    if (players.empty()) return;

    for (auto& e : list.items) {
        if (!e.alive || e.type != EntityType::Enemy) continue;

        if (e.hit_flash > 0.0f)   e.hit_flash -= dt;
        if (e.attack_cd > 0.0f)   e.attack_cd -= dt;
        if (e.retarget_cd > 0.0f) e.retarget_cd -= dt;

        // --- Each striking player's hit on this enemy (in their reach + cone).
        // Remember who landed the last blow this frame for retarget-on-hit. ---
        bool struck = false; uint32_t struck_by = 0;
        for (std::size_t pi = 0; pi < players.size(); ++pi) {
            const PlayerCombat& pc = players[pi];
            if (!pc.strike || !pc.alive) continue;   // ghosts deal no damage
            // 3D cone around the player's look direction. dx/dz are world-horizontal;
            // dy is relative-height (swing origin vs the target's body center), valid
            // at melee range where the two grounds are ~equal. Looking up reaches the flyer.
            const float tx = e.position[0] - pc.pos[0];
            const float tz = e.position[2] - pc.pos[2];
            const float enemy_y  = (e.kind == EnemyKind::Flying) ? e.position[1] : GROUND_BODY_CENTER;
            const float ty = enemy_y - (STRIKE_ORIGIN_Y + pc.strike_height);
            const float d = std::sqrt(tx * tx + ty * ty + tz * tz);
            if (d <= pc.strike_reach && d > 1e-4f) {
                const float dot = (tx * pc.aim[0] + ty * pc.aim[1] + tz * pc.aim[2]) / d;
                if (dot >= pc.strike_cos) {
                    e.health -= pc.strike_damage;
                    e.hit_flash = FLASH_TIME;
                    const float kb = knock_amount(pc.strike_knockback, e.stats.weight);
                    e.knock_vel[0] += (tx / d) * kb;   // knockback stays horizontal
                    e.knock_vel[2] += (tz / d) * kb;
                    struck = true; struck_by = pc.id;
                }
            }
        }
        if (e.health <= 0.0f) { e.alive = false; continue; }

        // Retarget-on-hit: the last player to land a blow grabs aggro, rate-limited
        // so two attackers don't make the enemy flip its target every frame.
        if (struck && e.retarget_cd <= 0.0f && struck_by != e.target_id) {
            e.target_id = struck_by; e.retarget_cd = RETARGET_LOCK;
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

        // --- Pursued target: the committed player (by id), or the nearest if none.
        // Proximity does NOT override a commitment — only retarget-on-hit / the
        // target leaving does. ti indexes both players[] and flows[]. ---
        // Ranged enemies give up a target that flees past their leash; melee chase
        // forever (huge cap). -1 = nobody valid in range -> idle.
        const bool ranged = (e.kind == EnemyKind::Ranged || e.kind == EnemyKind::Flying);
        const bool flying = (e.kind == EnemyKind::Flying);
        const float leash = !ranged ? 1e30f : (flying ? FLY_LEASH : RANGED_LEASH);
        const int ti = pick_target(e, players, leash);
        if (ti < 0) { e.anim_time = 0.0f; e.attacking = false; continue; }
        e.target_id = players[ti].id;            // re-stamp (acquires nearest on first tick)
        const PlayerCombat& tgt = players[ti];
        const float tdx = tgt.pos[0] - e.position[0], tdz = tgt.pos[2] - e.position[2];
        const float dist = std::sqrt(tdx * tdx + tdz * tdz);
        if (!e.attacking && dist > 1e-4f) e.yaw = std::atan2(tdz, tdx);   // track, commit during a melee swing
        const dc::world::FlowField& flow = flows[ti];

        if (ranged) {
            // Flying is a ranged variant: longer range, hovers (position[1] untouched
            // by the xz-only movement below), and shoots a differently-colored bolt.
            const float standoff = flying ? FLY_STANDOFF      : RANGED_STANDOFF;
            const float fire_int = flying ? FLY_FIRE_INTERVAL : RANGED_FIRE_INTERVAL;
            const float dmg      = flying ? FLY_DAMAGE        : RANGED_DAMAGE;
            const float shot_spd = flying ? FLY_SHOT_SPEED    : RANGED_SHOT_SPEED;
            // Shoot a sphere on a cooldown — even while repositioning.
            if (e.attack_cd <= 0.0f && dist > 1e-4f) {
                e.attack_cd = fire_int;
                Projectile pr;
                pr.pos[0] = e.position[0]; pr.pos[1] = flying ? FLY_HOVER : 1.0f; pr.pos[2] = e.position[2];
                pr.vel[0] = (tdx / dist) * shot_spd;
                pr.vel[2] = (tdz / dist) * shot_spd;
                pr.damage = dmg; pr.knockback = RANGED_KNOCKBACK; pr.life = RANGED_SHOT_LIFE;
                if (flying) { pr.color[0] = 1.0f; pr.color[1] = 0.5f; pr.color[2] = 0.2f; }   // orange flyer bolts
                list.projectiles.push_back(pr);
            }
            // Keep its distance: close to the standoff band, back straight off if the
            // target crowds it (still firing the whole time).
            if (dist > standoff + RANGED_BACKUP_MARGIN) {
                flow_advance(e, map, flow, list.rng, tgt.pos[0], tgt.pos[2], dt);
            } else if (dist < standoff - RANGED_BACKUP_MARGIN && dist > 1e-4f) {
                const float mx = -tdx / dist, mz = -tdz / dist, step = RANGED_BACKUP_SPEED * dt;   // retreat slowly
                const float npx = e.position[0] + mx * step, npz = e.position[2] + mz * step;
                if (!dc::world::circle_hits_solid(map, npx, e.position[2], ENEMY_RADIUS)) e.position[0] = npx;
                if (!dc::world::circle_hits_solid(map, e.position[0], npz, ENEMY_RADIUS)) e.position[2] = npz;
                e.anim_time += dt; e.tgt_col = -1;   // drop any stale advance waypoint while backing up
            } else {
                e.anim_time = 0.0f;   // in the sweet spot: stand and shoot
            }
            continue;
        }

        // --- Melee: swing on a cooldown (facing committed at the swing's start so
        // they can sidestep/back out), and chase via the flow field. ---
        if (!e.attacking && dist <= ENEMY_ATTACK_RANGE && e.attack_cd <= 0.0f) {
            e.attacking = true; e.attack_time = 0.0f; e.attack_yaw = e.yaw;
        }
        if (e.attacking) {
            e.attack_time += dt;
            if (e.attack_time >= ENEMY_ATTACK_WINDUP) {
                const PlayerCombat& pc = tgt;
                const float adx = pc.pos[0] - e.position[0], adz = pc.pos[2] - e.position[2];
                const float ad = std::sqrt(adx * adx + adz * adz);
                const float afx = std::cos(e.attack_yaw), afz = std::sin(e.attack_yaw);
                const bool in_cone = ad > 1e-4f && ad <= ENEMY_ATTACK_REACH
                                   && ((adx / ad) * afx + (adz / ad) * afz) >= ENEMY_ATTACK_CONE;
                if (in_cone) {
                    float dmg = e.stats.attack_damage;
                    float kb  = knock_amount(e.stats.knockback, pc.weight);
                    const float front = (-adx / ad) * std::cos(pc.yaw) + (-adz / ad) * std::sin(pc.yaw);
                    // Frontal shield: spend block_rate stamina per point of damage to
                    // negate it. Out of stamina -> the unaffordable remainder lands.
                    if (pc.blocking && pc.block_rate > 0.0f && front >= pc.block_cos) {
                        const float cost_full = pc.block_rate * dmg;            // to negate it all
                        const float spent = block_sta[ti] < cost_full ? block_sta[ti] : cost_full;
                        block_sta[ti] -= spent;
                        const float negated = spent / pc.block_rate;            // damage absorbed
                        const float taken = dmg > negated ? dmg - negated : 0.0f;
                        out[ti].stamina_cost += spent;
                        if (negated > 0.0f) out[ti].blocked = true;
                        kb *= (dmg > 1e-6f ? taken / dmg : 0.0f);               // knockback only for the part that lands
                        dmg = taken;
                    }
                    out[ti].damage += dmg;
                    if (dmg > 0.0f) out[ti].hit = true;                         // flash only if damage got through
                    out[ti].knock[0] += (adx / ad) * kb;   // shove that player away from this enemy
                    out[ti].knock[2] += (adz / ad) * kb;
                }
                e.attacking = false;
                e.attack_cd = ENEMY_ATTACK_INTERVAL;
            }
        }
        if (dist > ENEMY_ATTACK_RANGE * 0.7f) flow_advance(e, map, flow, list.rng, tgt.pos[0], tgt.pos[2], dt);
        else e.anim_time = 0.0f;   // point-blank: hold position, keep swinging
    }

    // Drop dead enemies (swap-pop; order doesn't matter), reporting where each died.
    for (std::size_t i = 0; i < list.items.size();) {
        if (!list.items[i].alive) {
            if (deaths) {
                deaths->push_back(list.items[i].position[0]);
                deaths->push_back(list.items[i].position[1]);
                deaths->push_back(list.items[i].position[2]);
            }
            list.items[i] = list.items.back();
            list.items.pop_back();
        } else ++i;
    }
}

void update_projectiles(EntityList& list, const dc::world::Map& map,
                        const std::vector<PlayerCombat>& players,
                        std::vector<EnemyHitPlayer>& out, float dt) {
    if (out.size() < players.size()) out.resize(players.size());   // usually pre-sized by update_enemies
    for (std::size_t i = 0; i < list.projectiles.size();) {
        Projectile& p = list.projectiles[i];
        p.life -= dt;
        p.pos[0] += p.vel[0] * dt; p.pos[2] += p.vel[2] * dt;
        bool gone = (p.life <= 0.0f)
                  || dc::world::circle_hits_solid(map, p.pos[0], p.pos[2], RANGED_SHOT_RADIUS);
        if (!gone) {
            // First living player within reach takes the hit; the shot is spent.
            for (std::size_t pi = 0; pi < players.size(); ++pi) {
                if (!players[pi].alive) continue;
                const float dx = players[pi].pos[0] - p.pos[0], dz = players[pi].pos[2] - p.pos[2];
                const float d2 = dx * dx + dz * dz;
                if (d2 <= PROJECTILE_HIT_DIST * PROJECTILE_HIT_DIST) {
                    out[pi].damage += p.damage;
                    out[pi].hit = true;
                    const float d = std::sqrt(d2);
                    if (d > 1e-4f) {   // shove the player along the shot's path
                        out[pi].knock[0] += (dx / d) * p.knockback;
                        out[pi].knock[2] += (dz / d) * p.knockback;
                    }
                    gone = true;
                    break;
                }
            }
        }
        if (gone) { list.projectiles[i] = list.projectiles.back(); list.projectiles.pop_back(); }
        else ++i;
    }
}

void radius_attack(EntityList& list, const vec3 center, float radius,
                   float damage, float knockback, std::vector<uint32_t>& already_hit) {
    const float r2 = radius * radius;
    for (auto& e : list.items) {
        if (!e.alive || e.type != EntityType::Enemy) continue;
        // skip enemies already hit this pass
        bool seen = false;
        for (uint32_t id : already_hit) if (id == e.id) { seen = true; break; }
        if (seen) continue;

        const float dx = e.position[0] - center[0];
        const float dz = e.position[2] - center[2];
        const float d2 = dx * dx + dz * dz;
        if (d2 > r2) continue;

        e.health -= damage;
        e.hit_flash = FLASH_TIME;
        const float d = std::sqrt(d2);
        const float kb = knock_amount(knockback, e.stats.weight);
        if (d > 1e-4f) { e.knock_vel[0] += (dx / d) * kb; e.knock_vel[2] += (dz / d) * kb; }
        if (e.health <= 0.0f) e.alive = false;   // update_enemies compacts dead next tick
        already_hit.push_back(e.id);
    }
}

} // namespace dc::entity
