#pragma once
#include <cglm/cglm.h>

namespace dc::entity { struct Player; }

namespace dc::renderer {

// A lens. Produces view/projection matrices from a Player's eye + orientation.
struct Camera {
    float fov_y  = 1.2217305f;   // 70 degrees in radians
    float near_z = 0.05f;
    float far_z  = 100.0f;
    float distance = 4.0f;       // third-person: how far the eye sits behind the player

    void view_matrix(mat4 out, const dc::entity::Player& p) const;
    void proj_matrix(mat4 out, float aspect) const;
};

} // namespace dc::renderer
