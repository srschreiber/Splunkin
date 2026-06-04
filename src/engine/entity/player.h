#pragma once
#include <cglm/cglm.h>
#include <optional>
#include "engine/world/map.h"
#include "engine/entity/entity.h"

namespace dc::entity {

inline constexpr float PLAYER_RADIUS = 0.4f;   // world units
inline constexpr float GRAVITY       = 20.0f;  // units/s^2
inline constexpr float JUMP_SPEED    = 6.0f;   // units/s (initial jump velocity)
inline constexpr float MOVE_SPEED    = 4.0f;   // units/s (walk)
inline constexpr float RUN_SPEED     = 7.0f;   // units/s (hold Shift)
inline constexpr float RUN_STAMINA_PER_SEC = 12.0f;  // stamina drained while running

// Eye must sit below the head-bonk ceiling, or the vertical clamps would fight.
static_assert(dc::world::EYE_HEIGHT < dc::world::WALL_HEIGHT - 0.2f,
              "EYE_HEIGHT must be below the ceiling clamp");

inline constexpr float PLAYER_MAX_HEALTH = 100.0f;

// A shield's defensive stats. A frontal blocked hit takes max(0, dmg - block_power);
// block_cos is cos(half-angle) of the arc it covers (smaller cos = wider arc).
struct Shield {
    float block_power     = 6.0f;   // damage subtracted on a frontal block
    float block_cos       = 0.6f;   // arccos(.6) ~53 deg half-cone
    float block_speed     = 1.0f;   // raise-animation playback multiplier (lower = slower to ready)
    float stamina_per_sec = 8.0f;   // drained per second while the shield is up
    float stamina_per_hit = 15.0f;  // extra stamina drained each time it blocks a hit
};

// A weapon's offensive stats. Its attack_bonus is ADDED to the player's base
// (unarmed) attack; reach/cone define the swing arc — every enemy inside is hit.
struct Weapon {
    float attack_bonus     = 10.0f;  // added to the player's base attack_damage
    float reach            = 1.9f;   // swing reach (world units)
    float cone_cos         = 0.35f;  // arccos(.35) ~70 deg half-arc
    float attack_speed     = 0.7f;   // swing-animation playback multiplier (lower = slower)
    float cooldown         = 0.25f;  // seconds after a swing before the next is allowed
    float stamina_per_swing = 20.0f; // stamina spent per swing
};

// Unarmed (fists) fallback when no weapon is equipped: short, narrow, base damage,
// quick light jabs.
inline constexpr float UNARMED_REACH        = 1.2f;
inline constexpr float UNARMED_CONE         = 0.5f;   // arccos(.5) = 60 deg half-arc
inline constexpr float UNARMED_ATTACK_SPEED = 1.2f;
inline constexpr float UNARMED_COOLDOWN     = 0.15f;
inline constexpr float UNARMED_STAMINA      = 5.0f;   // stamina per fist jab

struct Player {
    vec3  position = {0.0f, 0.0f, 0.0f};   // EYE position (authoritative)
    float yaw   = 0.0f;                    // radians
    float pitch = 0.0f;                    // radians, clamped +-89 deg
    float vel_y = 0.0f;                    // vertical velocity
    bool  on_ground = true;
    float speed = MOVE_SPEED;              // horizontal move speed (set per-frame: walk vs run)
    float health = PLAYER_MAX_HEALTH;      // clamps at 0 (no death screen yet)
    // combat: attack_damage/knockback used when striking, weight resists incoming knockback
    Stats stats = { PLAYER_MAX_HEALTH, 5.0f, 10.0f, 5.0f };  // attack_damage = base/unarmed
    float stamina       = 100.0f;
    float stamina_max   = 100.0f;
    float stamina_regen = 18.0f;               // per second, while not blocking
    std::optional<Shield> shield = Shield{};   // equipped shield (nullopt = none)
    std::optional<Weapon> weapon = Weapon{};   // equipped weapon (nullopt = fists)
    vec3  knock_vel = {0.0f, 0.0f, 0.0f};  // horizontal knockback velocity (decays in update)
    float hit_flash = 0.0f;                // red-flash timer (decayed by main)

    // Look direction unit vector from yaw/pitch.
    void front(vec3 out) const;
    // Mouse delta (pixels): yaw += dx*sens, pitch -= dy*sens, clamp pitch.
    void add_look(float dx, float dy);
    // forward/strafe in {-1,0,1}; jump=true attempts a jump this frame.
    // Horizontal motion slides against the map; vertical applies gravity/jump.
    void update(float forward, float strafe, bool jump, float dt,
                const dc::world::Map& map);
};

} // namespace dc::entity
