#include "engine/renderer/camera.h"
#include "engine/entity/player.h"
#include "engine/world/collision.h"
#include <cmath>

namespace dc::renderer {

// creates a matrix that transforms world coordinates into view space. Third-person:
// the eye sits `distance` units behind the player along the look direction, looking
// forward through the player.
void Camera::view_matrix(mat4 out, dc::entity::Player& p, dc::world::Map& map, float dt) {
    vec3 f;  p.front(f);
    vec3 up = {0.0f, 1.0f, 0.0f};

    // First person (default): eye exactly at the player's eye, looking along the full
    // look direction. No offset / smoothing / raycast — position is already resolved.
    if (!third_person) {
        (void)map; (void)dt;
        glm_look(p.position, f, up, out);
        return;
    }

    // Third person (debug): pull the eye back along the look direction and a bit up,
    // raycast so walls don't clip the camera, and ease toward the target for smoothness.
    vec3 eye = { p.position[0] - f[0] * distance,
                 p.position[1] - f[1] * distance + 1.0f,
                 p.position[2] - f[2] * distance };
    const float stepd = 0.05f;
    for (float d = stepd; d <= distance; d += stepd) {
        const float tx = p.position[0] - f[0] * d, tz = p.position[2] - f[2] * d;
        if (dc::world::circle_hits_solid(map, tx, tz, CAM_RADIUS)) { eye[0] = tx; eye[2] = tz; break; }
    }
    const float factor = 1.0f - std::exp(-12.0f * dt);
    if (!eye_initialized) { glm_vec3_copy(eye, smoothed_eye); eye_initialized = true; }
    else                  glm_vec3_lerp(smoothed_eye, eye, factor, smoothed_eye);
    glm_look(smoothed_eye, f, up, out);
}

// creates a perspective projection matrix with the given aspect ratio
void Camera::proj_matrix(mat4 out, float aspect) const {
    glm_perspective(fov_y, aspect, near_z, far_z, out);
}

} // namespace dc::renderer
