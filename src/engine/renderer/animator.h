#pragma once
#include <vector>
#include <cglm/cglm.h>
#include "engine/renderer/model.h"

namespace dc::renderer {

// Wrapper so matrices can live in a std::vector (cglm's mat4 is a raw array).
struct Mat4 { mat4 m; };

// One animation layer to apply. Layers are applied in order; for each bone, the
// last layer that writes it wins. `only_node >= 0` masks the layer to that bone
// and its descendants (e.g. armR -> the whole right arm), so bones outside that
// subtree keep the layers below it (punch/block over a walking body).
struct AnimLayer {
    const Animation* clip = nullptr;
    float            time = 0.0f;     // seconds into the clip
    int              only_node = -1;  // -1 = all bones the clip animates; else this bone + its subtree
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
// Optional attachment sockets: for each i, writes the world matrix of bone
// `attach_nodes[i]` into `*out_attach[i]` — used to hang separate models off
// animated bones (helmet -> head, shield -> hand, ...). The two vectors must be
// the same length; pass none for no sockets.
// `body_pitch` (optional) tilts the torso bone the same way, so the arms + held
// weapon aim up/down with the look (used for remote players, whose body is drawn).
void pose_model(const ModelData& model, const std::vector<AnimLayer>& layers,
                float head_pitch, std::vector<Mat4>& out_part_world,
                std::vector<int> attach_nodes = {}, std::vector<Mat4*> out_attach = {},
                float body_pitch = 0.0f,
                const std::vector<float>* bone_scale = nullptr);   // per-node uniform scale, applied AFTER animation

// Per-part local offset of a model's mesh nodes, relative to their parent bone
// (the node's own T*R*S). For equipment hung off a socket bone, draw part i at:
//   placement * boneWorld * offset[i]
// The result is sized to model.parts.size() (identity for any missing part), so
// it can be passed straight to draw_model as the per-part transform.
std::vector<Mat4> mesh_offsets(const ModelData& model);

} // namespace dc::renderer
