#pragma once
#include <cglm/cglm.h>
#include "engine/entity/entity.h"
#include "engine/world/map.h"
#include "engine/world/pathfind.h"

namespace dc::entity {

// Base XP an enemy of `kind` is worth when killed (the caller scales by run difficulty).
// Tougher kinds drop more so XP tracks roughly with how hard they are to take down.
// Rare "elite" variants: a small fraction of spawns roll into a bigger, golden, much
// tougher enemy that hits harder and (for shooters) fires bigger, deadlier projectiles.
inline constexpr float ELITE_CHANCE          = 0.06f;  // ~6% of spawns
inline constexpr float ELITE_SCALE           = 1.7f;   // render + presence size
inline constexpr float ELITE_HEALTH_MULT     = 3.0f;
inline constexpr float ELITE_DAMAGE_MULT     = 1.8f;
inline constexpr float ELITE_KNOCKBACK_MULT  = 1.5f;
inline constexpr float ELITE_WEIGHT_MULT     = 2.0f;   // harder to shove around
inline constexpr float ELITE_PROJ_MULT       = 1.9f;   // bigger, harder-hitting bolts
inline constexpr float ELITE_FLYER_BASE_HP   = 42.0f;  // elite flyers aren't one-shot fragile

inline float enemy_xp(EnemyKind kind) {
    switch (kind) {
        case EnemyKind::Ranged:       return 12.0f;
        case EnemyKind::Flying:       return 15.0f;
        case EnemyKind::Flamethrower: return 25.0f;
        case EnemyKind::Melee:
        default:                      return 10.0f;
    }
}

inline constexpr float ENEMY_RADIUS          = 0.4f;   // collision circle
inline constexpr float ENEMY_SPEED           = 2.5f;   // units/s (a touch slower than the player)
inline constexpr float ENEMY_MAX_HEALTH      = 70.0f;  // beefier (fewer but stronger)
inline constexpr float ENEMY_ATTACK_DAMAGE   = 11.0f;  // hits harder
inline constexpr float ENEMY_KNOCKBACK       = 7.0f;   // impulse an enemy deals to the player
inline constexpr float ENEMY_WEIGHT          = 4.0f;   // resists the player's knockback
inline constexpr float ENEMY_ATTACK_RANGE    = 4.5f;   // start a punch within this of the player (long reach)
inline constexpr float ENEMY_ATTACK_REACH    = 2.8f;   // (unused now the punch is a radial forcefield)
inline constexpr float ENEMY_ATTACK_CONE     = -0.5f;  // arccos(-.5)=120 deg half-arc (wide; can't just circle out)
inline constexpr float ENEMY_ATTACK_INTERVAL = 2.2f;   // seconds between attacks
inline constexpr float MELEE_SPEED_MULT      = 1.4f;   // melee chase faster than the base enemy speed
inline constexpr float ENEMY_MAX_CLIMB       = 1.5f;   // max ground rise (world units) an enemy steps up per tile
                                                       // (ramps/mounds OK; cliffs are one-way — they route to a ramp)
// Melee enemies PUNCH the ground, releasing a spherical forcefield: a radial shockwave
// that knocks back hard + does substantial damage to every player within its radius.
// A frontal shield blocks the DAMAGE (spends stamina), but the knockback shove always
// lands; otherwise dodge the i-frames or clear the radius during the windup. Big reach
// + big shove = a real threat.
inline constexpr float MELEE_FORCEFIELD_RADIUS    = 5.0f;
inline constexpr float MELEE_FORCEFIELD_DAMAGE    = 22.0f;  // substantial (2x a normal enemy hit)
inline constexpr float MELEE_FORCEFIELD_KNOCKBACK = 24.0f;  // hard radial shove
inline constexpr float PUNCH_ANIM_TIME            = 0.45f;  // forcefield-sphere visual duration
inline constexpr float ENEMY_ATTACK_WINDUP   = 0.4f;   // wind-up before the punch lands (dodge window)
inline constexpr float ENEMY_STAGGER_SPEED   = 0.5f;   // above this knock speed, AI is suppressed
inline constexpr float BLOCK_KNOCK_ABSORB    = 0.2f;   // knockback kept when a hit is blocked

// Soft separation: enemies gently repel nearby enemies so they don't pile onto the
// same tile. The push grows as they close, out to ~arm's length (so they settle a
// body-or-so apart rather than spreading out across the room).
inline constexpr float ENEMY_SEPARATION_DIST  = 1.3f;  // repel when centers are within this
inline constexpr float ENEMY_SEPARATION_SPEED = 2.2f;  // push strength (units/s at max closeness)

// Ranged enemy: keeps its distance and shoots spheres. It advances to RANGED_STANDOFF,
// backs up (still firing) if a target gets closer than standoff - margin, and drops a
// target that leaves RANGED_LEASH (out of range = unavailable -> retarget).
inline constexpr float RANGED_STANDOFF      = 7.0f;    // preferred distance — kept short so it's always inside turret range
inline constexpr float RANGED_BACKUP_MARGIN = 1.5f;    // hysteresis band around the standoff
inline constexpr float RANGED_BACKUP_SPEED  = 1.4f;    // retreat speed (slower than its advance, ENEMY_SPEED)
inline constexpr float RANGED_LEASH         = 1.0e9f;  // never give up — aggro is effectively infinite
inline constexpr float RANGED_FIRE_INTERVAL = 2.0f;    // seconds between shots (rate of fire halved)
inline constexpr float RANGED_DAMAGE        = 9.0f;
inline constexpr float RANGED_KNOCKBACK     = 3.0f;
inline constexpr float RANGED_SHOT_SPEED    = 16.0f;   // projectile travel speed (units/s) — faster
inline constexpr float RANGED_SHOT_RADIUS   = 0.35f;   // sphere size + hit radius
inline constexpr float RANGED_SHOT_LIFE     = 1.6f;    // short -> can't snipe from far outside turret range
inline constexpr float PROJECTILE_HIT_DIST  = 0.6f;    // shot within this of a player connects

// Flamethrower enemy ("Pyro"): a rare, tanky red bruiser that closes to a medium range
// and breathes a sustained cone of fire — big sustained damage that also SETS THE PLAYER
// ON FIRE (a lingering burn DoT). Its flame also lights the area like a torch.
inline constexpr float FLAME_MAX_HEALTH   = 120.0f;  // tankier than a normal enemy
inline constexpr float FLAME_RANGE        = 7.0f;    // cone reach (decent range)
inline constexpr float FLAME_CONE_COS     = 0.82f;   // ~35 deg half-angle spray
inline constexpr float FLAME_DPS          = 42.0f;   // sustained damage while you're in the cone
inline constexpr float FLAME_BURN_DPS     = 9.0f;    // lingering burn after you leave the flame
inline constexpr float FLAME_BURN_TIME    = 3.0f;    // seconds the player keeps burning
inline constexpr float FLAME_LIGHT_RADIUS = 8.0f;    // torch-like glow while firing

// Flying enemy: a ranged variant that hovers above the ground; fires fast hard lasers.
// Reachable in melee only by jumping up to it (see the vertical strike reach below).
inline constexpr float FLY_HOVER         = 2.5f;    // hover height above its ground (relative)
inline constexpr float FLY_STANDOFF      = 7.0f;    // short standoff — stays inside turret range
inline constexpr float FLY_LEASH         = 1.0e9f;  // infinite aggro
inline constexpr float FLY_FIRE_INTERVAL = 2.2f;    // rate of fire halved
inline constexpr float FLY_DAMAGE        = 16.0f;   // lasers hit hard
inline constexpr float FLY_SHOT_SPEED    = 30.0f;   // very fast laser beam

// Melee vertical reach: a swing connects only if the target's height (relative to its
// ground) is within this window of the player's strike elevation (feet-above-ground,
// 0 when standing). Ground enemies (height 0) are always in reach; the flyer (height
// FLY_HOVER) sits just above the standing window, so you must jump to clip it.
// 3D swing cone: the hit test is a cone around the player's full look direction, so
// looking up lets you reach the flyer. Heights are relative to ground (close-range
// melee, so terrain difference is negligible): the swing originates at chest height
// (+ jump), and a target's point is its body center (the flyer's is its hover height).
inline constexpr float STRIKE_ORIGIN_Y   = 1.3f;   // swing origin above the player's feet
inline constexpr float GROUND_BODY_CENTER = 1.0f;  // a ground enemy's aim point above its feet

// The player's state this frame, for combat resolution.
struct PlayerCombat {
    uint32_t id;          // stable player id (host = 0, clients 1..); enemies target by id
    bool  alive = true;   // dead players (ghosts) are ignored by enemies and deal no damage
    bool  invincible = false;  // mid-dodge i-frames: still targeted, but takes no damage
    vec3  pos;            // player position (eye)
    vec3  vel = {0,0,0};  // player velocity (units/s); ranged enemies lead their shots with it
    vec3  aim = {0,0,0};  // 3D look direction (yaw+pitch); the strike is a cone around this
    float yaw;
    bool  blocking;
    bool  strike;         // true on the single frame the player's swing connects
    float strike_reach;   // how far the swing reaches
    float strike_cos;     // cos(half-angle) of the forward hit cone
    float strike_damage;
    float strike_knockback;  // the player's knockback stat (applied to enemies hit)
    float weight;            // the player's weight (resists incoming knockback)
    float strike_height = 0.0f;  // feet height above own ground (0 standing, >0 mid-jump) for vertical reach
    // Shield: a frontal blocked hit spends block_rate stamina per point of damage to
    // negate it; with enough `stamina` the hit is fully negated (and its knockback
    // scaled away), otherwise the unaffordable remainder gets through at full damage.
    float block_cos = 0.0f;  // cos(half-angle) of the shield's frontal arc
    float block_rate = 0.0f; // stamina per point of damage blocked (0 = no shield)
    float stamina   = 0.0f;  // stamina available to spend blocking this tick
    float crit_chance = 0.0f;  // 0..1 chance a melee strike crits (rolled per enemy hit)
    float crit_mult   = 2.0f;  // damage multiplier applied on a crit
    // Elemental brands (a melee strike applies these to the enemy it hits).
    float burn_dps      = 0.0f;  // fire: burn damage/sec applied on hit (0 = no fire)
    float burn_duration = 0.0f;  // how long the burn lasts
    float slow_factor   = 1.0f;  // ice: movement multiplier applied on hit (1 = no ice)
    float slow_duration = 0.0f;
    float earth_knock   = 0.0f;  // earth: extra knockback added on hit
};

inline constexpr float BURN_INTERVAL = 0.5f;   // burn deals damage (+ a number) this often
inline constexpr float HEALTHBAR_TIME = 5.0f;  // an enemy shows its health bar this long after being hit

// A floating damage number to show over an enemy: where it was hit, how much, and
// whether it was a crit (rendered in a different color). Emitted at the damage site
// (so killing blows survive the enemy being compacted away the same tick).
struct HitNumber {
    vec3  pos;            // enemy position when hit
    float amount = 0.0f;
    bool  crit   = false;
};

// What the enemy tick did to the player (so the caller can apply it — keeps the
// sim pure and the result serializable).
struct EnemyHitPlayer {
    float damage  = 0.0f;
    vec3  knock   = {0.0f, 0.0f, 0.0f};  // impulse to add to the player's knock_vel
    bool  hit     = false;               // damage got through (-> red flash)
    bool  blocked = false;               // the shield absorbed some damage this tick
    float stamina_cost = 0.0f;           // stamina the shield spent blocking (caller deducts)
    float dealt   = 0.0f;                // damage THIS player dealt to enemies this tick (melee; for the scoreboard)
    float ignite_dps  = 0.0f;            // flamethrower: set the player on fire at this dps...
    float ignite_time = 0.0f;            // ...for this many seconds (0 = no ignite this tick)
    uint32_t attacker_id = NO_TARGET;    // id of the enemy that dealt this tick's damage (for taunts; ranged included)
};

// Advance enemies one tick against ALL players (co-op). Each player's strike is
// resolved against enemies; then every enemy pursues its committed target player
// (descending THAT player's flow field) and attacks it in range. `flows` is parallel
// to `players` (flows[i] is the distance field to players[i]); an enemy with no
// committed target picks the nearest, and retarget-on-hit switches it to the player
// who just struck it. `out` is sized to players.size(); out[i] is what was done to
// player i (damage/knock/flash). Pure over (list, map, flows, players) + list.rng.
// Dead enemies are removed. `deaths` (optional): each removed enemy appends its
// position as 3 floats (x,y,z) so the caller can drop loot.
// `hits` (optional): each player strike that connects appends a HitNumber (enemy pos,
// damage dealt, crit) for floating damage numbers; melee crits are rolled here.
// `tile_heights` (optional): row-major ground elevation per tile. When given, enemy
// flow-following obeys the same one-way climb rule as the flow field (can drop off
// cliffs, can only climb gentle rises/ramps) so they path to ramps instead of walls.
void update_enemies(EntityList& list, const dc::world::Map& map,
                    const std::vector<dc::world::FlowField>& flows,
                    const std::vector<PlayerCombat>& players,
                    std::vector<EnemyHitPlayer>& out, float dt,
                    std::vector<float>* deaths = nullptr,
                    std::vector<HitNumber>* hits = nullptr,
                    const std::vector<float>* tile_heights = nullptr,
                    std::vector<float>* death_xp = nullptr);   // parallel to `deaths`: XP per kill

// Advance in-flight projectiles (host-authoritative): move, expire by lifetime, die
// on walls, and on reaching a LIVING player deal damage + knockback into out[i]
// (parallel to `players`, same convention as update_enemies). Pure over
// (list.projectiles, map, players). Spent shots are removed.
void update_projectiles(EntityList& list, const dc::world::Map& map,
                        const std::vector<PlayerCombat>& players,
                        std::vector<EnemyHitPlayer>& out, float dt);

// Damage + knock every enemy within `radius` (xz) of `center`, skipping ids already
// in `already_hit` (so a moving hazard hits each enemy once per pass). Marks the
// dead; the caller's next update_enemies compacts them. Pure / reusable (thrown
// sword now, explosions later). Returns the total damage actually dealt (capped at
// each enemy's remaining health) so the caller can credit it to the attacker.
float radius_attack(EntityList& list, const vec3 center, float radius,
                    float damage, float knockback, std::vector<uint32_t>& already_hit,
                    std::vector<HitNumber>* hits = nullptr);

} // namespace dc::entity
