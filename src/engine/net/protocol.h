#pragma once
#include <cstdint>

// Wire messages for player replication. POD structs memcpy'd into packets — fine
// while both ends are the same build/arch (dev + same-platform Steam). Swap to
// explicit field-by-field serialization if we ever go cross-platform.
namespace dc::net {

enum class MsgType : uint8_t {
    Input = 1, Snapshot = 2, AssignId = 3,
    OpenChest = 4,      // client -> host: request to buy/open chest [uint32 index]
    ChestGranted = 5,   // host -> client: open approved [uint32 index] (deducted, marked open)
    BashCast = 6,       // client -> host: cast the shield-bash nova [BashCast]   (reliable)
    OrbitCast = 7,      // client -> host: summon the orbiting swords      [OrbitCast]  (reliable)
    ThrownCast = 8,     // client -> host: throw the sword                 [ThrownCast] (reliable)
    DashCast = 9,       // client -> host: dodge-dash                       [DashCast]   (reliable)
    BuyItem = 10,       // client -> host: buy a chest slot [uint32 chest][uint32 slot]   (reliable)
    ItemGranted = 11,   // host -> client: purchase approved [uint8 upgrade_type]         (reliable)
    ReleaseChest = 12,  // client -> host: closed the menu without buying [uint32 chest]  (reliable)
    BuyDrone = 13,      // client -> host: buy a ground drone vendor [uint32 vendor]      (reliable)
    DroneGranted = 14,  // host -> client: drone purchase approved (gain a gunner minion) (reliable)
    XpGranted = 15,     // host -> client: you absorbed an XP orb [float amount] -> client levels up locally (reliable)
    Appearance = 16,    // peer -> peer: a player's look [uint32 id][dc::game::Appearance] (reliable, relayed by host)
    StartGame = 17,     // host -> clients: leave the lobby and start playing (reliable)
    Taunt = 18,         // host -> clients: an enemy taunts [TauntState] -> floating text + TTS (reliable)
    BoltCast = 19,      // client -> host: wizard staff bolt(s) [BoltCast] (reliable)
    BuildPlace = 20,    // client -> host: place a base piece [BuildEdit] (reliable)
    BuildRemove = 21,   // client -> host: remove the base piece at a tile [BuildEdit] (reliable)
    BuyBaseArea = 22,   // client -> host: buy a buildable-area expansion (reliable)
    BuyTurret = 23,     // client -> host: buy one more perimeter turret (reliable)
    BuyUnlock = 24,     // client -> host: unlock a barracks mob type [uint8 type] (reliable)
    RallyCmd = 25,      // client -> host: set/clear the mob rally point [uint8 active][float x][float z] (reliable)
    HoldCmd = 26,       // client -> host: set a mob TYPE's command-map hold [uint8 type][uint8 active][float x] (reliable)
};

// Client -> host: place/remove a base piece at a tile. For BuildRemove only col/row matter.
struct BuildEdit {
    int16_t col = 0, row = 0;
    uint8_t piece = 0;   // dc::game::BuildPiece (place only)
    uint8_t rot = 0;     // 0..3 (place only)
};

// Client -> host: a wizard staff cast. The host spawns + simulates the bolt(s) (damage) and
// broadcasts their positions in the snapshot for everyone to render.
struct BoltCast {
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;   // origin (eye)
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;   // aim (unit)
    float radius = 0.5f, damage = 18.0f, knockback = 6.0f;
    uint8_t big = 0, count = 1;              // big bolt? how many (spread)
};

// One in-flight staff bolt's render state (host -> everyone).
struct BoltState { float x = 0.0f, y = 0.0f, z = 0.0f; uint8_t big = 0; };

// One enemy taunt fired this moment: where it spawns (over the enemy's head) and which
// canned insult (index into dc::game::taunt_text). Clients float the text + speak it.
struct TauntState { float x = 0.0f, y = 0.0f, z = 0.0f; char text[64] = {0}; };

// Client -> host one-shot "cast" events. The host runs each special on its own clock
// (motion + damage) and broadcasts the evolving state in the snapshot; the caster
// predicts locally for responsiveness. Reliable so a cast is never dropped.
struct BashCast {
    float radius = 0.0f, damage = 0.0f, knockback = 0.0f, duration = 0.0f;
};
struct OrbitCast {
    float duration = 0.0f, radius = 0.0f, hit_radius = 0.0f, damage = 0.0f, knockback = 0.0f;
    int32_t count = 0;
    float tick = 0.25f;        // damage re-hit interval (Orbit Tempo shortens it)
    float spin = 1.0f;         // revolve/spin speed multiplier (Orbit Tempo speeds it up)
};
struct ThrownCast {
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;   // launch direction (3D, unit)
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;   // launch origin (eye)
    float speed = 0.0f, distance = 0.0f, radius = 0.0f, damage = 0.0f, knockback = 0.0f, size = 1.0f;
};
struct DashCast {
    float dx = 0.0f, dz = 0.0f;              // dash direction (horizontal, unit)
    float speed = 0.0f, iframes = 0.0f, decay = 8.0f;
    float supersonic = 0.0f;                 // >0: dodge shockwave damage (host fires the AoE)
};

// Client -> host, every frame: what the player is doing. Gameplay fields (strike,
// blocking) drive the host's combat; the rest are cosmetic animation clocks the
// client owns and the host just relays so everyone sees the same swing/block.
struct InputCmd {
    float   forward = 0.0f, strafe = 0.0f;
    float   yaw = 0.0f, pitch = 0.0f;
    uint8_t jump = 0;
    uint8_t board = 0;         // edge: client wants to board/dismount a nearby friendly boat this frame
    uint8_t strike = 0;        // melee connected this frame (host resolves damage)
    uint8_t blocking = 0;      // shield fully raised (host mitigates)
    uint8_t anim_punch = 0;    // mid-swing (drives the punch clip)
    uint8_t anim_block = 0;    // shield being raised/held (drives the block clip)
    float   punch_time = 0.0f; // punch-clip clock
    float   block_time = 0.0f; // block-clip clock
    // Resolved combat loadout (the client's weapon + upgrades, already applied), so
    // the host uses the client's real stats. Trusts the client — fine for co-op PvE.
    float   strike_damage = 0.0f, strike_reach = 0.0f, strike_cos = 0.0f;
    float   strike_knockback = 0.0f, weight = 0.0f;
    float   block_cos = 0.0f, block_rate = 0.0f;   // shield arc + stamina-per-damage
    float   stamina = 0.0f;                        // client's current stamina (host needs it to resolve blocks)
    float   sword_scale = 1.0f;   // blade size (drives remote sword render)
    float   crit_chance = 0.0f, crit_mult = 2.0f;  // host rolls the client's melee crits with these
    float   health_regen = 1.5f;                   // hp/sec the host regens this client's body
    // Elemental brands the host applies on this client's melee hits.
    float   fire_dps = 0.0f, fire_duration = 0.0f;
    float   slow_factor = 1.0f, slow_duration = 0.0f;
    float   earth_knock = 0.0f;
    uint8_t minion_count = 0; float minion_damage = 0.0f, minion_range = 18.0f;   // host fires this client's gunner minions
    float   trail_damage = 0.0f, trail_life = 0.0f;         // trailblazer fire-trail (host damages from it)
    // (Specials are no longer streamed here — they're host-run from the *Cast events.)
};

// One player's replicated state in a snapshot. Beyond pose, it carries the combat
// animation clocks and hit-flash so remote avatars swing/block/flash like the local
// one (sword + shield are attached for everyone).
struct PlayerState {
    uint32_t id = 0;
    float    x = 0.0f, y = 0.0f, z = 0.0f;   // eye position (as Player::position)
    float    yaw = 0.0f, pitch = 0.0f;
    float    anim_time = 0.0f;
    float    health = 0.0f;
    uint8_t  moving = 0;
    int32_t  currency = 0;                   // this player's own coin wallet
    float    damage_dealt = 0.0f;            // total damage this player has dealt to enemies (scoreboard)
    uint8_t  elements = 0;                   // equipped elemental brands bitmask (1=fire,2=ice,4=earth) for sword particles
    uint8_t  minions = 0;                    // gunner minion count, so everyone renders the orbiting drones
    float    minion_range = 18.0f;           // drone targeting range (for remote drones' laser visuals)
    float    trail_life = 0.0f;              // trailblazer segment lifetime (>0 = leaving a fire trail)
    float    block_spent = 0.0f;             // stamina the host's block resolution spent for this player this tick
    uint8_t  punching = 0, blocking = 0;
    float    punch_time = 0.0f, block_time = 0.0f;
    float    hit_flash = 0.0f;
    uint8_t  burning = 0;                    // on fire (flamethrower) -> flame particles on every screen
    float    sword_scale = 1.0f;             // blue-upgrade blade size
    // Orbit/bash specials are single-per-player; thrown swords (which can be several at
    // once with Swordstorm) ride a separate owner-keyed list (ThrownState) instead.
    uint8_t  orbit_active = 0;
    int32_t  orbit_count = 0;
    float    orbit_angle = 0.0f, orbit_spin = 0.0f, orbit_radius = 0.0f;
    uint8_t  bash_active = 0;        // shield-bash nova in progress
    float    bash_radius = 0.0f;     // current shockwave radius (for the expanding sphere)
};

// One enemy's replicated state (render-only on clients).
struct EnemyState {
    float   x = 0.0f, z = 0.0f, yaw = 0.0f;
    float   anim_time = 0.0f, attack_time = 0.0f, hit_flash = 0.0f, punch_anim = 0.0f;
    float   health01 = 1.0f;        // current health fraction (for the over-head bar)
    float   healthbar_time = 0.0f;  // seconds left to show the bar (0 = hidden)
    uint8_t attacking = 0;
    uint8_t kind = 0;           // dc::entity::EnemyKind (0 = Melee, 1 = Ranged) -> render color
    uint8_t status = 0;         // status bitmask (1=burning, 2=slowed, 4=elite) -> particles/scale
};

// One friendly lane-mob's render state (host -> everyone): position, facing, health fraction.
struct AllyState { float x = 0.0f, z = 0.0f, yaw = 0.0f; float health01 = 1.0f; float size = 1.0f; uint8_t kind = 0; };

// One naval unit's render state (host -> everyone). `kind`: 0 = enemy boat, 1 = friendly sub
// (submerged periscope), 2 = friendly sub surfaced, 4 = friendly warship, 5/6 = enemy sub
// submerged/surfaced. `health01` drives the over-hull bar.
struct BoatState { float x = 0.0f, z = 0.0f, yaw = 0.0f; float health01 = 1.0f; uint8_t kind = 0; };

// One enemy SEA-MINE bobbing in the river (host -> everyone): position + arm fraction (0=dropping,1=armed).
struct MineState { float x = 0.0f, z = 0.0f, armed = 1.0f; };

// One slime puddle on the ground (host -> everyone): position, radius, remaining-life fraction.
struct SlimePatchState { float x = 0.0f, z = 0.0f, radius = 1.0f, life01 = 1.0f; };

// One dropped coin's position (render-only on clients).
struct CoinState { float x = 0.0f, z = 0.0f; };

// One dropped XP orb's position (render-only on clients; the host owns pickups + XP).
struct XPOrbState { float x = 0.0f, z = 0.0f; };

// One in-flight thrown sword (host -> everyone), keyed by its owner's player id so each
// client can render every player's swords (a Swordstorm volley is several at once) while
// skipping its own (which it predicts locally).
struct ThrownState { float x = 0.0f, y = 0.0f, z = 0.0f, spin = 0.0f, size = 1.0f; uint32_t owner = 0; };

// One in-flight projectile's position + glow color + travel dir (render-only on clients).
// `beam` => render as a stretched glowing laser (eye); vx/vz give its on-screen orientation.
struct ProjectileState { float x = 0.0f, y = 0.0f, z = 0.0f, r = 0.75f, g = 0.35f, b = 1.0f;
                         float vx = 0.0f, vy = 0.0f, vz = 0.0f; float radius = 0.35f; uint8_t beam = 0; };

// A floating damage number spawned this tick (host -> everyone): position, amount, crit.
struct DamageNumState { float x = 0.0f, y = 0.0f, z = 0.0f, amount = 0.0f; uint8_t crit = 0; };

// A dropped chest item lying on the ground (host -> everyone). `type` is dc Upgrade.
struct GroundItemState { uint32_t id = 0; uint8_t type = 0; float x = 0.0f, y = 0.0f, z = 0.0f; };

// Packets:
//   Input:    [MsgType::Input]    [InputCmd]
//   AssignId: [MsgType::AssignId] [uint32 id]      (host tells a client its id)
//   Snapshot: [MsgType::Snapshot]
//             [uint32 np][PlayerState x np]
//             [uint32 ne][EnemyState  x ne]
//             [uint32 nc][CoinState   x nc]
//             [uint32 nh][uint8 opened x nh]   (chest open-state, stable map order)
//             [uint32 npr][ProjectileState x npr]
//   OpenChest:    [MsgType::OpenChest]    [uint32 chest_index]   (client -> host)
//   ChestGranted: [MsgType::ChestGranted] [uint32 chest_index]   (host -> the requester)

} // namespace dc::net
