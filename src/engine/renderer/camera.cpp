#include "engine/renderer/camera.h"
#include "engine/entity/player.h"

namespace dc::renderer {

void Camera::view_matrix(mat4 out, const dc::entity::Player& p) const {
    vec3 f;  p.front(f);
    vec3 up = {0.0f, 1.0f, 0.0f};
    vec3 eye = { p.position[0], p.position[1], p.position[2] };  // copy (glm_look wants non-const)
    glm_look(eye, f, up, out);
}

void Camera::proj_matrix(mat4 out, float aspect) const {
    glm_perspective(fov_y, aspect, near_z, far_z, out);
}

} // namespace dc::renderer
