#include "engine/renderer/camera.h"
#include <cmath>

namespace dc::renderer {

void Camera::front(vec3 out) const {
    out[0] = std::cos(pitch) * std::cos(yaw);
    out[1] = std::sin(pitch);
    out[2] = std::cos(pitch) * std::sin(yaw);
    glm_vec3_normalize(out);
}

void Camera::look(float dx, float dy) {
    yaw   += dx * mouse_sens;
    pitch -= dy * mouse_sens;
    const float limit = glm_rad(89.0f);
    if (pitch >  limit) pitch =  limit;
    if (pitch < -limit) pitch = -limit;
}

void Camera::move(float forward, float strafe, float dt) {
    vec3 up = {0.0f, 1.0f, 0.0f};

    // Horizontal forward (ignore pitch so walking stays level).
    vec3 walk = { std::cos(yaw), 0.0f, std::sin(yaw) };
    glm_vec3_normalize(walk);

    // Right = normalize(cross(walk, up)).
    vec3 right;
    glm_vec3_cross(walk, up, right);
    glm_vec3_normalize(right);

    vec3 delta = {0, 0, 0};
    vec3 tmp;
    glm_vec3_scale(walk,  forward, tmp); glm_vec3_add(delta, tmp, delta);
    glm_vec3_scale(right, strafe,  tmp); glm_vec3_add(delta, tmp, delta);

    glm_vec3_scale(delta, move_speed * dt, delta);
    position[0] += delta[0];
    position[2] += delta[2];   // Y intentionally unchanged (locked eye height)
}

void Camera::view_matrix(mat4 out) const {
    vec3 f; front(f);
    vec3 up = {0.0f, 1.0f, 0.0f};
    glm_look(const_cast<float*>(position), f, up, out);
}

void Camera::proj_matrix(mat4 out, float aspect) const {
    glm_perspective(fov_y, aspect, 0.05f, 100.0f, out);
}

} // namespace dc::renderer
