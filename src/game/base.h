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
enum class BuildPiece : uint8_t { Barricade = 0, Landmine = 1, Turret = 2, Barracks = 3, Water = 4, Vacuum = 5, SubPen = 6, Shipyard = 7, Mortar = 8, Count = 9 };

// MORTAR (base artillery): a costly, very-slow siege piece that lobs shells far down the lane
// (out to ~mid-map) and detonates in a big AoE for massive damage. Counters enemy blobs at range.
inline constexpr float MORTAR_RANGE      = 130.0f;  // reaches roughly to the middle of the map
inline constexpr float MORTAR_MIN_RANGE  = 14.0f;   // can't hit point-blank (lob arcs over the near area)
inline constexpr float MORTAR_BLAST      = 6.0f;    // splash radius
inline constexpr float MORTAR_DAMAGE     = 320.0f;  // massive (clears blobs)
inline constexpr float MORTAR_CD         = 7.0f;    // seconds between shots — VERY slow
inline constexpr float MORTAR_SHELL_TIME = 1.7f;    // shell flight time (arc)
inline constexpr float MORTAR_HP         = 360.0f;

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
        case BuildPiece::Mortar:    return 400;   // costly siege artillery
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
        case BuildPiece::Mortar:    return "Mortar";
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

// Naval support ships (the only source of SEA MINES now). A MINELAYER wanders the river dropping
// mines that blow up the OTHER team's warships (enough of them grind down a battleship). A
// MINESWEEPER is a cheap kamikaze rowboat that drives into the enemy's mines to clear them.
inline constexpr float MINELAYER_HP        = 600.0f;
inline constexpr float MINELAYER_SPEED     = 2.2f;
inline constexpr float MINELAYER_DROP_CD   = 6.0f;    // seconds between mines laid
inline constexpr int   MINE_CAP_PER_TEAM   = 18;      // a team's mines stop arming past this
inline constexpr float MINE_DAMAGE         = 900.0f;  // per mine — warships have ~7000 HP, so it takes several
inline constexpr float MINE_BLAST_R        = 3.2f;
inline constexpr float MINESWEEPER_HP      = 140.0f;  // glass: it's meant to die ON a mine
inline constexpr float MINESWEEPER_SPEED   = 3.2f;    // fast little rowboat

// Water pool slow: anyone standing in a water tile moves at this fraction of speed (and can't jump).
inline constexpr float WATER_SLOW = 0.45f;
// Vacuum: slowly drags coins + XP orbs toward the base core within its range.
inline constexpr float VACUUM_PULL_SPEED = 5.5f;   // world units/sec the loot drifts toward the core

// Friendly-mob (lane unit) shared tuning.
inline constexpr int   ALLY_CAP       = 90;    // hard ceiling on friendly mobs (per-type caps gate below this)
inline constexpr float ALLY_SPEED     = 4.2f;  // march/charge speed
inline constexpr float ALLY_REACH     = 2.0f;  // melee reach
inline constexpr float ALLY_ATTACK_CD = 0.8f;  // seconds between swings
inline constexpr float ALLY_AGGRO     = 16.0f; // engages enemies within this range

