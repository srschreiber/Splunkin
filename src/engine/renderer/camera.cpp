#include "engine/renderer/camera.h"
#include "engine/entity/player.h"
#include "engine/world/collision.h"

namespace dc::renderer {

// creates a matrix that transforms world coordinates into view space. Third-person:
// the eye sits `distance` units behind the player along the look direction, looking
// forward through the player.
void Camera::view_matrix(mat4 out, dc::entity::Player& p, dc::world::Map& map) {
    vec3 f;  p.front(f);
    vec3 up = {0.0f, 1.0f, 0.0f};
    vec3 eye = { p.position[0] - f[0] * distance,
                 p.position[1] - f[1] * distance*1.5f,
                 p.position[2] - f[2] * distance };
    
    // // if eye is in a solid cell, move it against the surface normal
  

    // cap the camera height
    const float cam_ceil = dc::world::WALL_HEIGHT - 0.2f;
    if (eye[1] > cam_ceil) eye[1] = cam_ceil;

    // make sure camera is always higher than the player's head so we aren't blocked
    if (eye[1] < p.position[1] + 0.5f) eye[1] = p.position[1] + .5f;
    
    // raycast from player to eye. If any solid is hit, that eye will be clamped to the hit position
    const float step = 0.01f;
    
    for (float d = step; d <= distance; d += step) {
        vec3 test = { p.position[0] - f[0] * d,
                      p.position[2] - f[2] * d };
        if (dc::world::circle_hits_solid(map, test[0], test[1], CAM_RADIUS)) {
            eye[0] = test[0];
            eye[2] = test[1];
            break;
        }
    }

    // Now we shift the last_safe_eye towards the current eye, so that if we get stuck in a wall, the camera will smoothly move to the last safe position instead of snapping.
    const float smooth_factor = 0.1f;
    glm_vec3_lerp(last_safe_eye, eye, smooth_factor, last_safe_eye);

    glm_look(last_safe_eye, f, up, out);
}

// creates a perspective projection matrix with the given aspect ratio
void Camera::proj_matrix(mat4 out, float aspect) const {
    glm_perspective(fov_y, aspect, near_z, far_z, out);
}

} // namespace dc::renderer
