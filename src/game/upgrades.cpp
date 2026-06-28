#include "game/upgrades.h"

const UpgradeDef& upgrade_def(Upgrade u) {
    static const UpgradeDef defs[UPGRADE_COUNT] = {
        { "Conditioning", "Stamina costs -15% per stack.",                    IconShape::Boot,     0.20f, 0.85f, 0.30f },  // green
        { "Heavy Hitter", "+4 knockback per stack.",                          IconShape::Hammer,   0.90f, 0.80f, 0.15f },  // yellow
        { "Whetstone",    "+25% attack damage per stack.",                    IconShape::Sword,    0.85f, 0.20f, 0.20f },  // red
        { "Long Reach",   "Longer, wider swing and a bigger blade per stack.", IconShape::Crescent,0.25f, 0.50f, 1.00f },  // blue
        { "Adrenaline",   "All ability cooldowns -15% per stack.",            IconShape::Clock,    0.20f, 0.85f, 0.90f },  // cyan
        { "Keen Edge",    "+8% melee crit chance per stack.",                 IconShape::Star,     1.00f, 0.85f, 0.20f },  // gold
        { "Deathblow",    "+0.5x melee crit damage per stack.",               IconShape::Fang,     0.95f, 0.20f, 0.60f },  // magenta
        { "Regeneration", "+1 health/sec per stack.",                         IconShape::Heart,    0.95f, 0.45f, 0.55f },  // rose
        { "Ember Brand",  "Strikes set enemies ablaze; +burn damage per stack.", IconShape::Flame,    1.00f, 0.45f, 0.10f },  // fire
        { "Frost Brand",  "Strikes slow enemies; stronger slow per stack.",    IconShape::IceShard, 0.40f, 0.80f, 1.00f },  // ice
        { "Stone Brand",  "Strikes knock enemies back harder per stack.",      IconShape::Brick,    0.60f, 0.42f, 0.22f },  // earth
        { "Gun Drone",    "A gunner minion that shoots enemies (up to 4).",    IconShape::Drone,    0.60f, 0.72f, 0.88f },  // steel
        { "Munitions",    "+4 minion damage per stack.",                       IconShape::Ammo,     0.85f, 0.70f, 0.30f },  // brass
        { "Trailblazer",  "Leave a burning trail as you run; +damage & longer burn per stack.", IconShape::Streak, 1.00f, 0.40f, 0.10f },  // ember
        { "Supersonic",   "Dodging breaks the sound barrier: knocks back + hurts nearby enemies (+dmg per stack).", IconShape::Boom, 0.85f, 0.90f, 1.00f },  // shockwave white
        { "Drone Sensors","+6 minion targeting range per stack.",             IconShape::Drone,    0.35f, 0.90f, 0.55f },  // green (drone)
        // --- level-up only (gated) ---
        { "Orbit Blades", "Unlock the Orbit autocast (uses a spell slot).",    IconShape::Ring,     0.30f, 0.85f, 0.95f },  // cyan autocast
        { "Force Nova",   "Unlock the Force Nova autocast (uses a spell slot).",IconShape::Burst,    0.85f, 0.90f, 1.00f },  // shockwave white
        { "Attunement",   "+1 spell slot (hold another autocast).",            IconShape::Slot,     0.70f, 0.55f, 0.95f },  // violet
        { "More Blades",  "+1 sword in your orbit ring.",                      IconShape::Ring,     0.40f, 0.90f, 1.00f },  // orbit
        { "Orbit Tempo",  "Orbit spins faster and re-hits sooner.",            IconShape::Ring,     0.20f, 0.95f, 0.80f },  // orbit
        { "Orbit Cycle",  "Orbit autocast recharges 15% faster per stack.",    IconShape::Ring,     0.25f, 0.70f, 0.95f },  // orbit
        { "Wider Nova",   "+1 Force Nova blast radius per stack.",             IconShape::Burst,    0.80f, 0.85f, 1.00f },  // nova
        { "Nova Cycle",   "Force Nova recharges 15% faster per stack.",        IconShape::Burst,    0.70f, 0.80f, 1.00f },  // nova
        { "Overclock",    "All autocasts recharge 9% faster per stack.",       IconShape::Slot,     0.45f, 0.75f, 1.00f },  // global
        { "Swordstorm",   "Throw an extra sword at once (up to 3).",           IconShape::Streak,   0.90f, 0.85f, 0.95f },  // throw
        { "Twin Bolts",   "Your staff fires +1 bolt per shot (up to 4).",      IconShape::IceShard, 0.45f, 0.62f, 1.00f },  // wizard primary
        { "Rapid Cast",   "Staff fires 15% faster per stack.",                 IconShape::Clock,    0.40f, 0.70f, 1.00f },  // wizard fire-rate
    };
    return defs[static_cast<int>(u)];
}

