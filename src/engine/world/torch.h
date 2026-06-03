#pragma once
#include <cglm/cglm.h>
#include "engine/world/map.h"

namespace dc::world {

// Placement tuning for wall torches (mirrors the MODEL_* constants in main.cpp;
// tune against the authored torch.glb).
inline constexpr float TORCH_SCALE        = 0.5f;               // shrink the (tall) model to fit the room
inline constexpr float TORCH_MOUNT_HEIGHT = 1.0f;               // world Y of the model origin on the wall
inline constexpr float TORCH_WALL_OFFSET  = TILE * 0.5f;        // out from cell center to the wall face
inline constexpr float TORCH_TILT         = 0.2f;               // radians: tip the flame away from the wall
inline constexpr float TORCH_YAW_OFFSET   = 0.0f;               // correct for the model's authored facing

// Computes a wall torch's world placement matrix (translate * yaw * tilt * scale).
// The flame point for the light + particles is derived separately from the model's
// emissive geometry (see main.cpp). GL-free / testable.
void torch_placement(int col, int row, Dir dir, mat4 out_placement);

} // namespace dc::world
