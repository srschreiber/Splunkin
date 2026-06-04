#pragma once
#include <cglm/cglm.h>
#include "engine/world/map.h"
#include "engine/entity/entity.h"

namespace dc::entity {

inline constexpr float PLAYER_RADIUS = 0.4f;   // world units
inline constexpr float GRAVITY       = 20.0f;  // units/s^2
inline constexpr float JUMP_SPEED    = 6.0f;   // units/s (initial jump velocity)
inline constexpr float MOVE_SPEED    = 4.0f;   // units/s

// Eye must sit below the head-bonk ceiling, or the vertical clamps would fight.
static_assert(dc::world::EYE_HEIGHT < dc::world::WALL_HEIGHT - 0.2f,
              "EYE_HEIGHT must be below the ceiling clamp");

inline constexpr float PLAYER_MAX_HEALTH = 100.0f;

struct Player {
    vec3  position = {0.0f, 0.0f, 0.0f};   // EYE position (authoritative)
    float yaw   = 0.0f;                    // radians
    float pitch = 0.0f;                    // radians, clamped +-89 deg
    float vel_y = 0.0f;                    // vertical velocity
    bool  on_ground = true;
    float health = PLAYER_MAX_HEALTH;      // clamps at 0 (no death screen yet)
    // combat: attack_damage/knockback used when striking, weight resists incoming knockback
    Stats stats = { PLAYER_MAX_HEALTH, 12.0f, 10.0f, 5.0f };
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
