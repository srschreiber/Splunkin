#pragma once
#include <cstdint>
#include <vector>
#include <cglm/cglm.h>

namespace dc::entity {

// The kinds of runtime entity. Only Enemy for now; turrets/minions/medics/door
// come later and reuse this same plain-data, serializable record.
enum class EntityType : uint8_t { Enemy };

// Enemy behavior archetype. Targeting is shared (pick_target); the per-kind action
// (melee swing vs. ranged standoff + projectile) branches in update_enemies. Flying
// is a ranged variant that hovers above the ground (only reachable by jumping).
enum class EnemyKind : uint8_t { Melee, Ranged, Flying, Flamethrower, Skeleton, Bat };

inline constexpr float FLASH_TIME = 0.15f;   // red hit-flash duration (seconds)
inline constexpr float KNOCK_DAMP = 9.0f;    // knockback velocity decay rate (1/s)
inline constexpr uint32_t NO_TARGET = 0xFFFFFFFFu;   // enemy target_id "none yet" (0 is a valid player id: the host)

// Combat stats shared by every actor (player + enemies). Knockback applied to a
// target = max(0, attacker.knockback - target.weight), as a starting impulse speed.
struct Stats {
    float max_health    = 1.0f;
    float attack_damage = 0.0f;
    float knockback     = 0.0f;   // impulse this actor deals on hit
    float weight        = 1.0f;   // resistance subtracted from incoming knockback
};

// One dynamic entity. Plain data (no pointers) so the whole list is trivially
// serializable for networking; the simulation is a pure function over it.
struct Entity {
    uint32_t   id   = 0;
    EntityType type = EntityType::Enemy;
    EnemyKind  kind = EnemyKind::Melee;
    vec3       position = {0.0f, 0.0f, 0.0f};   // feet on the floor (y = 0)
    float      yaw      = 0.0f;
    float      health   = 0.0f;
    Stats      stats;
    vec3       knock_vel  = {0.0f, 0.0f, 0.0f};  // horizontal knockback velocity (decays)
    float      hit_flash  = 0.0f;                // red-flash timer (counts down)
    float      healthbar_time = 0.0f;            // seconds left to show the over-head health bar (refreshed on hit)

    // animation / combat clocks
    float anim_time   = 0.0f;   // walk clock (advances while moving)
    bool  attacking   = false;
    float attack_time = 0.0f;   // into the attack swing
    float attack_cd   = 0.0f;   // time until the next attack is allowed
    float attack_yaw  = 0.0f;   // facing committed at the swing's start (for the hit cone)
    float punch_anim  = 0.0f;   // melee forcefield-burst visual timer (counts down; replicated)

    // committed next path cell (so flow-following doesn't jitter each frame)
    int  tgt_col = -1, tgt_row = -1;

    // AI targeting state. The enemy commits to a player (by id) and pursues THEM —
    // proximity only chooses the target when none is committed. Retarget-on-hit
    // rewrites it (rate-limited by retarget_cd). 0 = none yet -> pick nearest.
    uint32_t target_id   = NO_TARGET;
    float    retarget_cd = 0.0f;

    // Elemental status effects (applied by an elemental melee strike).
    float    burn_time   = 0.0f;   // seconds of fire burn remaining
    float    burn_dps    = 0.0f;   // burn damage per second
    float    burn_tick   = 0.0f;   // countdown to the next burn damage tick
    uint32_t burn_owner  = 0;      // player id who lit it (scoreboard credit)
    float    slow_time   = 0.0f;   // seconds of ice slow remaining
    float    slow_factor = 1.0f;   // movement multiplier while slowed (1 = normal)
    bool elite = false;            // rare elite: larger, tankier, hits harder, golden aura
    bool alive = true;
};

// A flying sphere fired by a ranged enemy. Plain data, host-simulated, replicated
// to clients for rendering. Hits the first player it reaches, then despawns.
struct Projectile {
    vec3  pos   = {0.0f, 0.0f, 0.0f};
    vec3  vel   = {0.0f, 0.0f, 0.0f};
    vec3  color = {0.75f, 0.35f, 1.0f};   // glow color (varies by shooter)
    float damage    = 0.0f;
    float knockback = 0.0f;
    float life      = 0.0f;   // seconds remaining before it fizzles
    float radius    = 0.35f;  // sphere size + hit radius (bigger for elite shots)
    bool  beam      = false;  // render as a stretched glowing laser (eye), not a sphere
    uint32_t owner_id = 0;    // the enemy that fired it (so its hits can be attributed -> taunts)
};

// The world's dynamic entities plus the sim's RNG seed. The RNG lives here (in
// the state) so the host owns the sole source of randomness for networking.
struct EntityList {
    std::vector<Entity> items;
    std::vector<Projectile> projectiles;   // in-flight enemy shots
    uint32_t next_id = 1;
    uint32_t rng     = 0x1234567u;

    Entity& spawn_enemy(float x, float z, EnemyKind kind = EnemyKind::Melee, bool elite = false);
};

} // namespace dc::entity
