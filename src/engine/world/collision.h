#pragma once
#include "engine/world/map.h"

namespace dc::world {

// True if a circle centered at world (x,z) with radius r overlaps any Solid
// cell. Out-of-bounds counts as Solid (via Map::at).
bool circle_hits_solid(const Map& map, float x, float z, float r);

} // namespace dc::world
