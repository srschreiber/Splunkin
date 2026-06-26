#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

// The player-built BASE: placed building pieces (floors / walls / stairs / doors) plus the
// two things you buy to grow it — buildable AREA (a radius around the core) and the number
// of perimeter TURRETS. The base PERSISTS across runs (saved to disk): gold spent on it and
// the layout you built are kept, even though your per-run gold wallet resets. Plain POD so it
// serializes trivially for both the save file and the network snapshot. App-level, not engine.

namespace dc::game {

// Build piece kinds — structures you place to defend and to PUSH THE LANE:
//  - Barricade: a destructible wall that blocks enemies and soaks damage (regens when idle).
//  - Landmine:  arms on the ground; the first enemy to step near it detonates an AoE blast.
//  - Turret:    an always-on auto-gun that shoots the nearest enemy in range.
//  - Barracks:  periodically spawns a FRIENDLY mob (each spawn costs gold) that marches the
//               lane to fight the enemy and attack their base.
// Keep Count last.
enum class BuildPiece : uint8_t { Barricade = 0, Landmine = 1, Turret = 2, Barracks = 3, Water = 4, Vacuum = 5, SubPen = 6, Shipyard = 7, Count = 8 };

// Gold cost of PLACING one piece (refund is half on removal).
inline int piece_cost(BuildPiece p) {
    switch (p) {
        case BuildPiece::Barricade: return 15;
        case BuildPiece::Landmine:  return 20;
        case BuildPiece::Turret:    return 50;
        case BuildPiece::Barracks:  return 80;
        case BuildPiece::Water:     return 25;
        case BuildPiece::Vacuum:    return 120;
        case BuildPiece::SubPen:    return 200;
        case BuildPiece::Shipyard:  return 220;
        default:                    return 15;
    }
}

inline const char* piece_name(BuildPiece p) {
    switch (p) {
        case BuildPiece::Barricade: return "Barricade";
        case BuildPiece::Landmine:  return "Landmine";
        case BuildPiece::Turret:    return "Turret";
        case BuildPiece::Barracks:  return "Barracks";
        case BuildPiece::Water:     return "Water";
        case BuildPiece::Vacuum:    return "Vacuum";
        case BuildPiece::SubPen:    return "Sub Pen";
        case BuildPiece::Shipyard:  return "Shipyard";
        default:                    return "?";
    }
}

// Submarine tuning. A friendly sub hunts enemy boats: it travels SUBMERGED (a periscope —
// invulnerable, can't be targeted) until in range, then SURFACES briefly to fire, then dives.
// The only counter to a sub is a landmine (or, conceptually, an enemy sub).
inline constexpr float SUBPEN_INTERVAL = 18.0f;   // seconds between sub launches per pen
inline constexpr int   SUB_CAP         = 6;       // max friendly subs at once
inline constexpr float SUB_SPEED       = 3.0f;
inline constexpr float SUB_RANGE       = 14.0f;   // surfaces + fires within this of a boat
inline constexpr float SUB_FIRE_CD     = 2.2f;
inline constexpr float SUB_DAMAGE      = 90.0f;
inline constexpr float SUB_MAX_HP      = 320.0f;  // a sub is only vulnerable while SURFACED (submerged = untouchable)

// Water pool slow: anyone standing in a water tile moves at this fraction of speed (and can't jump).
inline constexpr float WATER_SLOW = 0.45f;
// Vacuum: slowly drags coins + XP orbs toward the base core within its range.
inline constexpr float VACUUM_PULL_SPEED = 5.5f;   // world units/sec the loot drifts toward the core

// Friendly-mob (lane unit) shared tuning.
inline constexpr int   ALLY_CAP       = 24;    // max friendly mobs alive at once (all barracks)
inline constexpr float ALLY_SPEED     = 4.2f;  // march/charge speed
inline constexpr float ALLY_REACH     = 2.0f;  // melee reach
inline constexpr float ALLY_ATTACK_CD = 0.8f;  // seconds between swings
inline constexpr float ALLY_AGGRO     = 16.0f; // engages enemies within this range

// Barracks MOB TYPES. Each barracks is one type (stored in the piece's `rot`); you unlock a
// type once (unlock_cost), pay place_cost to build a barracks of it, and it then spawns its mob
// every `interval` seconds for `spawn_cost` gold each. The Scavenger doesn't fight the lane —
// it roams collecting dropped coins into the shared pool (with decent HP for self-defense).
// Visual/behaviour class of a friendly mob — mirrors the enemy roster.
enum class MobVisual : uint8_t { Ground = 0, Scavenger = 1, Flier = 2, Bat = 3, Demon = 4, Mage = 5, Insulter = 6, Knight = 7 };
struct MobType {
    const char* name;
    int   unlock_cost;   // one-time gold to unlock this barracks type (0 = free from the start)
    int   place_cost;    // gold to build one barracks of this type
    int   spawn_cost;    // gold per mob spawned (0 now — barracks spawn free)
    float interval;      // seconds between spawns
    float hp, damage;
    float reach;         // engage/strike range (big for ranged mage/flier so they zap from afar)
    bool  scavenger;     // true: collects coins instead of pushing the lane
    MobVisual visual;    // which model + behaviour
    bool  flies;         // hovers + ignores ground (fliers, bats)
    float speed;         // movement multiplier vs ALLY_SPEED (the Mounted Knight is heavy + slow)
};
inline constexpr int MOB_TYPE_COUNT = 9;
inline const MobType& mob_type(int i) {
    // Barracks are a one-time PURCHASE (place_cost) that then spawn their mob for FREE on a
    // timer. The roster MIRRORS the enemy types (grunt=skeleton, mage=ranged caster, bat,
    // flier=eye, demon, bill=insulter with a friendly attack-weakening aura), plus the top-tier
    // MOUNTED KNIGHT (a single horse+rider model: very tanky, hits hard, but heavy and slow).
    static const MobType T[MOB_TYPE_COUNT] = {
        { "Grunt",     0,    100,  0, 3.0f,  90.0f, 18.0f, 2.0f, false, MobVisual::Ground,    false, 1.00f },
        { "Mage",      150,  150,  0, 4.0f, 110.0f, 26.0f, 9.0f, false, MobVisual::Mage,      false, 1.00f },
        { "Brute",     250,  250,  0, 6.0f, 430.0f, 62.0f, 2.2f, false, MobVisual::Ground,    false, 0.90f },
        { "Bat",       80,   60,   0, 3.0f,  55.0f, 12.0f, 2.0f, false, MobVisual::Bat,       true,  1.20f },
        { "Flier",     120,  90,   0, 4.0f,  70.0f, 16.0f, 9.0f, false, MobVisual::Flier,     true,  1.10f },
        { "Demon",     500,  500,  0, 8.0f, 800.0f, 80.0f, 9.0f, false, MobVisual::Demon,     false, 1.00f },
        { "Bill",      200,  200,  0, 11.0f, 1200.0f, 4.0f, 2.0f, false, MobVisual::Insulter,  false, 1.00f },
        { "Scavenger", 50,   100,  0, 4.5f, 150.0f, 12.0f, 2.0f, true,  MobVisual::Scavenger, false, 1.00f },
        { "Knight",    700,  700,  0, 12.0f, 1600.0f, 90.0f, 2.6f, false, MobVisual::Knight,   false, 0.55f },
    };
    return T[(i < 0 || i >= MOB_TYPE_COUNT) ? 0 : i];
}

// Runtime tuning for the defensive pieces (per-run state, not persisted).
inline constexpr float BARRICADE_MAX_HP   = 350.0f;  // damage a barricade soaks before breaking
inline constexpr float BARRICADE_BLOCK_R  = 1.1f;    // enemies can't pass within this of its center
inline constexpr float BARRICADE_CHIP_DPS = 18.0f;   // HP an adjacent enemy chips off per second
inline constexpr float BARRICADE_REGEN    = 25.0f;   // HP/sec it repairs during the day
inline constexpr float LANDMINE_TRIGGER_R = 2.0f;    // an enemy this close trips the mine
inline constexpr float LANDMINE_BLAST_R   = 4.5f;    // explosion radius
inline constexpr float LANDMINE_DAMAGE    = 220.0f;  // explosion damage at center
inline constexpr float LANDMINE_KNOCK     = 30.0f;   // explosion knockback

// One placed piece, snapped to a tile. rot is 0..3 (×90° about Y).
struct BasePiece {
    int16_t col = 0, row = 0;
    uint8_t piece = 0;   // BuildPiece
    uint8_t rot = 0;     // 0..3
};

// Buildable-area growth + the cost curve for buying more area (turrets are bought by simply
// placing turret pieces, priced via piece_cost).
inline constexpr float BASE_AREA_START   = 11.0f;  // starting buildable radius (matches the shield dome)
inline constexpr float BASE_AREA_STEP    = 3.0f;   // radius gained per area purchase
inline constexpr float BASE_AREA_MAX     = 32.0f;  // cap

// Cost to buy the NEXT area expansion, scaling with how much you already own.
inline int base_area_cost(float radius) {
    const int steps = static_cast<int>((radius - BASE_AREA_START) / BASE_AREA_STEP + 0.5f);
    return 60 + steps * 40;   // 60, 100, 140, ...
}

// The whole persisted base: buildable radius + every placed piece (turrets included).
struct BaseSave {
    float                  build_radius = BASE_AREA_START;
    std::vector<BasePiece> pieces;
};

inline constexpr uint32_t BASE_MAGIC = 0xB0A5E002u;

inline bool save_base(const BaseSave& b, const char* path) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const uint32_t magic = BASE_MAGIC;
    std::fwrite(&magic, 4, 1, f);
    std::fwrite(&b.build_radius, sizeof b.build_radius, 1, f);
    const uint32_t n = static_cast<uint32_t>(b.pieces.size());
    std::fwrite(&n, 4, 1, f);
    if (n) std::fwrite(b.pieces.data(), sizeof(BasePiece), n, f);
    std::fclose(f);
    return true;
}

inline bool load_base(BaseSave& b, const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    uint32_t magic = 0;
    bool ok = std::fread(&magic, 4, 1, f) == 1 && magic == BASE_MAGIC;
    if (ok) ok = std::fread(&b.build_radius, sizeof b.build_radius, 1, f) == 1;
    uint32_t n = 0;
    if (ok && std::fread(&n, 4, 1, f) == 1 && n < 100000u) {
        b.pieces.resize(n);
        if (n) ok = std::fread(b.pieces.data(), sizeof(BasePiece), n, f) == n;
    }
    std::fclose(f);
    if (!ok) { b = BaseSave{}; return false; }   // corrupt -> fresh base
    return true;
}

} // namespace dc::game
