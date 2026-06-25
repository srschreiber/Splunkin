#pragma once
#include <cstdint>
#include "engine/entity/player.h"

// Chest upgrades + their display catalog (placeholder icon shape, name, hover desc) and
// the per-pick effect. App-level gameplay data, kept out of main.cpp.

// NOTE: order matters. The first CHEST_UPGRADE_COUNT entries are the "core" pool that
// chests roll from; the entries AFTER it are level-up-only (unlocks + autocast/throw
// upgrades) that are gated by `upgrade_eligible`. Appending keeps existing indices
// stable (chest seeds, inventory[], net casts all index by enum value).
enum class Upgrade { StaminaCost, Knockback, Damage, SwingArc, Cooldown, DodgeDistance, DodgeIframes,
                     CritChance, CritDamage, Regen, Fire, Ice, Earth, Gunner, Munitions,
                     Trailblazer, Supersonic, DroneRange,
                     // --- level-up only (gated) ---
                     UnlockOrbit, UnlockForcefield, ExtraSpellSlot,
                     OrbitSword, OrbitTempo, OrbitCooldown,
                     NovaRadius, NovaCooldown,
                     AutocastHaste, MultiThrow };
inline constexpr int UPGRADE_COUNT = 28;
inline constexpr int CHEST_UPGRADE_COUNT = 18;   // chests roll only the core [0,18) pool

// Placeholder icon shapes (real icon art comes later). One per upgrade so the stacked
// items in the top-left HUD are visually distinct. Rendered by main's icon_shape/bb_shape.
enum class IconShape { Square, Triangle, Diamond, Circle, Cross, Arrow, Hourglass, Star, Pentagon, Hexagon,
                       Flame, IceShard, Brick, Pip, Ammo, Streak, Boom, Ring, Burst, Slot,
                       // meaningful glyphs (drawn in main's icon_shape)
                       Sword, Heart, Clock, Hammer, Boot, Shield, Crescent, Drone, Fang };

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
// Whether `u` may appear as a level-up choice for this player right now. Gated
// upgrades (autocast/orbit/nova/drone) only become eligible once their prerequisite
// (an unlocked autocast, a free spell slot, an owned drone) is met.
bool upgrade_eligible(const dc::entity::Player& p, Upgrade u);
// Whether `u` is thematically appropriate for a class (weapon_class: 0=Knight/sword,
// 1=Wizard/staff). Wizards don't roll sword-swing / melee-brand / sword-throw upgrades.
bool upgrade_for_class(Upgrade u, uint8_t weapon_class);
// Equipped elemental-brand bitmask (1=fire, 2=ice, 4=earth) — drives the sword particles.
uint8_t elem_mask(float fire_dps, float ice_slow, float earth_knock);
