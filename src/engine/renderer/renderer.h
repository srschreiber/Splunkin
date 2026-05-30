#pragma once
#include <cstdint>

namespace dc::renderer {

struct Renderer {
    uint32_t program = 0;
    uint32_t vao = 0;
    uint32_t vbo = 0;

    // Loads the triangle program and uploads geometry. Returns false on failure.
    bool init();
    void render();
    void shutdown();
};

} // namespace dc::renderer
