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
};

// Client -> host one-shot "cast" events. The host runs each special on its own clock
// (motion + damage) and broadcasts the evolving state in the snapshot; the caster
// predicts locally for responsiveness. Reliable so a cast is never dropped.
struct BashCast {
    float radius = 0.0f, damage = 0.0f, knockback = 0.0f, duration = 0.0f;
};
struct OrbitCast {
    float duration = 0.0f, radius = 0.0f, hit_radius = 0.0f, damage = 0.0f, knockback = 0.0f;
    int32_t count = 0;
};
struct ThrownCast {
    float dx = 0.0f, dy = 0.0f, dz = 0.0f;   // launch direction (3D, unit)
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;   // launch origin (eye)
    float speed = 0.0f, distance = 0.0f, radius = 0.0f, damage = 0.0f, knockback = 0.0f, size = 1.0f;
};

// Client -> host, every frame: what the player is doing. Gameplay fields (strike,
// blocking) drive the host's combat; the rest are cosmetic animation clocks the
// client owns and the host just relays so everyone sees the same swing/block.
struct InputCmd {
    float   forward = 0.0f, strafe = 0.0f;
    float   yaw = 0.0f, pitch = 0.0f;
    uint8_t jump = 0;
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
    float    block_spent = 0.0f;             // stamina the host's block resolution spent for this player this tick
    uint8_t  punching = 0, blocking = 0;
    float    punch_time = 0.0f, block_time = 0.0f;
    float    hit_flash = 0.0f;
    float    sword_scale = 1.0f;             // blue-upgrade blade size
    // Specials, for rendering everyone's thrown/orbit swords on every screen.
    uint8_t  thrown_active = 0;
    float    thrown_x = 0.0f, thrown_y = 0.0f, thrown_z = 0.0f, thrown_spin = 0.0f, thrown_size = 1.0f;
    uint8_t  orbit_active = 0;
    int32_t  orbit_count = 0;
    float    orbit_angle = 0.0f, orbit_spin = 0.0f, orbit_radius = 0.0f;
    uint8_t  bash_active = 0;        // shield-bash nova in progress
    float    bash_radius = 0.0f;     // current shockwave radius (for the expanding sphere)
};

// One enemy's replicated state (render-only on clients).
struct EnemyState {
    float   x = 0.0f, z = 0.0f, yaw = 0.0f;
    float   anim_time = 0.0f, attack_time = 0.0f, hit_flash = 0.0f;
    uint8_t attacking = 0;
    uint8_t kind = 0;           // dc::entity::EnemyKind (0 = Melee, 1 = Ranged) -> render color
};

// One dropped coin's position (render-only on clients).
struct CoinState { float x = 0.0f, z = 0.0f; };

// One in-flight projectile's position + glow color (render-only on clients).
struct ProjectileState { float x = 0.0f, y = 0.0f, z = 0.0f, r = 0.75f, g = 0.35f, b = 1.0f; };

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
