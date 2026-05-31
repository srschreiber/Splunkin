#pragma once
#include <cstdint>
#include <vector>

namespace dc::renderer {

// Owns a VAO + VBO for an interleaved vertex array:
// 9 floats/vertex = pos(3) + normal(3) + color(3).
struct Mesh {
    uint32_t vao = 0;
    uint32_t vbo = 0;
    int vertex_count = 0;

    void upload(const std::vector<float>& interleaved);
    void draw() const;
    void destroy();
};

} // namespace dc::renderer
