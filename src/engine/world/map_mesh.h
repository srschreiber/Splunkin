#pragma once
#include <vector>
#include "engine/world/map.h"

namespace dc::world {

// Builds an interleaved vertex array for the map.
// Layout per vertex (9 floats): pos.x,y,z, normal.x,y,z, uv.u,v, layer.
// Triangles, 6 vertices per quad. Floor & ceiling per Open cell; wall faces
// emitted only where a Solid cell borders an Open cell (out-of-bounds = Solid).
std::vector<float> build_map_mesh(const Map& map);

} // namespace dc::world