void apply_upgrade(dc::entity::Player& p, Upgrade u) {
    switch (u) {
        case Upgrade::StaminaCost: p.stamina_mult *= 0.85f;       break;  // green: -15% costs
        case Upgrade::Knockback:   p.stats.knockback += 4.0f;     break;  // yellow
        case Upgrade::Damage:      p.damage_mult += 0.25f;        break;  // red: +25% dmg
        case Upgrade::SwingArc:    p.swing_reach_bonus += 0.5f;           // blue: longer + wider swing
                                   p.swing_cone_bonus  += 0.12f;
                                   p.sword_scale       += 0.2f;   break;  // + a bigger blade
        case Upgrade::Cooldown:    p.cooldown_mult *= 0.85f;      break;  // cyan: -15% cooldowns (diminishing)
        case Upgrade::CritChance:    p.crit_chance += 0.08f;     break;  // gold: +8% crit chance
        case Upgrade::CritDamage:    p.crit_mult += 0.5f;        break;  // magenta: +0.5x crit damage
        case Upgrade::Regen:         p.health_regen += 1.0f;     break;  // rose: +1 hp/sec
        case Upgrade::Fire:  p.fire_dps += 6.0f;                          break;  // fire: +burn dps
        case Upgrade::Ice:   p.ice_slow = (p.ice_slow - 0.15f < 0.35f) ? 0.35f : p.ice_slow - 0.15f; break;  // ice: deeper slow
        case Upgrade::Earth: p.earth_knock += 5.0f;                       break;  // earth: +knockback
        case Upgrade::Gunner:    if (p.minion_count < 4) p.minion_count++; break;  // +1 gunner minion (cap 4)
        case Upgrade::Munitions: p.minion_damage += 4.0f;                 break;  // +minion damage
        case Upgrade::Trailblazer: p.trail_damage += 8.0f; p.trail_life += 0.8f; break;  // fire trail: +dmg, longer burn
        case Upgrade::Supersonic:  p.supersonic_damage += 14.0f;          break;  // dodge shockwave damage
        case Upgrade::DroneRange:  p.minion_range += 6.0f;                break;  // +drone targeting range
        // --- level-up only (gated) ---
        case Upgrade::UnlockOrbit:      p.orbit_unlocked = true;              break;  // occupies a spell slot
        case Upgrade::UnlockForcefield: p.forcefield_unlocked = true;         break;  // occupies a spell slot
        case Upgrade::ExtraSpellSlot:   if (p.spell_slots < dc::entity::SPELL_SLOTS_MAX) p.spell_slots++; break;
        case Upgrade::OrbitSword:       if (p.weapon && p.weapon->orbit_count < dc::entity::ORBIT_COUNT_MAX) p.weapon->orbit_count++; break;
        case Upgrade::OrbitTempo:       p.orbit_spin_mult += 0.30f; p.orbit_tick_mult *= 0.82f; break;  // faster spin + re-hit
        case Upgrade::OrbitCooldown:    p.orbit_cd_mult *= 0.85f;             break;  // targeted -15%
        case Upgrade::NovaRadius:       if (p.shield) p.shield->bash_radius += 1.0f; break;
        case Upgrade::NovaCooldown:     p.forcefield_cd_mult *= 0.85f;        break;  // targeted -15%
        case Upgrade::AutocastHaste:    p.autocast_cd_mult *= (1.0f - 0.15f * 0.6f); break;  // global: 3/5 of targeted
        case Upgrade::MultiThrow:       if (p.throw_count < dc::entity::THROW_MAX) p.throw_count++; break;
        case Upgrade::MultiBolt:        if (p.bolt_count < 4) p.bolt_count++; break;      // +1 staff bolt per shot (cap 4)
        case Upgrade::BoltHaste:        p.bolt_cd_mult *= 0.85f; break;                  // faster primary fire
    }
}

