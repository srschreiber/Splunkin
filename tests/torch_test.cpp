#include "engine/world/torch.h"
#include "engine/world/map.h"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace dc::world;

int main() {
    // A torch on cell (3,3) facing North: its placement should sit at the cell's
    // XZ, pushed toward -Z (the open cell above), at the mount height.
    mat4 place;
    torch_placement(3, 3, Dir::North, place);

    const float cx = (3 + 0.5f) * TILE;
    const float cz = (3 + 0.5f) * TILE;
    // The translation column (place[3]) is the mount point.
    assert(std::fabs(place[3][0] - cx) < 1e-4f);          // centered on the cell in X
    assert(place[3][2] < cz);                             // pushed toward the open (-Z) side
    assert(std::fabs(place[3][1] - TORCH_MOUNT_HEIGHT) < 1e-4f);

    // Facing East pushes the mount toward +X; West toward -X.
    mat4 pe, pw;
    torch_placement(3, 3, Dir::East, pe);
    torch_placement(3, 3, Dir::West, pw);
    assert(pe[3][0] > cx);
    assert(pw[3][0] < cx);

    std::printf("PASS torch\n");
    return 0;
}
