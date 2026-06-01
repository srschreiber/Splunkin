#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <cglm/cglm.h>

namespace dc::renderer {

// --- Animation clip (node TRS keyframes) -----------------------------------

enum class AnimPath { Translation, Rotation, Scale };
enum class AnimInterp { Linear, Step };

struct AnimChannel {
    int        node = -1;                  // target node index
    AnimPath   path = AnimPath::Translation;
    AnimInterp interp = AnimInterp::Linear;
    std::vector<float> times;              // keyframe input (seconds), ascending
    std::vector<float> values;             // flat: 3 floats (T/S) or 4 (R) per keyframe
};

struct Animation {
    std::string              name;
    float                    duration = 0.0f;
    std::vector<AnimChannel> channels;
    // A single-keyframe clip (duration 0) is a valid static hold pose.
    bool valid() const { return !channels.empty(); }
};

// --- Scene graph ------------------------------------------------------------

// One node. (t,r,s) is the node's base/local transform; animation overrides it.
// Basically a blender bone
struct Node {
    vec3   t = {0.0f, 0.0f, 0.0f};
    versor r = {0.0f, 0.0f, 0.0f, 1.0f};   // identity quaternion (x,y,z,w)
    vec3   s = {1.0f, 1.0f, 1.0f};
    int    parent = -1;
    int    mesh_part = -1;                 // index into ModelData::parts, or -1
};

// CPU vertex data for one mesh part. Interleaved 8 floats: pos3, normal3, uv2.
struct PartCPU {
    std::vector<float>    vertices;
    std::vector<uint32_t> indices;
};

// GL-free parse result: scene graph + mesh parts + the (first) animation.
// parts[i] is drawn at the world transform of the node whose mesh_part == i.
struct ModelData {
    std::vector<Node>    nodes;
    std::vector<PartCPU> parts;
    Animation            walk;
    Animation            punch;
    Animation           block;  // example of a block animation layer that masks no bones
    Animation            open;   // chest: lid swing (played forward to open)
    Animation            close;  // chest: closed pose (unused for now; open played in reverse closes)
    int head_node  = -1;   // for head-look, the "head" bone
    int arm_l_node = -1;   // the "armL" bone — punch is masked to it so you can walk + punch
    int arm_r_node = -1;   // the "armR" bone — block is masked to it so you can block + punch + walk
};

bool read_model(const char* path, ModelData& out);

// --- GL resource: one indexed mesh per part (same order as ModelData::parts) -

struct PartMesh {
    uint32_t vao = 0, vbo = 0, ebo = 0;
    int index_count = 0;
};
struct Model {
    std::vector<PartMesh> parts;
    void upload(const ModelData& data);
    void destroy();
};

} // namespace dc::renderer