bool upgrade_eligible(const dc::entity::Player& p, Upgrade u) {
    const int used_slots = (p.orbit_unlocked ? 1 : 0) + (p.forcefield_unlocked ? 1 : 0);
    const bool free_slot = used_slots < p.spell_slots;
    switch (u) {
        // Unlock an autocast only if it's still locked AND a slot is free to hold it.
        case Upgrade::UnlockOrbit:      return !p.orbit_unlocked && free_slot;
        case Upgrade::UnlockForcefield: return !p.forcefield_unlocked && free_slot;
        case Upgrade::ExtraSpellSlot:   return p.spell_slots < dc::entity::SPELL_SLOTS_MAX;
        // Orbit upgrades need orbit unlocked; nova upgrades need nova unlocked.
        case Upgrade::OrbitSword:       return p.orbit_unlocked && p.weapon && p.weapon->orbit_count < dc::entity::ORBIT_COUNT_MAX;
        case Upgrade::OrbitTempo:
        case Upgrade::OrbitCooldown:    return p.orbit_unlocked;
        case Upgrade::NovaRadius:
        case Upgrade::NovaCooldown:     return p.forcefield_unlocked;
        case Upgrade::AutocastHaste:    return p.orbit_unlocked || p.forcefield_unlocked;
        case Upgrade::MultiThrow:       return p.weapon.has_value() && p.throw_count < dc::entity::THROW_MAX;
        case Upgrade::MultiBolt:        return p.bolt_count < 4;   // up to 4 staff bolts per shot
        case Upgrade::BoltHaste:        return true;               // always offer faster casting (wizard-gated below)
        // Drone upgrades require owning a drone; getting the first comes from Gunner.
        case Upgrade::Gunner:           return p.minion_count < 4;
        case Upgrade::Munitions:
        case Upgrade::DroneRange:       return p.minion_count > 0;
        // Everything else (core melee/dodge/elemental) is always eligible.
        default:                        return true;
    }
}

bool upgrade_for_class(Upgrade u, uint8_t weapon_class) {
    if (u == Upgrade::MultiBolt || u == Upgrade::BoltHaste) return weapon_class == 1;   // wizard-only staff upgrades
    if (weapon_class != 1) return true;   // Knight (sword): everything is fair game
    // Wizard (staff): exclude the sword-swing / melee-on-hit / sword-throw upgrades. Its
    // offense comes from staff bolts (scaled by Whetstone damage + Adrenaline cooldown +
    // crit), plus the reskinned autocasts (orbiting orbs + a magic knockback wave).
    switch (u) {
        case Upgrade::SwingArc:    // Long Reach (sword swing arc/blade)
        case Upgrade::Knockback:   // Heavy Hitter (melee shove)
        case Upgrade::Fire:        // Ember Brand (melee-strike brand)
        case Upgrade::Ice:         // Frost Brand
        case Upgrade::Earth:       // Stone Brand
        case Upgrade::OrbitSword:  // More Blades (+1 SWORD in the orbit)
        case Upgrade::MultiThrow:  // Swordstorm (extra thrown SWORD)
        case Upgrade::CritChance:  // Keen Edge — melee crit only (bolts don't crit)
        case Upgrade::CritDamage:  // Deathblow — melee crit only
            return false;
        default:
            return true;
    }
}

uint8_t elem_mask(float fire_dps, float ice_slow, float earth_knock) {
    uint8_t m = 0;
    if (fire_dps > 0.0f)    m |= 1;
    if (ice_slow < 1.0f)    m |= 2;
    if (earth_knock > 0.0f) m |= 4;
    return m;
}
