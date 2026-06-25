#include "engine/entity/enemy.h"
#include "engine/world/collision.h"
#include <algorithm>
#include <cmath>

namespace dc::entity {

Entity& EntityList::spawn_enemy(float x, float z, EnemyKind kind, bool elite) {
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
    } else if (kind == EnemyKind::Bat) {
        e.position[1] = FLY_HOVER;     // bats hover + flap; a touch tankier than the eye
        e.stats.max_health = 18.0f; e.health = 18.0f;
    } else if (kind == EnemyKind::Flamethrower) {
        e.stats.max_health = FLAME_MAX_HEALTH;   // tanky bruiser
        e.health = e.stats.max_health;
    } else if (kind == EnemyKind::Troll) {
        e.stats = { TROLL_MAX_HEALTH, TROLL_DAMAGE, TROLL_KNOCKBACK, TROLL_WEIGHT };
        e.health = e.stats.max_health;
    } else if (kind == EnemyKind::Demon) {
        e.stats = { DEMON_MAX_HEALTH, DEMON_DAMAGE, DEMON_KNOCKBACK, DEMON_WEIGHT };
        e.health = e.stats.max_health;
    }
    if (elite) {
        e.elite = true;
        // Elite flyers get a real health pool (not the one-shot 1.0) so they're a threat.
        float base = (kind == EnemyKind::Flying) ? ELITE_FLYER_BASE_HP : e.stats.max_health;
        e.stats.max_health = base * ELITE_HEALTH_MULT;
        e.health = e.stats.max_health;
        e.stats.knockback *= ELITE_KNOCKBACK_MULT;
        e.stats.weight    *= ELITE_WEIGHT_MULT;
    }
    e.alive = true;
    items.push_back(e);
    return items.back();
}

