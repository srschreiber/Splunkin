#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dc::world {

inline constexpr float TILE        = 2.0f;  // world units per tile
inline constexpr float WALL_HEIGHT = 3.0f;  // floor (y=0) to ceiling
inline constexpr float EYE_HEIGHT  = 1.6f;  // camera height above floor

enum class Cell : uint8_t { Open, Solid };

struct Map {
    int width = 0;
    int height = 0;
    std::vector<Cell> cells;  // row-major, size width*height
    int spawn_col = 0;
    int spawn_row = 0;

    // Returns Solid for out-of-bounds (so the world is treated as closed).
    Cell at(int col, int row) const;
};

// Parses an ASCII grid. '#'=Solid, '.'/' '=Open, '@'=Open + spawn (first wins).
// Ragged rows padded with Open to the longest line. Empty input -> nullopt.
// No '@' -> spawn = first Open cell (row-major), or (0,0) if none.
std::optional<Map> parse_map(const std::string& text);

} // namespace dc::world
