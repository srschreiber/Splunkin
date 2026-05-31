#include "engine/renderer/camera.h"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace dc::renderer;

static bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

int main() {
    // Default yaw=0,pitch=0 -> front points along +X.
    {
        Camera c;
        vec3 f; c.front(f);
        assert(approx(f[0], 1.0f) && approx(f[1], 0.0f) && approx(f[2], 0.0f));
    }

    // Pitch clamps to +-89 degrees (~1.55334 rad).
    {
        Camera c;
        c.look(0.0f, -100000.0f);  // huge upward delta
        assert(c.pitch <= glm_rad(89.0f) + 1e-3f);
        c.look(0.0f,  200000.0f);  // huge downward delta
        assert(c.pitch >= -glm_rad(89.0f) - 1e-3f);
    }

    // Moving forward with yaw=0 advances +X by move_speed*dt, leaves Y fixed.
    {
        Camera c;
        c.position[1] = 1.6f;
        c.move(1.0f, 0.0f, 0.5f);   // dt=0.5, speed=4 -> +2 along +X
        assert(approx(c.position[0], 2.0f));
        assert(approx(c.position[1], 1.6f));   // height unchanged
        assert(approx(c.position[2], 0.0f));
    }

    // Strafing right with yaw=0 (front=+X, up=+Y): right = cross(front,up) = +Z.
    {
        Camera c;
        c.move(0.0f, 1.0f, 0.5f);   // +2 along right
        assert(approx(c.position[2], 2.0f));
        assert(approx(c.position[0], 0.0f));
    }

    std::printf("PASS camera\n");
    return 0;
}
