#pragma once
#include <cglm/cglm.h>
#include <optional>
#include "engine/world/map.h"
#include "engine/world/terrain.h"
#include "engine/entity/entity.h"

namespace dc::entity {

inline constexpr float PLAYER_RADIUS = 0.4f;   // world units
inline constexpr float MAX_STEP_UP   = 0.6f;   // max terrain rise (ahead) you can walk up; steeper = a wall
inline constexpr float STEP_LOOKAHEAD = 0.8f;  // how far ahead to sample the rise (so cliffs block at the foot)
inline constexpr float GRAVITY       = 15.0f;  // units/s^2 (a bit floaty)
inline constexpr float JUMP_SPEED    = 9.5f;   // units/s (~2.5x the old apex height; height ~ v^2)
inline constexpr float MOVE_SPEED    = 5.5f;   // units/s (faster, snappier traversal)
inline constexpr float RUN_SPEED     = 7.0f;   // units/s (hold Shift)
inline constexpr float RUN_STAMINA_PER_SEC = 12.0f;  // stamina drained while sprinting
inline constexpr float JUMP_STAMINA  = 12.0f;  // stamina spent per jump

// Eye must sit below the head-bonk ceiling, or the vertical clamps would fight.
static_assert(dc::world::EYE_HEIGHT < dc::world::WALL_HEIGHT - 0.2f,
              "EYE_HEIGHT must be below the ceiling clamp");

inline constexpr float PLAYER_MAX_HEALTH = 100.0f;

// A shield's defensive stats. A frontal block negates damage by SPENDING stamina:
// it costs block_rate stamina per point of damage. With enough stamina the hit is
// fully negated; if stamina runs out, the unaffordable remainder gets through at
// full strength (taken = shortfall_stamina / block_rate). block_cos is cos(half-angle)
// of the arc it covers (smaller cos = wider arc).
struct Shield {
    float block_rate      = 0.5f;   // stamina spent per point of damage blocked
    float block_cos       = -1.0f;  // omnidirectional: a full bubble blocks hits from any angle
    float block_speed     = 2.0f;   // raise-animation playback multiplier (lower = slower to ready)
    float stamina_per_sec = 8.0f;   // drained per second while the shield is up
    // Shield-bash nova (3): a white sphere expands from you, knocking everything it
    // sweeps far back for light damage. Very high knockback, slow cooldown.
    float bash_radius     = 4.5f;   // how far the shockwave reaches
    float bash_damage     = 26.0f;  // hits hard now
    float bash_knockback  = 85.0f;  // even bigger shove
    float bash_duration   = 0.35f;  // seconds for the sphere to expand fully (quick)
    float bash_cooldown   = 8.0f;   // very slow
    float stamina_per_bash = 55.0f;
};

// A weapon's offensive stats. Its attack_bonus is ADDED to the player's base
// (unarmed) attack; reach/cone define the swing arc — every enemy inside is hit.
struct Weapon {
    float attack_bonus     = 10.0f;  // added to the player's base attack_damage
    float reach            = 7.5f;   // swing reach (world units) — 1.5x longer poke by default
    float cone_cos         = 0.80f;  // arccos(.80) ~37 deg half-arc — skinny + long
    float attack_speed     = 1.4f;   // swing-animation playback multiplier (faster swing)
    float cooldown         = 0.083f; // seconds after a swing before the next is allowed (~1/3 of before)
    float stamina_per_swing = 20.0f; // stamina spent per swing
    // Thrown special (MMB): the sword detaches, flies `throw_distance` out, then
    // boomerangs back. Bigger cost/cooldown, more damage than a swing.
    float throw_distance    = 8.0f;  // outbound distance before it returns
    float throw_speed       = 14.0f; // flight speed (units/s)
    float throw_radius      = 0.9f;  // hit radius while flying
    float throw_damage      = 25.0f; // damage per enemy hit
    float throw_cooldown    = 2.0f;  // longer cooldown for the special (~2x a basic swing gap)
    float stamina_per_throw = 35.0f; // stamina spent to throw
    float throw_size        = 1.0f;  // size multiplier for the thrown sword (scales hit radius too)
    // Orbit special (2): summon `orbit_count` spinning swords circling you for a
    // duration, damaging enemies they sweep. Long cooldown, but hits hard.
    int   orbit_count       = 3;
    float orbit_radius      = 1.9f;  // how far the swords circle from the player
    float orbit_duration    = 2.5f;  // seconds they stay
    float orbit_damage      = 30.0f; // damage per hit-tick (strong — earns the 5x cooldown)
    float orbit_hit_radius  = 0.85f; // each sword's hit radius
    float orbit_cooldown    = 5.0f;  // ~5x the throw cooldown
    float stamina_per_orbit = 60.0f; // a lot
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
    float health_regen = 4.5f;             // hp/sec while alive (1.5x base; upgradeable)
    // combat: attack_damage/knockback used when striking, weight resists incoming knockback
    Stats stats = { PLAYER_MAX_HEALTH, 5.0f, 10.0f, 5.0f };  // attack_damage = base/unarmed
    float stamina       = 100.0f;
    float stamina_max   = 100.0f;
    float stamina_regen = 18.0f;               // per second, while not blocking
    // Upgrade modifiers (from chest cards). knockback upgrade bumps stats.knockback.
    float stamina_mult       = 0.5f;           // base: every action costs half endurance (green upgrade cheapens further)
    float damage_mult        = 1.0f;           // red: scales melee + throw damage
    float swing_reach_bonus  = 0.0f;           // blue: + melee reach
    float swing_cone_bonus   = 0.0f;           // blue: widens the swing arc (subtract from cos)
    float sword_scale        = 1.0f;           // blue: also scales the sword model visually
    float cooldown_mult      = 1.0f;           // cyan: scales every ability cooldown (<1 faster)
    float crit_chance        = 0.05f;          // gold: base 5% chance a melee strike crits
    float crit_mult          = 2.0f;           // magenta: crit damage multiplier
    // Elemental sword brands (0 / 1.0 = not equipped). Each adds a sword particle stream.
    float fire_dps           = 0.0f;           // fire: burn damage/sec applied on hit
    float fire_duration      = 3.0f;           // how long the burn lasts
    float ice_slow           = 1.0f;           // ice: enemy move multiplier on hit (<1 slows)
    float ice_duration       = 2.5f;           // how long the slow lasts
    float earth_knock        = 0.0f;           // earth: extra knockback on hit
    int   minion_count       = 0;              // gunner minions orbiting you (cap 4)
    float minion_damage      = 5.0f;           // damage per minion volley shot
    float minion_range       = 18.0f;          // how far minions acquire + shoot targets (upgradeable)
    float trail_damage       = 0.0f;           // trailblazer: fire-trail damage/sec (0 = not equipped)
    float trail_life         = 0.0f;           // how long each trail segment burns (upgradable decay)
    float supersonic_damage  = 0.0f;           // supersonic: dodge shockwave damage (0 = not equipped)
    std::optional<Shield> shield = Shield{};   // equipped shield (nullopt = none)
    std::optional<Weapon> weapon = Weapon{};   // equipped weapon (nullopt = fists)
    vec3  knock_vel = {0.0f, 0.0f, 0.0f};  // horizontal knockback velocity (decays in update)
    float hit_flash = 0.0f;                // red-flash timer (decayed by main)
    float burn_time = 0.0f;                // on fire: seconds of burn DoT remaining (flamethrower)
    float burn_dps  = 0.0f;                // burn damage per second while burn_time > 0
    // Dodge-dash (Shift): a burst of velocity with brief invincibility. All upgradeable.
    float dash_speed    = 24.0f;   // initial burst speed (units/s)
    float dash_decay    = 8.0f;    // how fast the burst tapers (1/s; higher = shorter dash)
    float dash_iframes  = 0.35f;   // seconds of invincibility granted
    float dash_cost     = 25.0f;   // stamina per dash
    float dash_cooldown = 0.7f;    // seconds between dashes
    vec3  dash_vel  = {0.0f, 0.0f, 0.0f};  // current dash velocity (decays in update)
    float iframes   = 0.0f;        // invincible while > 0 (counts down in update)
    float dash_cd   = 0.0f;        // time until the next dash is allowed (counts down in update)

    // Look direction unit vector from yaw/pitch.
    void front(vec3 out) const;
    // Mouse delta (pixels): yaw += dx*sens, pitch -= dy*sens, clamp pitch.
    void add_look(float dx, float dy);
    // forward/strafe in {-1,0,1}; jump=true attempts a jump this frame.
    // Horizontal motion slides against the map's walls; vertical applies gravity/jump
    // and lands on the terrain height under the player (open-top, no ceiling).
    void update(float forward, float strafe, bool jump, float dt,
                const dc::world::Map& map, const dc::world::Terrain& terrain);
};

} // namespace dc::entity
