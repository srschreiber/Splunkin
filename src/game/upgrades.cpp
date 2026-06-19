#include "game/upgrades.h"

const UpgradeDef& upgrade_def(Upgrade u) {
    static const UpgradeDef defs[UPGRADE_COUNT] = {
        { "Conditioning", "Stamina costs -15% per stack.",                    IconShape::Square,   0.20f, 0.85f, 0.30f },  // green
        { "Heavy Hitter", "+4 knockback per stack.",                          IconShape::Triangle, 0.90f, 0.80f, 0.15f },  // yellow
        { "Whetstone",    "+25% melee damage per stack.",                     IconShape::Diamond,  0.85f, 0.20f, 0.20f },  // red
        { "Long Reach",   "Longer, wider swing and a bigger blade per stack.", IconShape::Circle,  0.25f, 0.50f, 1.00f },  // blue
        { "Adrenaline",   "All ability cooldowns -15% per stack.",            IconShape::Cross,    0.20f, 0.85f, 0.90f },  // cyan
        { "Lunge",        "Dodge dashes farther per stack.",                  IconShape::Arrow,    1.00f, 0.55f, 0.15f },  // orange
        { "Afterimage",   "+0.12s dodge invincibility per stack.",            IconShape::Hourglass,0.70f, 0.50f, 0.95f },  // violet
        { "Keen Edge",    "+8% melee crit chance per stack.",                 IconShape::Star,     1.00f, 0.85f, 0.20f },  // gold
        { "Deathblow",    "+0.5x melee crit damage per stack.",               IconShape::Pentagon, 0.95f, 0.20f, 0.60f },  // magenta
        { "Regeneration", "+1 health/sec per stack.",                         IconShape::Hexagon,  0.95f, 0.45f, 0.55f },  // rose
        { "Ember Brand",  "Strikes set enemies ablaze; +burn damage per stack.", IconShape::Flame,    1.00f, 0.45f, 0.10f },  // fire
        { "Frost Brand",  "Strikes slow enemies; stronger slow per stack.",    IconShape::IceShard, 0.40f, 0.80f, 1.00f },  // ice
        { "Stone Brand",  "Strikes knock enemies back harder per stack.",      IconShape::Brick,    0.60f, 0.42f, 0.22f },  // earth
        { "Gun Drone",    "A gunner minion that shoots enemies (up to 4).",    IconShape::Pip,      0.60f, 0.72f, 0.88f },  // steel
        { "Munitions",    "+4 minion damage per stack.",                       IconShape::Ammo,     0.85f, 0.70f, 0.30f },  // brass
        { "Trailblazer",  "Leave a burning trail as you run; +damage & longer burn per stack.", IconShape::Streak, 1.00f, 0.40f, 0.10f },  // ember
        { "Supersonic",   "Dodging breaks the sound barrier: knocks back + hurts nearby enemies (+dmg per stack).", IconShape::Boom, 0.85f, 0.90f, 1.00f },  // shockwave white
        { "Drone Sensors","+6 minion targeting range per stack.",             IconShape::Pip,      0.35f, 0.90f, 0.55f },  // green (drone)
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
        case Upgrade::DodgeDistance: p.dash_speed += 6.0f;       break;  // orange: faster -> farther dash
        case Upgrade::DodgeIframes:  p.dash_iframes += 0.12f;    break;  // violet: longer i-frame window
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
    }
}

uint8_t elem_mask(float fire_dps, float ice_slow, float earth_knock) {
    uint8_t m = 0;
    if (fire_dps > 0.0f)    m |= 1;
    if (ice_slow < 1.0f)    m |= 2;
    if (earth_knock > 0.0f) m |= 4;
    return m;
}
