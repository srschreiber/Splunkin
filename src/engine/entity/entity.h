#pragma once
#include <cstdint>
#include <vector>
#include <cglm/cglm.h>

namespace dc::entity {

// The kinds of runtime entity. Only Enemy for now; turrets/minions/medics/door
// come later and reuse this same plain-data, serializable record.
enum class EntityType : uint8_t { Enemy };

inline constexpr float FLASH_TIME = 0.15f;   // red hit-flash duration (seconds)
inline constexpr float KNOCK_DAMP = 9.0f;    // knockback velocity decay rate (1/s)

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
    vec3       position = {0.0f, 0.0f, 0.0f};   // feet on the floor (y = 0)
    float      yaw      = 0.0f;
    float      health   = 0.0f;
    Stats      stats;
    vec3       knock_vel  = {0.0f, 0.0f, 0.0f};  // horizontal knockback velocity (decays)
    float      hit_flash  = 0.0f;                // red-flash timer (counts down)

    // animation / combat clocks
    float anim_time   = 0.0f;   // walk clock (advances while moving)
    bool  attacking   = false;
    float attack_time = 0.0f;   // into the attack swing
    float attack_cd   = 0.0f;   // time until the next attack is allowed

    // committed next path cell (so flow-following doesn't jitter each frame)
    int  tgt_col = -1, tgt_row = -1;
    bool alive = true;
};

// The world's dynamic entities plus the sim's RNG seed. The RNG lives here (in
// the state) so the host owns the sole source of randomness for networking.
struct EntityList {
    std::vector<Entity> items;
    uint32_t next_id = 1;
    uint32_t rng     = 0x1234567u;

    Entity& spawn_enemy(float x, float z);
};

} // namespace dc::entity
