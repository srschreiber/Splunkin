#pragma once
#include <cglm/cglm.h>

namespace dc::renderer {

struct Camera {
    vec3  position = {0.0f, 0.0f, 0.0f};
    float yaw   = 0.0f;            // radians
    float pitch = 0.0f;            // radians, clamped to +-89 degrees
    float fov_y = 1.2217305f;      // 70 degrees in radians
    float move_speed = 4.0f;       // units / second
    float mouse_sens = 0.0025f;    // radians / pixel

    // Full look direction unit vector from yaw/pitch.
    void front(vec3 out) const;

    // Apply a mouse delta (pixels). yaw += dx*sens; pitch -= dy*sens; clamp pitch.
    void look(float dx, float dy);

    // Move in the XZ plane: forward along horizontal look dir, strafe to the
    // right. Y (height) is left unchanged. amounts are typically -1, 0, or 1.
    void move(float forward, float strafe, float dt);

    void view_matrix(mat4 out) const;
    void proj_matrix(mat4 out, float aspect) const;
};

} // namespace dc::renderer
