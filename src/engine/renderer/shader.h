#pragma once
#include <cstdint>

namespace dc::renderer {

// Compiles and links a program from two GLSL source files. Returns 0 on failure.
uint32_t load_program(const char* vert_path, const char* frag_path);

} // namespace dc::renderer
