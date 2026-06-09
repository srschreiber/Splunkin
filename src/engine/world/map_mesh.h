#pragma once
#include <vector>
#include "engine/world/map.h"
#include "engine/world/terrain.h"

namespace dc::world {

// Builds the textured wall mesh (world shader). Layout per vertex (9 floats):
// pos.x,y,z, normal.x,y,z, uv.u,v, layer. Wall faces only — emitted where a Solid
// cell borders an Open cell (out-of-bounds = Solid). The floor is the terrain mesh
// (below) and there's no ceiling (open-top arenas). Walls sit on the ground: each is
// anchored at its Solid cell's terrain height.
std::vector<float> build_map_mesh(const Map& map, const Terrain& terrain);

// Builds the solid-color floor mesh from the height field: each Open (or chest) tile
// is subdivided and its vertices sampled from terrain.height, with flat per-face
// normals for a low-poly faceted look. Same 9-float layout (uv/layer unused; the
// renderer draws it solid-colored). Gives walkable verticality with no texture stretch.
std::vector<float> build_terrain_mesh(const Map& map, const Terrain& terrain);

} // namespace dc::world
