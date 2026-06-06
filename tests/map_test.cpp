#include "engine/world/map.h"
#include <cassert>
#include <cstdio>

using namespace dc::world;

int main() {
    // Basic grid with spawn and a ragged (short) middle row.
    const std::string text =
        "###\n"
        "#@\n"          // ragged: only 2 chars; should pad to width 3 with Open
        "#.#\n";
    auto m = parse_map(text);
    assert(m.has_value());
    assert(m->width == 3);
    assert(m->height == 3);
    assert(m->at(0, 0) == Cell::Solid);
    assert(m->at(1, 0) == Cell::Solid);
    assert(m->at(1, 1) == Cell::Open);   // the '@'
    assert(m->at(2, 1) == Cell::Open);   // padded
    assert(m->spawn_col == 1 && m->spawn_row == 1);
    assert(m->at(2, 2) == Cell::Solid);

    // Out of bounds is Solid.
    assert(m->at(-1, 0) == Cell::Solid);
    assert(m->at(0, 99) == Cell::Solid);

    // No '@' -> spawn is first Open cell (row-major).
    auto m2 = parse_map("##\n#.\n");
    assert(m2.has_value());
    assert(m2->spawn_col == 1 && m2->spawn_row == 1);

    // Empty input -> nullopt.
    assert(!parse_map("").has_value());
    assert(!parse_map("\n\n").has_value());

    // 'C' is a Solid tile (blocks movement) that also records a chest spawn.
    auto m3 = parse_map("##\n#C\n");
    assert(m3.has_value());
    assert(m3->at(1, 1) == Cell::Solid);    // chest tile blocks movement
    assert(m3->chests.size() == 1);
    assert(m3->chests[0].col == 1 && m3->chests[0].row == 1);

    // N/E/S/W are Solid wall cells that each record a torch facing that way.
    auto m4 = parse_map(
        "NESW\n"
        "....\n");
    assert(m4.has_value());
    assert(m4->at(0, 0) == Cell::Solid);    // torch cells are walls
    assert(m4->at(3, 0) == Cell::Solid);
    assert(m4->torches.size() == 4);
    assert(m4->torches[0].dir == Dir::North && m4->torches[0].col == 0);
    assert(m4->torches[1].dir == Dir::East);
    assert(m4->torches[2].dir == Dir::South);
    assert(m4->torches[3].dir == Dir::West && m4->torches[3].col == 3);

    std::printf("PASS map\n");
    return 0;
}