namespace {
constexpr float RETARGET_LOCK = 0.6f;   // min seconds between hit-driven target switches
// LCG step -> a float in [0,1); same generator as pathfind's wander.
float rng01(uint32_t& s) { s = s * 1664525u + 1013904223u; return (s >> 8) * (1.0f / 16777216.0f); }

// Unit xz aim direction from (fx,fz) toward a target that's moving (tgt.pos + tgt.vel),
// led so a projectile of speed `spd` intercepts it. Solves |D + V t| = spd t for the
// soonest t > 0; falls back to a straight shot if the target outruns the bullet.
void lead_dir(float fx, float fz, const PlayerCombat& tgt, float spd, float& ox, float& oz) {
    const float Dx = tgt.pos[0] - fx, Dz = tgt.pos[2] - fz;
    const float vx = tgt.vel[0], vz = tgt.vel[2];
    const float a = vx*vx + vz*vz - spd*spd, b = 2.0f*(Dx*vx + Dz*vz), c = Dx*Dx + Dz*Dz;
    float t = -1.0f;
    if (std::fabs(a) < 1e-3f) { if (std::fabs(b) > 1e-4f) t = -c / b; }
    else { const float disc = b*b - 4.0f*a*c;
           if (disc >= 0.0f) { const float sq = std::sqrt(disc);
               const float t1 = (-b - sq) / (2.0f*a), t2 = (-b + sq) / (2.0f*a);
               t = (t1 > 0.0f && (t2 <= 0.0f || t1 < t2)) ? t1 : t2; } }
    float ax = Dx, az = Dz;
    if (t > 0.0f) { ax = Dx + vx*t; az = Dz + vz*t; }
    const float l = std::sqrt(ax*ax + az*az);
    if (l > 1e-4f) { ox = ax / l; oz = az / l; }
    else { const float dl = std::sqrt(Dx*Dx + Dz*Dz); ox = dl > 1e-4f ? Dx/dl : 1.0f; oz = dl > 1e-4f ? Dz/dl : 0.0f; }
}
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
                  uint32_t& rng, float fx, float fz, float dt, float speed_mult = 1.0f,
                  const std::vector<float>* heights = nullptr, float max_climb = 1e9f) {
    const int cc = tile_of(e.position[0]), cr = tile_of(e.position[2]);
    if (e.tgt_col < 0 || (cc == e.tgt_col && cr == e.tgt_row)) {
        int nc, nr;
        if (dc::world::flow_step(flow, cc, cr, rng, nc, nr, heights, max_climb)) { e.tgt_col = nc; e.tgt_row = nr; }
        else { e.tgt_col = -1; e.tgt_row = -1; }
    }
    float gx, gz;
    if (e.tgt_col >= 0) { gx = tile_center(e.tgt_col); gz = tile_center(e.tgt_row); }
    else                { gx = fx; gz = fz; }   // no path: head straight
    float mx = gx - e.position[0], mz = gz - e.position[2];
    const float ml = std::sqrt(mx * mx + mz * mz);
    if (ml > 1e-4f) {
        mx /= ml; mz /= ml;
        const float step = ENEMY_SPEED * speed_mult * dt;
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
                    std::vector<float>* deaths, std::vector<HitNumber>* hits,
                    const std::vector<float>* tile_heights, std::vector<float>* death_xp) {
    out.assign(players.size(), EnemyHitPlayer{});
    // Frontal-shield block pool (same model as projectiles): a blocking player spends
    // block_rate stamina per point of melee-forcefield damage to negate it, drawn down
    // across multiple punches this tick.
    std::vector<float> bsta(players.size());
    for (std::size_t i = 0; i < players.size(); ++i) bsta[i] = players[i].stamina;
    const float kdamp = std::exp(-KNOCK_DAMP * dt);
    if (players.empty()) return;

    for (auto& e : list.items) {
        if (!e.alive || e.type != EntityType::Enemy) continue;

        if (e.hit_flash > 0.0f)   e.hit_flash -= dt;
        if (e.attack_cd > 0.0f)   e.attack_cd -= dt;
        if (e.retarget_cd > 0.0f) e.retarget_cd -= dt;
        if (e.slow_time > 0.0f)   e.slow_time -= dt;
        if (e.punch_anim > 0.0f)  e.punch_anim -= dt;
        if (e.healthbar_time > 0.0f) e.healthbar_time -= dt;

        // Fire burn: deal burn_dps in BURN_INTERVAL chunks while it lasts (a number per
        // tick), credited to whoever lit it. A burn tick can kill (checked below).
        if (e.burn_time > 0.0f) {
            e.burn_time -= dt;
            e.burn_tick -= dt;
            if (e.burn_tick <= 0.0f) {
                e.burn_tick = BURN_INTERVAL;
                const float bd = e.burn_dps * BURN_INTERVAL;
                const float dealt = (bd < e.health) ? bd : (e.health > 0.0f ? e.health : 0.0f);
                e.health -= bd;
                e.healthbar_time = HEALTHBAR_TIME;
                if (hits) hits->push_back({ {e.position[0], e.position[1], e.position[2]}, bd, false });
                for (std::size_t pi = 0; pi < players.size(); ++pi)
                    if (players[pi].id == e.burn_owner) { out[pi].dealt += dealt; break; }
            }
            if (e.health <= 0.0f) { e.alive = false; continue; }   // burned to death
        }

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
                    bool crit = pc.crit_chance > 0.0f && rng01(list.rng) < pc.crit_chance;
                    const float dmg = crit ? pc.strike_damage * pc.crit_mult : pc.strike_damage;
                    out[pi].dealt += (dmg < e.health) ? dmg : (e.health > 0.0f ? e.health : 0.0f);
                    if (hits) hits->push_back({ {e.position[0], e.position[1], e.position[2]}, dmg, crit });
                    e.health -= dmg;
                    e.hit_flash = FLASH_TIME;
                    e.healthbar_time = HEALTHBAR_TIME;
                    const float kb = knock_amount(pc.strike_knockback + pc.earth_knock, e.stats.weight);
                    e.knock_vel[0] += (tx / d) * kb;   // knockback stays horizontal
                    e.knock_vel[2] += (tz / d) * kb;
                    // Elemental brands: light the burn / refresh the slow.
                    if (pc.burn_dps > 0.0f) {
                        e.burn_time = pc.burn_duration;
                        if (pc.burn_dps > e.burn_dps) e.burn_dps = pc.burn_dps;
                        e.burn_owner = pc.id;
                    }
                    if (pc.slow_factor < 1.0f) { e.slow_time = pc.slow_duration; e.slow_factor = pc.slow_factor; }
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
        const bool demon  = (e.kind == EnemyKind::Demon);   // big caster: slow exploding fireballs
        const bool ranged = (e.kind == EnemyKind::Ranged || e.kind == EnemyKind::Flying || e.kind == EnemyKind::Bat || demon);
        const bool flying = (e.kind == EnemyKind::Flying || e.kind == EnemyKind::Bat);   // bats hover + flap like the eye
        const float leash = !ranged ? 1e30f : (flying ? FLY_LEASH : RANGED_LEASH);
        const int ti = pick_target(e, players, leash);
        if (ti < 0) { e.anim_time = 0.0f; e.attacking = false; continue; }
        e.target_id = players[ti].id;            // re-stamp (acquires nearest on first tick)
        const PlayerCombat& tgt = players[ti];
        const float tdx = tgt.pos[0] - e.position[0], tdz = tgt.pos[2] - e.position[2];
        const float dist = std::sqrt(tdx * tdx + tdz * tdz);
        if (!e.attacking && dist > 1e-4f) e.yaw = std::atan2(tdz, tdx);   // track, commit during a melee swing
        const dc::world::FlowField& flow = flows[ti];
        const float move_mult = (e.slow_time > 0.0f) ? e.slow_factor : 1.0f;   // ice slow

        if (ranged) {
            // Flying is a ranged variant: longer range, hovers (position[1] untouched
            // by the xz-only movement below), and shoots a differently-colored bolt.
            const float standoff = demon ? 9.0f : flying ? FLY_STANDOFF      : RANGED_STANDOFF;
            const float fire_int = demon ? DEMON_FIRE_INTERVAL : flying ? FLY_FIRE_INTERVAL : RANGED_FIRE_INTERVAL;
            const float dmg      = demon ? DEMON_DAMAGE : flying ? FLY_DAMAGE : RANGED_DAMAGE;
            const float shot_spd = demon ? DEMON_SHOT_SPEED : flying ? FLY_SHOT_SPEED : RANGED_SHOT_SPEED;
            // Shoot a sphere on a cooldown — even while repositioning.
            if (e.attack_cd <= 0.0f && dist > 1e-4f) {
                e.attack_cd = fire_int;
                Projectile pr;
                pr.pos[0] = e.position[0]; pr.pos[1] = flying ? FLY_HOVER : 1.0f; pr.pos[2] = e.position[2];
                float ax, az; lead_dir(e.position[0], e.position[2], tgt, shot_spd, ax, az);   // predictive aim
                pr.vel[0] = ax * shot_spd;
                pr.vel[2] = az * shot_spd;
                pr.vel[1] = (dist > 0.1f) ? (tgt.pos[1] - pr.pos[1]) * shot_spd / dist : 0.0f;   // climb to the target's height
                pr.damage = dmg; pr.knockback = RANGED_KNOCKBACK; pr.life = RANGED_SHOT_LIFE;
                pr.radius = RANGED_SHOT_RADIUS; pr.owner_id = e.id;   // so its hits can taunt
                if (flying) { pr.color[0] = 1.0f; pr.color[1] = 0.12f; pr.color[2] = 0.08f; pr.beam = true; }   // red glowing laser
                if (e.kind == EnemyKind::Bat) { pr.color[0] = 0.7f; pr.color[1] = 0.25f; pr.color[2] = 0.95f; }   // purple screech bolt
                if (demon) {   // fast, big fireball that explodes on impact (splash)
                    pr.pos[1] = 1.6f;                       // hurled from chest height
                    pr.radius = DEMON_SHOT_RADIUS; pr.knockback = DEMON_KNOCKBACK; pr.life = 4.0f;
                    pr.explodes = true; pr.blast = DEMON_BLAST_RADIUS;
                    pr.color[0] = 1.0f; pr.color[1] = 0.45f; pr.color[2] = 0.1f;   // fiery orange
                    // Aim straight at the player's body from chest height (no lob), so it
                    // flies AT you, not into the sky.
                    pr.vel[1] = (dist > 0.1f) ? ((tgt.pos[1] - 0.4f) - pr.pos[1]) * shot_spd / dist : 0.0f;
                }
                if (e.elite) {                          // bigger, harder-hitting elite shot
                    pr.damage *= ELITE_DAMAGE_MULT; pr.knockback *= ELITE_KNOCKBACK_MULT;
                    pr.radius *= ELITE_PROJ_MULT;
                    pr.color[0] = std::min(1.0f, pr.color[0] + 0.3f); pr.color[1] = std::min(1.0f, pr.color[1] + 0.25f); pr.color[2] *= 0.4f;  // golden tint
                }
                list.projectiles.push_back(pr);
            }
            // Demon: play the open-mouth "yell" cast for ~0.7s after each shot (drives the
            // punch clip on every peer). Other ranged enemies don't body-animate their shots.
            if (demon) {
                e.attacking = (e.attack_cd > fire_int - 0.7f);
                if (e.attacking) e.attack_time += dt; else e.attack_time = 0.0f;
            }
            // Keep its distance: close to the standoff band, back straight off if the
            // target crowds it (still firing the whole time).
            if (dist > standoff + RANGED_BACKUP_MARGIN) {
                flow_advance(e, map, flow, list.rng, tgt.pos[0], tgt.pos[2], dt, move_mult, tile_heights, ENEMY_MAX_CLIMB);
            } else if (dist < standoff - RANGED_BACKUP_MARGIN && dist > 1e-4f) {
                const float mx = -tdx / dist, mz = -tdz / dist, step = RANGED_BACKUP_SPEED * move_mult * dt;   // retreat slowly
                const float npx = e.position[0] + mx * step, npz = e.position[2] + mz * step;
                if (!dc::world::circle_hits_solid(map, npx, e.position[2], ENEMY_RADIUS)) e.position[0] = npx;
                if (!dc::world::circle_hits_solid(map, e.position[0], npz, ENEMY_RADIUS)) e.position[2] = npz;
                e.anim_time += dt; e.tgt_col = -1;   // drop any stale advance waypoint while backing up
            } else {
                e.anim_time = 0.0f;   // in the sweet spot: stand and shoot
            }
            continue;
        }

        // --- Flamethrower: close to medium range and breathe a sustained fire cone.
        // Continuous damage to anyone in the cone, plus it lights them on fire (a burn DoT
        // that keeps ticking after they escape). `attacking` flags "currently spraying" for
        // the flame particles + light on every peer. ---
        if (e.kind == EnemyKind::Flamethrower) {
            if (dist > 1e-4f) e.yaw = std::atan2(tdz, tdx);   // track the target
            e.attack_yaw = e.yaw;
            const float fdx = std::cos(e.yaw), fdz = std::sin(e.yaw);   // facing (spray direction)
            bool firing = false;
            if (dist <= FLAME_RANGE) {
                for (std::size_t pi = 0; pi < players.size(); ++pi) {
                    const PlayerCombat& pc = players[pi];
                    if (!pc.alive || pc.invincible) continue;
                    const float px = pc.pos[0] - e.position[0], pz = pc.pos[2] - e.position[2];
                    const float pd = std::sqrt(px * px + pz * pz);
                    if (pd > FLAME_RANGE || pd < 1e-3f) continue;
                    if ((px / pd) * fdx + (pz / pd) * fdz < FLAME_CONE_COS) continue;   // outside the spray arc
                    firing = true;
                    float dmg = FLAME_DPS * dt * (e.elite ? ELITE_DAMAGE_MULT : 1.0f);   // sustained (per-tick) damage
                    // A raised shield burns the flame off (spends stamina), same as any hit.
                    if (pc.blocking && pc.block_rate > 0.0f) {
                        const float front = (-px / pd) * std::cos(pc.yaw) + (-pz / pd) * std::sin(pc.yaw);
                        if (front >= pc.block_cos) {
                            const float cost_full = pc.block_rate * dmg;
                            const float spent = bsta[pi] < cost_full ? bsta[pi] : cost_full;
                            bsta[pi] -= spent;
                            const float negated = spent / pc.block_rate;
                            out[pi].stamina_cost += spent;
                            if (negated > 0.0f) out[pi].blocked = true;
                            dmg = dmg > negated ? dmg - negated : 0.0f;
                        }
                    }
                    out[pi].damage += dmg;
                    if (dmg > 0.0f) {                     // only ignites if the flame got through
                        out[pi].hit = true;
                        out[pi].attacker_id = e.id;
                        out[pi].ignite_dps  = FLAME_BURN_DPS;
                        out[pi].ignite_time = FLAME_BURN_TIME;
                    }
                }
            }
            e.attacking = firing;
            if (e.attacking) e.attack_time += dt; else e.attack_time = 0.0f;
            // Hold around flame range: advance if too far, otherwise stand and spray.
            if (dist > FLAME_RANGE * 0.8f)
                flow_advance(e, map, flow, list.rng, tgt.pos[0], tgt.pos[2], dt, move_mult * MELEE_SPEED_MULT, tile_heights, ENEMY_MAX_CLIMB);
            else e.anim_time = 0.0f;
            continue;
        }

        // --- Melee: punch on a cooldown, LEADING the player so the forcefield lands where
        // they're heading (not where they were). The enemy keeps closing through the whole
        // wind-up and commits the swing while still a bit outside range — by the time the
        // punch lands it has closed the gap and the player has walked into the blast. ---
        // The TROLL is a melee variant: slow, long telegraphed wind-up, huge reach/damage/knock.
        const bool  troll      = (e.kind == EnemyKind::Troll);
        const float windup     = troll ? TROLL_WINDUP    : ENEMY_ATTACK_WINDUP;
        const float ff_radius  = troll ? TROLL_RADIUS    : MELEE_FORCEFIELD_RADIUS;
        const float ff_damage  = troll ? TROLL_DAMAGE    : MELEE_FORCEFIELD_DAMAGE;
        const float ff_knock   = troll ? TROLL_KNOCKBACK : MELEE_FORCEFIELD_KNOCKBACK;
        const float spd_mult   = troll ? TROLL_SPEED_MULT: MELEE_SPEED_MULT;
        const float melee_cd   = troll ? TROLL_ATTACK_CD : ENEMY_ATTACK_INTERVAL;
        // Predict the player's position at the moment a punch would land (windup remaining).
        const float tland = e.attacking ? (windup - e.attack_time) : windup;
        const float lead_x = tgt.pos[0] + tgt.vel[0] * tland;
        const float lead_z = tgt.pos[2] + tgt.vel[2] * tland;
        const float ldx = lead_x - e.position[0], ldz = lead_z - e.position[2];
        const float lead_dist = std::sqrt(ldx * ldx + ldz * ldz);
        if (lead_dist > 1e-4f) { e.yaw = std::atan2(ldz, ldx); e.attack_yaw = e.yaw; }  // face/track the intercept point
        // How far the enemy will still close before the punch lands (it never stops moving).
        const float closing = ENEMY_SPEED * spd_mult * move_mult * tland;
        if (!e.attacking && e.attack_cd <= 0.0f &&
            lead_dist - closing <= ff_radius - 0.5f) {   // will be in range when it lands
            e.attacking = true; e.attack_time = 0.0f;
        }
        if (e.attacking) {
            e.attack_time += dt;
            if (e.attack_time >= windup) {
                // Punch: slam out a spherical forcefield. Every living, non-dodging player
                // within MELEE_FORCEFIELD_RADIUS takes substantial damage and a hard radial
                // shove. A frontal shield blocks it like anything else (spends stamina to
                // negate); the other counterplay is the wind-up telegraph (dodge i-frames or
                // clear the radius before it lands).
                for (std::size_t pi = 0; pi < players.size(); ++pi) {
                    const PlayerCombat& pc = players[pi];
                    if (!pc.alive || pc.invincible) continue;
                    const float dx = pc.pos[0] - e.position[0], dz = pc.pos[2] - e.position[2];
                    const float d2 = dx * dx + dz * dz;
                    if (d2 > ff_radius * ff_radius) continue;
                    const float d = std::sqrt(d2);
                    float dmg = ff_damage * (e.elite ? ELITE_DAMAGE_MULT : 1.0f);
                    const float kb = ff_knock * (e.elite ? ELITE_KNOCKBACK_MULT : 1.0f);   // shove always lands (block stops damage, not the blast)
                    // Frontal shield negates the DAMAGE (spends block_rate stamina per point).
                    if (pc.blocking && pc.block_rate > 0.0f && d > 1e-4f) {
                        const float front = (-dx / d) * std::cos(pc.yaw) + (-dz / d) * std::sin(pc.yaw);
                        if (front >= pc.block_cos) {
                            const float cost_full = pc.block_rate * dmg;
                            const float spent = bsta[pi] < cost_full ? bsta[pi] : cost_full;
                            bsta[pi] -= spent;
                            const float negated = spent / pc.block_rate;
                            out[pi].stamina_cost += spent;
                            if (negated > 0.0f) out[pi].blocked = true;
                            dmg = dmg > negated ? dmg - negated : 0.0f;
                        }
                    }
                    out[pi].damage += dmg;
                    if (dmg > 0.0f) { out[pi].hit = true; out[pi].attacker_id = e.id; }
                    if (d > 1e-4f) {   // shove radially outward from the punch (unblockable)
                        out[pi].knock[0] += (dx / d) * kb;
                        out[pi].knock[2] += (dz / d) * kb;
                    } else {           // dead-center: shove along the enemy's facing
                        out[pi].knock[0] += std::cos(e.attack_yaw) * kb;
                        out[pi].knock[2] += std::sin(e.attack_yaw) * kb;
                    }
                }
                e.punch_anim = PUNCH_ANIM_TIME;   // forcefield-sphere visual (replicated)
                e.attacking = false;
                e.attack_cd = melee_cd;
            }
        }
        // Never pause to swing: keep chasing the LEAD point (even mid-wind-up) until
        // essentially on top of it. This is what makes the timed punch actually connect.
        if (lead_dist > ENEMY_RADIUS + 0.3f)
            flow_advance(e, map, flow, list.rng, lead_x, lead_z, dt, move_mult * spd_mult, tile_heights, ENEMY_MAX_CLIMB);
        else e.anim_time = 0.0f;   // on top of the target: hold, keep swinging
    }

    // Soft separation: nudge each enemy away from nearby enemies (strength grows as
    // they close, out to ENEMY_SEPARATION_DIST) so they don't stack on one tile. Applied
    // in place with the same per-axis wall slide as normal movement.
    for (auto& ea : list.items) {
        if (!ea.alive || ea.type != EntityType::Enemy) continue;
        float px = 0.0f, pz = 0.0f;
        for (auto& eb : list.items) {
            if (&eb == &ea || !eb.alive || eb.type != EntityType::Enemy) continue;
            const float dx = ea.position[0] - eb.position[0];
            const float dz = ea.position[2] - eb.position[2];
            const float d2 = dx * dx + dz * dz;
            if (d2 >= ENEMY_SEPARATION_DIST * ENEMY_SEPARATION_DIST || d2 < 1e-6f) continue;
            const float d = std::sqrt(d2);
            const float w = 1.0f - d / ENEMY_SEPARATION_DIST;   // 0 at the edge, 1 when coincident
            px += (dx / d) * w; pz += (dz / d) * w;
        }
        if (px == 0.0f && pz == 0.0f) continue;
        const float mx = px * ENEMY_SEPARATION_SPEED * dt;
        const float mz = pz * ENEMY_SEPARATION_SPEED * dt;
        if (!dc::world::circle_hits_solid(map, ea.position[0] + mx, ea.position[2], ENEMY_RADIUS)) ea.position[0] += mx;
        if (!dc::world::circle_hits_solid(map, ea.position[0], ea.position[2] + mz, ENEMY_RADIUS)) ea.position[2] += mz;
    }

    // Drop dead enemies (swap-pop; order doesn't matter), reporting where each died.
    for (std::size_t i = 0; i < list.items.size();) {
        if (!list.items[i].alive) {
            if (deaths) {
                deaths->push_back(list.items[i].position[0]);
                deaths->push_back(list.items[i].position[1]);
                deaths->push_back(list.items[i].position[2]);
            }
            if (death_xp) death_xp->push_back(enemy_xp(list.items[i].kind) * (list.items[i].elite ? 4.0f : 1.0f));
            list.items[i] = list.items.back();
            list.items.pop_back();
        } else ++i;
    }
}

void update_projectiles(EntityList& list, const dc::world::Map& map,
                        const std::vector<PlayerCombat>& players,
                        std::vector<EnemyHitPlayer>& out, float dt,
                        std::vector<float>* booms) {
    if (out.size() < players.size()) out.resize(players.size());   // usually pre-sized by update_enemies
    // Block stamina pool (same model as melee): a frontal shield negates damage by
    // spending block_rate stamina per point. Drawn down across multiple shots this tick.
    std::vector<float> bsta(players.size());
    for (std::size_t i = 0; i < players.size(); ++i) bsta[i] = players[i].stamina;
    for (std::size_t i = 0; i < list.projectiles.size();) {
        Projectile& p = list.projectiles[i];
        p.life -= dt;
        p.pos[0] += p.vel[0] * dt; p.pos[1] += p.vel[1] * dt; p.pos[2] += p.vel[2] * dt;   // 3D travel
        bool gone = (p.life <= 0.0f)
                  || dc::world::circle_hits_solid(map, p.pos[0], p.pos[2], RANGED_SHOT_RADIUS);
        if (!gone) {
            // First living player within reach takes the hit; the shot is spent. The hit is
            // a vertical capsule: close in xz AND within the player's body height band (so
            // a shot only connects at the right elevation — being up high matters).
            for (std::size_t pi = 0; pi < players.size(); ++pi) {
                if (!players[pi].alive || players[pi].invincible) continue;   // dodging -> shot passes through
                const float dx = players[pi].pos[0] - p.pos[0], dz = players[pi].pos[2] - p.pos[2];
                const float d2 = dx * dx + dz * dz;
                const float feet = players[pi].pos[1] - dc::world::EYE_HEIGHT;
                // Bigger (elite) shots have a proportionally bigger hit capsule.
                const float hit = PROJECTILE_HIT_DIST * (p.radius / RANGED_SHOT_RADIUS);
                const bool in_band = p.pos[1] >= feet - hit
                                  && p.pos[1] <= players[pi].pos[1] + hit;
                if (d2 <= hit * hit && in_band) {
                    const PlayerCombat& pc = players[pi];
                    const float d = std::sqrt(d2);
                    float dmg = p.damage, kb = p.knockback;
                    // Frontal shield blocks the shot (spends stamina to negate it).
                    if (pc.blocking && pc.block_rate > 0.0f && d > 1e-4f) {
                        const float front = (-dx / d) * std::cos(pc.yaw) + (-dz / d) * std::sin(pc.yaw);
                        if (front >= pc.block_cos) {
                            const float cost_full = pc.block_rate * dmg;
                            const float spent = bsta[pi] < cost_full ? bsta[pi] : cost_full;
                            bsta[pi] -= spent;
                            const float negated = spent / pc.block_rate;
                            const float taken = dmg > negated ? dmg - negated : 0.0f;
                            out[pi].stamina_cost += spent;
                            if (negated > 0.0f) out[pi].blocked = true;
                            kb *= (dmg > 1e-6f ? taken / dmg : 0.0f);
                            dmg = taken;
                        }
                    }
                    out[pi].damage += dmg;
                    if (dmg > 0.0f) { out[pi].hit = true; out[pi].attacker_id = p.owner_id; }   // the shooter taunts
                    if (d > 1e-4f) {   // shove the player along the shot's path
                        out[pi].knock[0] += (dx / d) * kb;
                        out[pi].knock[2] += (dz / d) * kb;
                    }
                    gone = true;
                    break;
                }
            }
        }
        if (gone) {
            if (p.explodes) {   // demon fireball detonates: splash all living players in range
                for (std::size_t pi = 0; pi < players.size(); ++pi) {
                    if (!players[pi].alive || players[pi].invincible) continue;
                    const float dx = players[pi].pos[0] - p.pos[0], dz = players[pi].pos[2] - p.pos[2];
                    const float d = std::sqrt(dx*dx + dz*dz);
                    if (d > p.blast) continue;
                    const float falloff = 1.0f - d / p.blast;        // full at center, 0 at the edge
                    out[pi].damage += DEMON_BLAST_DAMAGE * falloff;
                    out[pi].hit = true; out[pi].attacker_id = p.owner_id;
                    if (d > 1e-4f) { out[pi].knock[0] += (dx/d) * p.knockback; out[pi].knock[2] += (dz/d) * p.knockback; }
                }
                if (booms) { booms->push_back(p.pos[0]); booms->push_back(p.pos[1]); booms->push_back(p.pos[2]); }
            }
            list.projectiles[i] = list.projectiles.back(); list.projectiles.pop_back();
        }
        else ++i;
    }
}

float radius_attack(EntityList& list, const vec3 center, float radius,
                    float damage, float knockback, std::vector<uint32_t>& already_hit,
                    std::vector<HitNumber>* hits) {
    const float r2 = radius * radius;
    float total_dealt = 0.0f;
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

        total_dealt += (damage < e.health) ? damage : (e.health > 0.0f ? e.health : 0.0f);
        if (hits) hits->push_back({ {e.position[0], e.position[1], e.position[2]}, damage, false });
        e.health -= damage;
        e.hit_flash = FLASH_TIME;
        e.healthbar_time = HEALTHBAR_TIME;
        const float d = std::sqrt(d2);
        const float kb = knock_amount(knockback, e.stats.weight);
        if (d > 1e-4f) { e.knock_vel[0] += (dx / d) * kb; e.knock_vel[2] += (dz / d) * kb; }
        if (e.health <= 0.0f) e.alive = false;   // update_enemies compacts dead next tick
        already_hit.push_back(e.id);
    }
    return total_dealt;
}

} // namespace dc::entity
