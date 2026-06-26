#pragma once
#include <cstdint>
#include <cglm/cglm.h>
#include "engine/entity/entity.h"
#include "engine/world/map.h"

namespace dc::entity {

// Spawns enemies over time within a disc, only on valid (Open) floor tiles.
// GL-free / testable; the RNG lives in the struct so the host owns it (networking).
struct Spawner {
    vec3     pos       = {0.0f, 0.0f, 0.0f};  // disc center (xz used)
    float    radius    = 4.0f;                // spawn within this radius
    float    rate      = 0.5f;                // entities per second
    int      max_alive = 8;                   // stop emitting past this many entities total
    float    ranged_fraction = 0.0f;          // chance each spawn is a ranged enemy (vs melee)
    float    flying_fraction = 0.0f;          // chance each spawn is a flying enemy (checked first)
    float    flame_fraction  = 0.0f;          // chance each spawn is a (rare) flamethrower enemy
    float    skeleton_fraction = 0.0f;        // chance each spawn is a (melee-style) skeleton
    float    bat_fraction      = 0.0f;        // chance each spawn is a (flying) bat
    float    troll_fraction    = 0.0f;        // chance each spawn is a (rare) big troll
    float    demon_fraction    = 0.0f;        // chance each spawn is a (rare) big demon
    float    insulter_fraction = 0.0f;        // chance each spawn is an Insulter (attack-weakening aura)
    float    slime_fraction    = 0.0f;        // chance each spawn is a Slime (leaves slowing trails)
    float    elite_fraction  = 0.0f;          // chance each spawn rolls into a rare elite (any kind)
    float    stat_mult = 1.0f;                // escalation: scales each spawn's HP + damage over time
    float    accum     = 0.0f;                // fractional spawns owed
    uint32_t rng       = 0x2545f491u;

    // Advance: emit at `rate`/sec, each at a random Open floor tile inside the disc.
    void update(float dt, EntityList& list, const dc::world::Map& map);
};

} // namespace dc::entity
