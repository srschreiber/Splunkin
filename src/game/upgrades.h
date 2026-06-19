#pragma once
#include <cstdint>
#include "engine/entity/player.h"

// Chest upgrades + their display catalog (placeholder icon shape, name, hover desc) and
// the per-pick effect. App-level gameplay data, kept out of main.cpp.

enum class Upgrade { StaminaCost, Knockback, Damage, SwingArc, Cooldown, DodgeDistance, DodgeIframes,
                     CritChance, CritDamage, Regen, Fire, Ice, Earth, Gunner, Munitions,
                     Trailblazer, Supersonic, DroneRange };
inline constexpr int UPGRADE_COUNT = 18;

// Placeholder icon shapes (real icon art comes later). One per upgrade so the stacked
// items in the top-left HUD are visually distinct. Rendered by main's icon_shape/bb_shape.
enum class IconShape { Square, Triangle, Diamond, Circle, Cross, Arrow, Hourglass, Star, Pentagon, Hexagon,
                       Flame, IceShard, Brick, Pip, Ammo, Streak, Boom };

struct UpgradeDef {
    const char* name;        // short title (shown on the card + tooltip header)
    const char* desc;        // what ONE stack does (hover description)
    IconShape   shape;
    float       r, g, b;     // icon / card color
};

// Catalog entry for an upgrade (name/desc/icon).
const UpgradeDef& upgrade_def(Upgrade u);
// Apply one pick to the player (stacks additively/multiplicatively).
void apply_upgrade(dc::entity::Player& p, Upgrade u);
// Equipped elemental-brand bitmask (1=fire, 2=ice, 4=earth) — drives the sword particles.
uint8_t elem_mask(float fire_dps, float ice_slow, float earth_knock);
