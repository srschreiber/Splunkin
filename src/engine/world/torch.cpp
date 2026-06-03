#include "engine/world/torch.h"

namespace dc::world {

namespace {
// Unit vector from a wall cell toward the open cell the torch faces.
void face_vec(Dir dir, float& dx, float& dz) {
    switch (dir) {
        case Dir::North: dx = 0.0f;  dz = -1.0f; break;  // open is -Z
        case Dir::East:  dx = 1.0f;  dz = 0.0f;  break;
        case Dir::South: dx = 0.0f;  dz = 1.0f;  break;
        case Dir::West:  dx = -1.0f; dz = 0.0f;  break;
    }
}

// Yaw that turns the model (authored facing -Z at yaw 0) to face `dir`.
float face_yaw(Dir dir) {
    switch (dir) {
        case Dir::North: return 0.0f;
        case Dir::East:  return -GLM_PI_2f;
        case Dir::South: return GLM_PIf;
        case Dir::West:  return GLM_PI_2f;
    }
    return 0.0f;
}
} // namespace

void torch_placement(int col, int row, Dir dir, mat4 out_placement) {
    float dx, dz;
    face_vec(dir, dx, dz);

    // Mount point: the wall cell's center, pushed out to its open-facing face.
    vec3 mount = {
        (col + 0.5f) * TILE + dx * TORCH_WALL_OFFSET,
        TORCH_MOUNT_HEIGHT,
        (row + 0.5f) * TILE + dz * TORCH_WALL_OFFSET,
    };

    // Placement = translate(mount) * yaw(face) * tilt(away from wall) * scale.
    glm_mat4_identity(out_placement);
    glm_translate(out_placement, mount);
    glm_rotate_y(out_placement, face_yaw(dir) + TORCH_YAW_OFFSET, out_placement);
    glm_rotate_x(out_placement, -TORCH_TILT, out_placement);
    vec3 scale = { TORCH_SCALE, TORCH_SCALE, TORCH_SCALE };
    glm_scale(out_placement, scale);
}

} // namespace dc::world
