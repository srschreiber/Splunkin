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
enum class BuildPiece : uint8_t { Barricade = 0, Landmine = 1, Turret = 2, Barracks = 3, Count = 4 };

// Gold cost of PLACING one piece (refund is half on removal).
inline int piece_cost(BuildPiece p) {
    switch (p) {
        case BuildPiece::Barricade: return 15;
        case BuildPiece::Landmine:  return 20;
        case BuildPiece::Turret:    return 50;
        case BuildPiece::Barracks:  return 80;
        default:                    return 15;
    }
}

inline const char* piece_name(BuildPiece p) {
    switch (p) {
        case BuildPiece::Barricade: return "Barricade";
        case BuildPiece::Landmine:  return "Landmine";
        case BuildPiece::Turret:    return "Turret";
        case BuildPiece::Barracks:  return "Barracks";
        default:                    return "?";
    }
}

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
struct MobType {
    const char* name;
    int   unlock_cost;   // one-time gold to unlock this barracks type (0 = free from the start)
    int   place_cost;    // gold to build one barracks of this type
    int   spawn_cost;    // gold per mob spawned
    float interval;      // seconds between spawns
    float hp, damage;
    bool  scavenger;     // true: collects coins instead of pushing the lane
};
inline constexpr int MOB_TYPE_COUNT = 4;
inline const MobType& mob_type(int i) {
    // Barracks are a one-time PURCHASE (place_cost) that then spawn their mob for FREE on a
    // timer — so spawn_cost is 0 and stronger types cost more to buy.
    static const MobType T[MOB_TYPE_COUNT] = {
        { "Grunt",     0,    100,  0, 3.0f,  90.0f, 18.0f, false },
        { "Soldier",   180,  250,  0, 4.0f, 190.0f, 34.0f, false },
        { "Brute",     450,  500,  0, 6.0f, 430.0f, 62.0f, false },
        { "Scavenger", 250,  200,  0, 4.5f, 150.0f, 12.0f, true  },
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
