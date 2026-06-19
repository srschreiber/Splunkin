#pragma once
#include <cglm/cglm.h>

namespace dc::entity { struct Player; }

namespace dc::world { struct Map; }
namespace dc::renderer {
inline constexpr float CAM_RADIUS    = 0.2f;   // world units
// A lens. Produces view/projection matrices from a Player's eye + orientation.
struct Camera {
    float fov_y  = 1.2217305f;   // 70 degrees in radians
    float near_z = 0.05f;
    float far_z  = 100.0f;
    bool third_person = true;    // default view: eye pulled back behind the player (V toggles FP)
    float distance = 3.0f;       // third-person: how far the eye sits behind the player
    vec3 smoothed_eye = {0.0f, 0.0f, 0.0f};   // exponentially-smoothed camera position
    bool eye_initialized = false;             // snap on the first frame, then smooth

    void view_matrix(mat4 out, dc::entity::Player& p, dc::world::Map& map, float dt);
    void proj_matrix(mat4 out, float aspect) const;
};

} // namespace dc::renderer
