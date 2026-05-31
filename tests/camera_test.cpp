#include "engine/renderer/camera.h"
#include "engine/entity/player.h"
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

    // View: player at origin looking +X (yaw=0). A world point ahead (+X) lands
    // in front of the camera => negative view-space Z.
    {
        Player p;            // position {0,0,0}, yaw 0, pitch 0
        mat4 view;
        cam.view_matrix(view, p);
        vec4 world = {5.0f, 0.0f, 0.0f, 1.0f}, vp;
        glm_mat4_mulv(view, world, vp);
        assert(vp[2] < 0.0f);
    }

    std::printf("PASS camera\n");
    return 0;
}
