#include "engine/renderer/camera.h"
#include "engine/entity/player.h"

namespace dc::renderer {

// creates a matrix that transforms world coordinates into view space. Third-person:
// the eye sits `distance` units behind the player along the look direction, looking
// forward through the player.
void Camera::view_matrix(mat4 out, const dc::entity::Player& p) const {
    vec3 f;  p.front(f);
    vec3 up = {0.0f, 1.0f, 0.0f};
    vec3 eye = { p.position[0] - f[0] * distance,
                 p.position[1] - f[1] * distance,
                 p.position[2] - f[2] * distance };
    glm_look(eye, f, up, out);
}

// creates a perspective projection matrix with the given aspect ratio
void Camera::proj_matrix(mat4 out, float aspect) const {
    glm_perspective(fov_y, aspect, near_z, far_z, out);
}

} // namespace dc::renderer
