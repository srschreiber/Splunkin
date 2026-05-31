#include "engine/entity/player.h"
#include "engine/world/map.h"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace dc::entity;
using dc::world::EYE_HEIGHT;

static bool approx(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

// A 10x7 room: solid border, open interior cols 1..8 rows 1..5.
static const char* ROOM =
    "##########\n"
    "#........#\n"
    "#........#\n"
    "#........#\n"
    "#........#\n"
    "#........#\n"
    "##########\n";

int main() {
    auto map = dc::world::parse_map(ROOM);
    assert(map.has_value());

    // add_look clamps pitch to +-89 deg.
    {
        Player p;
        p.add_look(0.0f, -1.0e6f);
        assert(p.pitch <= glm_rad(89.0f) + 1e-3f);
        p.add_look(0.0f, 2.0e6f);
        assert(p.pitch >= -glm_rad(89.0f) - 1e-3f);
    }

    // Blocked axis: standing in cell col1 (x in [2,4]) near the left wall (col0,
    // x<=2), moving -X is blocked; position.x unchanged.
    {
        Player p;
        p.position[0] = 2.5f; p.position[1] = EYE_HEIGHT; p.position[2] = 7.0f;
        p.yaw = static_cast<float>(M_PI);   // walk dir = -X
        p.update(1.0f, 0.0f, false, 0.1f, *map);   // 0.4 units toward wall
        assert(approx(p.position[0], 2.5f));        // blocked, did not move
    }

    // Free axis slides: from the same spot, moving +Z (no wall) advances Z and
    // leaves X unchanged.
    {
        Player p;
        p.position[0] = 2.5f; p.position[1] = EYE_HEIGHT; p.position[2] = 7.0f;
        p.yaw = static_cast<float>(M_PI / 2.0);   // walk dir = +Z
        p.update(1.0f, 0.0f, false, 0.1f, *map);
        assert(p.position[2] > 7.0f);             // moved along Z
        assert(approx(p.position[0], 2.5f));      // X unchanged
    }

    // Diagonal not faster: horizontal displacement magnitude == MOVE_SPEED*dt.
    {
        Player p;
        p.position[0] = 10.0f; p.position[1] = EYE_HEIGHT; p.position[2] = 7.0f;
        p.yaw = 0.0f;
        const float x0 = p.position[0], z0 = p.position[2];
        const float dt = 0.05f;
        p.update(1.0f, 1.0f, false, dt, *map);    // forward + strafe
        const float dx = p.position[0] - x0, dz = p.position[2] - z0;
        assert(approx(std::sqrt(dx * dx + dz * dz), MOVE_SPEED * dt));
    }

    // Gravity: dropped from above rest, eventually lands at EYE_HEIGHT, grounded.
    {
        Player p;
        p.position[0] = 10.0f; p.position[1] = EYE_HEIGHT + 2.0f; p.position[2] = 7.0f;
        p.on_ground = false;
        for (int i = 0; i < 200; ++i) p.update(0.0f, 0.0f, false, 0.05f, *map);
        assert(approx(p.position[1], EYE_HEIGHT));
        assert(p.on_ground == true);
    }

    // Jump + no double-jump.
    {
        Player p;
        p.position[0] = 10.0f; p.position[1] = EYE_HEIGHT; p.position[2] = 7.0f;
        p.on_ground = true;
        p.update(0.0f, 0.0f, true, 0.1f, *map);   // jump from ground
        assert(p.on_ground == false);
        assert(p.position[1] > EYE_HEIGHT);
        const float v1 = p.vel_y;                 // upward, reduced by one g*dt
        assert(v1 > 0.0f);
        p.update(0.0f, 0.0f, true, 0.1f, *map);   // jump pressed again mid-air
        assert(p.vel_y < v1);                     // no re-launch; gravity reduced it
    }

    std::printf("PASS player\n");
    return 0;
}
