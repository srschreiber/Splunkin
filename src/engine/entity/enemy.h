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
inline constexpr float ENEMY_ATTACK_RANGE    = 1.3f;   // stop & attack within this of the player
inline constexpr float ENEMY_ATTACK_INTERVAL = 1.2f;   // seconds between attacks
inline constexpr float ENEMY_ATTACK_WINDUP   = 0.4f;   // swing time before the hit lands
inline constexpr float ENEMY_STAGGER_SPEED   = 0.5f;   // above this knock speed, AI is suppressed
inline constexpr float BLOCK_KNOCK_ABSORB    = 0.2f;   // knockback kept when a hit is blocked

// The player's state this frame, for combat resolution.
struct PlayerCombat {
    vec3  pos;            // player position (eye)
    float yaw;
    bool  blocking;
    bool  strike;         // true on the single frame the player's swing connects
    float strike_reach;   // how far the swing reaches
    float strike_cos;     // cos(half-angle) of the forward hit cone
    float strike_damage;
    float strike_knockback;  // the player's knockback stat (applied to enemies hit)
    float weight;            // the player's weight (resists incoming knockback)
    // Shield: a blocked hit (attacker within the frontal cone) takes
    // max(0, damage - block_power) and most of its knockback absorbed.
    float block_cos;         // cos(half-angle) of the shield's frontal arc
    float block_power;       // damage the shield subtracts when it blocks
};

// What the enemy tick did to the player (so the caller can apply it — keeps the
// sim pure and the result serializable).
struct EnemyHitPlayer {
    float damage  = 0.0f;
    vec3  knock   = {0.0f, 0.0f, 0.0f};  // impulse to add to the player's knock_vel
    bool  hit     = false;               // an UNBLOCKED hit landed (-> red flash)
    bool  blocked = false;               // a hit was absorbed by the shield (-> no red flash)
};

// Advance all enemies one tick: resolve the player's strike (damage + knockback +
// flash on enemies), then pathfind/move toward the player and attack when in
// range. Returns what was done to the player. Pure over (list, map, flow, player)
// + list.rng. Dead enemies are removed.
EnemyHitPlayer update_enemies(EntityList& list, const dc::world::Map& map,
                              const dc::world::FlowField& flow, const PlayerCombat& pc, float dt);

} // namespace dc::entity
