#include "engine/world/collision.h"
#include <cmath>

namespace dc::world {

bool circle_hits_solid(const Map& map, float x, float z, float r) {
    const int c0 = static_cast<int>(std::floor((x - r) / TILE));
    const int c1 = static_cast<int>(std::floor((x + r) / TILE));
    const int r0 = static_cast<int>(std::floor((z - r) / TILE));
    const int r1 = static_cast<int>(std::floor((z + r) / TILE));

    for (int row = r0; row <= r1; ++row) {
        for (int col = c0; col <= c1; ++col) {
            dc::world::Cell cell = map.at(col, row);
            if (cell != Cell::Solid) continue;
            const float minx = col * TILE, maxx = (col + 1) * TILE;
            const float minz = row * TILE, maxz = (row + 1) * TILE;
            const float cx = x < minx ? minx : (x > maxx ? maxx : x);
            const float cz = z < minz ? minz : (z > maxz ? maxz : z);
            const float dx = x - cx, dz = z - cz;
            if (dx * dx + dz * dz < r * r) return true;
        }
    }
    return false;
}

} // namespace dc::world
