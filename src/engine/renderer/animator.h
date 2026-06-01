#pragma once
#include <vector>
#include <cglm/cglm.h>
#include "engine/renderer/model.h"

namespace dc::renderer {

// Wrapper so matrices can live in a std::vector (cglm's mat4 is a raw array).
struct Mat4 { mat4 m; };

// One animation layer to apply. Layers are applied in order; for each bone, the
// last layer that writes it wins. `only_node >= 0` masks the layer to a single
// bone (e.g. punch -> just the arm), so other bones keep the layers below it.
struct AnimLayer {
    const Animation* clip = nullptr;
    float            time = 0.0f;     // seconds into the clip
    int              only_node = -1;  // -1 = all bones the clip animates; else just this bone
    bool             loop = true;     // true: wrap time over duration; false: hold first/last frame
};

// Computes the world transform of each mesh part by:
//   1. starting from the rest pose,
//   2. applying each layer in order (sampling its clip at `time`),
//   3. tilting the head bone by `head_pitch` (head-look).
// With no layers you get the rest pose. `out_part_world` is sized to
// model.parts.size(); out_part_world[i] is the transform for the part whose
// node has mesh_part == i.
//
// Optionally, if `attach_node >= 0` and `out_attach != nullptr`, writes that
// node's world matrix into *out_attach — used as an attachment "socket" (e.g.
// the head bone) to hang separate models (a helmet) off an animated bone.
void pose_model(const ModelData& model, const std::vector<AnimLayer>& layers,
                float head_pitch, std::vector<Mat4>& out_part_world,
                int attach_node = -1, Mat4* out_attach = nullptr);

} // namespace dc::renderer
