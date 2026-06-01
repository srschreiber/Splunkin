#pragma once
#include <vector>
#include <cglm/cglm.h>
#include "engine/renderer/model.h"

namespace dc::renderer {

// Wrapper so matrices can live in a std::vector (cglm's mat4 is a raw array).
struct Mat4 { mat4 m; };

// Computes the world transform of each mesh part at clip time `t` (looped over
// the walk duration). If `animate` is false, returns the rest pose (base TRS).
// `out_part_world` is sized to model.parts.size(); out_part_world[i] is the
// transform for the part whose node has mesh_part == i.
void pose_model(const ModelData& model, float t, bool animate,
                std::vector<Mat4>& out_part_world, float head_pitch = 0.0f);

} // namespace dc::renderer
