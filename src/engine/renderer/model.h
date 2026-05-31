#pragma once
#include <cstdint>
#include <vector>
#include <cglm/cglm.h>

namespace dc::renderer {

// CPU-side, GL-free. Interleaved vertex = pos(3) + normal(3) + uv(2) = 8 floats.
struct PartData {
    std::vector<float>    vertices;
    std::vector<uint32_t> indices;
    mat4                  node_world;   // rest-pose world transform of this part's node
};
struct ModelData {
    std::vector<PartData> parts;
};

// Parses a .glb/.gltf: every node with a mesh becomes one part per primitive,
// reading POSITION/NORMAL/TEXCOORD_0 + indices and the node's world transform.
// Returns false on failure. GL-free.
bool read_model(const char* path, ModelData& out);

// GL resource: one indexed mesh per part. (Defined in model_gl.cpp — later task.)
struct Part {
    uint32_t vao = 0, vbo = 0, ebo = 0;
    int index_count = 0;
    mat4 node_world;
};
struct Model {
    std::vector<Part> parts;
    void upload(const ModelData& data);
    void destroy();
};

} // namespace dc::renderer