// Barracks MOB TYPES. Each barracks is one type (stored in the piece's `rot`); you unlock a
// type once (unlock_cost), pay place_cost to build a barracks of it, and it then spawns its mob
// every `interval` seconds for `spawn_cost` gold each. The Scavenger doesn't fight the lane —
// it roams collecting dropped coins into the shared pool (with decent HP for self-defense).
// Visual/behaviour class of a friendly mob — mirrors the enemy roster.
enum class MobVisual : uint8_t { Ground = 0, Scavenger = 1, Flier = 2, Bat = 3, Demon = 4, Mage = 5, Insulter = 6, Knight = 7, Flame = 8, Troll = 9, Slime = 10, Drone = 11 };
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
    int   cap;           // how many of THIS mob one barracks keeps alive at once (weak swarmers = high)
};
inline constexpr int MOB_TYPE_COUNT = 13;
inline const MobType& mob_type(int i) {
    // Barracks are a one-time PURCHASE (place_cost) that then spawn their mob for FREE on a
    // timer. The roster MIRRORS the enemy types (grunt=skeleton, mage=ranged caster, bat,
    // flier=eye, demon, bill=insulter with a friendly attack-weakening aura), plus the top-tier
    // MOUNTED KNIGHT (a single horse+rider model: very tanky, hits hard, but heavy and slow).
    static const MobType T[MOB_TYPE_COUNT] = {
        { "Grunt",     0,    100,  0, 3.0f,  90.0f, 18.0f, 2.0f, false, MobVisual::Ground,    false, 1.00f, 10 },
        { "Mage",      150,  150,  0, 4.0f, 110.0f, 26.0f, 9.0f, false, MobVisual::Mage,      false, 1.00f,  7 },
        { "Brute",     250,  250,  0, 6.0f, 430.0f, 62.0f, 2.2f, false, MobVisual::Ground,    false, 0.90f,  4 },
        { "Bat",       80,   100,  0, 3.0f,  55.0f, 12.0f, 2.0f, false, MobVisual::Bat,       true,  1.20f, 20 },
        { "Flier",     120,  90,   0, 4.0f,  70.0f, 16.0f, 9.0f, false, MobVisual::Flier,     true,  1.10f, 10 },
        { "Demon",     500,  500,  0, 8.0f, 800.0f, 80.0f, 9.0f, false, MobVisual::Demon,     false, 1.00f,  3 },
        { "Bill",      200,  200,  0, 11.0f, 1200.0f, 4.0f, 2.0f, false, MobVisual::Insulter,  false, 1.00f,  2 },
        { "Scavenger", 50,   100,  0, 13.5f, 150.0f, 12.0f, 2.0f, true,  MobVisual::Scavenger, false, 1.00f,  4 },
        { "Knight",    700,  700,  0, 10.0f, 1600.0f, 90.0f, 2.8f, false, MobVisual::Knight,   false, 0.72f,  2 },
        { "Gnome",     0,    150,  0, 5.0f,  200.0f, 30.0f, 3.5f, false, MobVisual::Flame,    false, 1.00f,  6 },
        { "Troll",     0,    300,  0, 7.0f,  600.0f, 70.0f, 2.2f, false, MobVisual::Troll,    false, 0.75f,  4 },
        { "Slime",     0,    130,  0, 4.5f,  550.0f, 14.0f, 2.0f, false, MobVisual::Slime,    false, 0.85f,  6 },
        { "Drone",     100,  120,  0, 1.4f,  10.0f,  5.0f,  2.0f, false, MobVisual::Drone,    true,  1.30f, 15 },   // cheap low-HP swarm to distract; cap 15
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
// For a BARRACKS, `up[]` holds per-stat upgrade LEVELS that buff only this barracks' troops:
// up[0]=HP, up[1]=DEF, up[2]=SPEED, up[3]=SPAWN-RATE, up[4]=CAPACITY (more alive at once).
inline constexpr int BARRACKS_UP_STATS = 5;
struct BasePiece {
    int16_t col = 0, row = 0;
    uint8_t piece = 0;   // BuildPiece
    uint8_t rot = 0;     // 0..3 (or, for a barracks, the mob TYPE; for a shipyard, the boat type)
    uint8_t up[BARRACKS_UP_STATS] = {0,0,0,0,0};   // HP / DEF / SPEED / RATE / CAPACITY
};

// Barracks upgrades: each stat can be bought up to this many times; cost scales with the level.
inline constexpr int   BARRACKS_UP_MAX = 5;
inline int   barracks_upgrade_cost(int stat_level) { return 60 + stat_level * 50; }   // 60,110,160,...
inline float barracks_hp_mult(int lvl)   { return 1.0f + 0.30f * lvl; }   // +30% HP per level
inline float barracks_def_mult(int lvl)  { float m = 1.0f; for (int i=0;i<lvl;++i) m *= 0.85f; return m; }  // damage TAKEN ×0.85^lvl
inline float barracks_spd_mult(int lvl)  { return 1.0f + 0.15f * lvl; }   // +15% move speed per level
inline float barracks_rate_mult(int lvl) { return 1.0f + 0.22f * lvl; }   // spawns this much faster per level
inline int   barracks_cap_bonus(int lvl) { return lvl * 4; }              // +4 to the active cap per level
inline int   barracks_up_total(const BasePiece& p) { int t=0; for (int s=0;s<BARRACKS_UP_STATS;++s) t+=p.up[s]; return t; }
inline constexpr int BARRACKS_CAP_BASE = 5;   // base barracks capacity (grows with area expansions)

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

// How many BARRACKS the base can hold: 5, +1 for each buildable-area expansion bought.
inline int barracks_capacity(float radius) {
    return BARRACKS_CAP_BASE + static_cast<int>((radius - BASE_AREA_START) / BASE_AREA_STEP + 0.5f);
}

// The whole persisted base: buildable radius + every placed piece (turrets included).
struct BaseSave {
    float                  build_radius = BASE_AREA_START;
    std::vector<BasePiece> pieces;
};

inline constexpr uint32_t BASE_MAGIC = 0xB0A5E004u;   // bumped: BasePiece up[] grew to 5 stats (added capacity)

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
