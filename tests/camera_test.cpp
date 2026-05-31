#include "engine/renderer/camera.h"
#include "engine/entity/player.h"
#include "engine/world/map.h"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace dc::renderer;
using dc::entity::Player;

static bool approx(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

int main() {
    Camera cam;

    // Projection is a standard OpenGL RH perspective: out[3][3]==0, out[2][3]==-1.
    {
        mat4 proj;
        cam.proj_matrix(proj, 16.0f / 9.0f);
        assert(approx(proj[3][3], 0.0f));
        assert(approx(proj[2][3], -1.0f));
    }

    // View: player in the middle of an open room looking +X (yaw=0). The camera
    // sits behind the player; a world point ahead of the player (+X) lands in
    // front of the camera => negative view-space Z.
    {
        auto m = dc::world::parse_map(
            "#####\n"
            "#...#\n"
            "#...#\n"
            "#...#\n"
            "#####\n");
        assert(m.has_value());

        Player p;
        p.position[0] = 5.0f;                     // center-ish open cell (TILE=2)
        p.position[1] = dc::world::EYE_HEIGHT;
        p.position[2] = 5.0f;
        p.yaw = 0.0f;                             // front = +X

        mat4 view;
        cam.view_matrix(view, p, *m);
        vec4 world = { p.position[0] + 5.0f, p.position[1], p.position[2], 1.0f }, vp;
        glm_mat4_mulv(view, world, vp);
        assert(vp[2] < 0.0f);
    }

    std::printf("PASS camera\n");
    return 0;
}
