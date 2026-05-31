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

    std::printf("PASS map\n");
    return 0;
}
