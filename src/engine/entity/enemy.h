#pragma once
#include <cglm/cglm.h>
#include "engine/entity/entity.h"
#include "engine/world/map.h"
#include "engine/world/pathfind.h"

namespace dc::entity {

inline constexpr float ENEMY_RADIUS          = 0.4f;   // collision circle
inline constexpr float ENEMY_SPEED           = 2.5f;   // units/s (a touch slower than the player)
inline constexpr float ENEMY_MAX_HEALTH      = 30.0f;
inline constexpr float ENEMY_ATTACK_DAMAGE   = 8.0f;
inline constexpr float ENEMY_KNOCKBACK       = 7.0f;   // impulse an enemy deals to the player
inline constexpr float ENEMY_WEIGHT          = 4.0f;   // resists the player's knockback
inline constexpr float ENEMY_ATTACK_RANGE    = 1.8f;   // start a swing within this of the player
inline constexpr float ENEMY_ATTACK_REACH    = 2.4f;   // hit connects within this at the strike (long, hard to backpedal out of)
inline constexpr float ENEMY_ATTACK_CONE     = -0.5f;  // arccos(-.5)=120 deg half-arc (wide; can't just circle out)
inline constexpr float ENEMY_ATTACK_INTERVAL = 1.2f;   // seconds between attacks
inline constexpr float ENEMY_ATTACK_WINDUP   = 0.4f;   // swing time before the hit lands (dodge window)
inline constexpr float ENEMY_STAGGER_SPEED   = 0.5f;   // above this knock speed, AI is suppressed
inline constexpr float BLOCK_KNOCK_ABSORB    = 0.2f;   // knockback kept when a hit is blocked

// Ranged enemy: keeps its distance and shoots spheres. It advances to RANGED_STANDOFF,
// backs up (still firing) if a target gets closer than standoff - margin, and drops a
// target that leaves RANGED_LEASH (out of range = unavailable -> retarget).
inline constexpr float RANGED_STANDOFF      = 6.0f;    // preferred distance from the target
inline constexpr float RANGED_BACKUP_MARGIN = 1.5f;    // hysteresis band around the standoff
inline constexpr float RANGED_BACKUP_SPEED  = 1.4f;    // retreat speed (slower than its advance, ENEMY_SPEED)
inline constexpr float RANGED_LEASH         = 13.0f;   // give up on a target past this
inline constexpr float RANGED_FIRE_INTERVAL = 1.3f;    // seconds between shots (attack speed)
inline constexpr float RANGED_DAMAGE        = 7.0f;
inline constexpr float RANGED_KNOCKBACK     = 3.0f;
inline constexpr float RANGED_SHOT_SPEED    = 9.0f;    // projectile travel speed (units/s)
inline constexpr float RANGED_SHOT_RADIUS   = 0.35f;   // sphere size + hit radius
inline constexpr float RANGED_SHOT_LIFE     = 2.5f;    // seconds before a shot fizzles
inline constexpr float PROJECTILE_HIT_DIST  = 0.6f;    // shot within this of a player connects

// Flying enemy: a ranged variant that hovers above the ground with longer range.
// Reachable in melee only by jumping up to it (see the vertical strike reach below).
inline constexpr float FLY_HOVER         = 2.5f;    // hover height above its ground (relative)
inline constexpr float FLY_STANDOFF      = 5.0f;    // close enough that a chasing player can corner + jump it
inline constexpr float FLY_LEASH         = 18.0f;   // better range than ground ranged
inline constexpr float FLY_FIRE_INTERVAL = 1.6f;
inline constexpr float FLY_DAMAGE        = 6.0f;
inline constexpr float FLY_SHOT_SPEED    = 11.0f;

// Melee vertical reach: a swing connects only if the target's height (relative to its
// ground) is within this window of the player's strike elevation (feet-above-ground,
// 0 when standing). Ground enemies (height 0) are always in reach; the flyer (height
// FLY_HOVER) sits just above the standing window, so you must jump to clip it.
// 3D swing cone: the hit test is a cone around the player's full look direction, so
// looking up lets you reach the flyer. Heights are relative to ground (close-range
// melee, so terrain difference is negligible): the swing originates at chest height
// (+ jump), and a target's point is its body center (the flyer's is its hover height).
inline constexpr float STRIKE_ORIGIN_Y   = 1.3f;   // swing origin above the player's feet
inline constexpr float GROUND_BODY_CENTER = 1.0f;  // a ground enemy's aim point above its feet

// The player's state this frame, for combat resolution.
struct PlayerCombat {
    uint32_t id;          // stable player id (host = 0, clients 1..); enemies target by id
    bool  alive = true;   // dead players (ghosts) are ignored by enemies and deal no damage
    vec3  pos;            // player position (eye)
    vec3  aim = {0,0,0};  // 3D look direction (yaw+pitch); the strike is a cone around this
    float yaw;
    bool  blocking;
    bool  strike;         // true on the single frame the player's swing connects
    float strike_reach;   // how far the swing reaches
    float strike_cos;     // cos(half-angle) of the forward hit cone
    float strike_damage;
    float strike_knockback;  // the player's knockback stat (applied to enemies hit)
    float weight;            // the player's weight (resists incoming knockback)
    float strike_height = 0.0f;  // feet height above own ground (0 standing, >0 mid-jump) for vertical reach
    // Shield: a frontal blocked hit spends block_rate stamina per point of damage to
    // negate it; with enough `stamina` the hit is fully negated (and its knockback
    // scaled away), otherwise the unaffordable remainder gets through at full damage.
    float block_cos = 0.0f;  // cos(half-angle) of the shield's frontal arc
    float block_rate = 0.0f; // stamina per point of damage blocked (0 = no shield)
    float stamina   = 0.0f;  // stamina available to spend blocking this tick
};

// What the enemy tick did to the player (so the caller can apply it — keeps the
// sim pure and the result serializable).
struct EnemyHitPlayer {
    float damage  = 0.0f;
    vec3  knock   = {0.0f, 0.0f, 0.0f};  // impulse to add to the player's knock_vel
    bool  hit     = false;               // damage got through (-> red flash)
    bool  blocked = false;               // the shield absorbed some damage this tick
    float stamina_cost = 0.0f;           // stamina the shield spent blocking (caller deducts)
};

// Advance enemies one tick against ALL players (co-op). Each player's strike is
// resolved against enemies; then every enemy pursues its committed target player
// (descending THAT player's flow field) and attacks it in range. `flows` is parallel
// to `players` (flows[i] is the distance field to players[i]); an enemy with no
// committed target picks the nearest, and retarget-on-hit switches it to the player
// who just struck it. `out` is sized to players.size(); out[i] is what was done to
// player i (damage/knock/flash). Pure over (list, map, flows, players) + list.rng.
// Dead enemies are removed. `deaths` (optional): each removed enemy appends its
// position as 3 floats (x,y,z) so the caller can drop loot.
void update_enemies(EntityList& list, const dc::world::Map& map,
                    const std::vector<dc::world::FlowField>& flows,
                    const std::vector<PlayerCombat>& players,
                    std::vector<EnemyHitPlayer>& out, float dt,
                    std::vector<float>* deaths = nullptr);

// Advance in-flight projectiles (host-authoritative): move, expire by lifetime, die
// on walls, and on reaching a LIVING player deal damage + knockback into out[i]
// (parallel to `players`, same convention as update_enemies). Pure over
// (list.projectiles, map, players). Spent shots are removed.
void update_projectiles(EntityList& list, const dc::world::Map& map,
                        const std::vector<PlayerCombat>& players,
                        std::vector<EnemyHitPlayer>& out, float dt);

// Damage + knock every enemy within `radius` (xz) of `center`, skipping ids already
// in `already_hit` (so a moving hazard hits each enemy once per pass). Marks the
// dead; the caller's next update_enemies compacts them. Pure / reusable (thrown
// sword now, explosions later).
void radius_attack(EntityList& list, const vec3 center, float radius,
                   float damage, float knockback, std::vector<uint32_t>& already_hit);

} // namespace dc::entity
