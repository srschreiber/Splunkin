#pragma once
#include <cstdint>

namespace dc::renderer {

// Loads `count` same-sized RGBA images into a GL_TEXTURE_2D_ARRAY (layer i = paths[i]).
// Nearest filtering, GL_REPEAT wrap. Returns the GL texture id, or 0 on failure.
uint32_t load_texture_array(const char* const* paths, int count);

} // namespace dc::renderer
