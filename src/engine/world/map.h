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

// Which way a wall torch faces — toward the adjacent open cell. (North = -Z.)
enum class Dir : uint8_t { North, East, South, West };

struct ChestSpawn { int col = 0; int row = 0; };
// A torch mounted on a wall cell, its flame facing `dir` into the open cell there.
struct TorchSpawn { int col = 0; int row = 0; Dir dir = Dir::North; };
struct EnemySpawn { int col = 0; int row = 0; };

struct Map {
    int width = 0;
    int height = 0;
    std::vector<Cell> cells;  // row-major, size width*height
    int spawn_col = 0;
    int spawn_row = 0;
    std::vector<ChestSpawn> chests;    // tiles marked 'C'
    std::vector<TorchSpawn> torches;   // wall cells marked N/E/S/W
    std::vector<EnemySpawn> enemies;   // open tiles marked 'X'

    // Returns Solid for out-of-bounds (so the world is treated as closed).
    Cell at(int col, int row) const;
};

// Parses an ASCII grid. '#'=Solid, '.'/' '=Open, '@'=Open + spawn (first wins),
// 'C'=Solid + chest spawn (blocks movement; opened from an adjacent tile),
// 'X'=Open + enemy spawn. 'N'/'E'/'S'/'W'=Solid wall +
// a torch facing that way (the letter is the direction the flame points, into
// the adjacent open cell).
// Ragged rows padded with Open to the longest line. Empty input -> nullopt.
// No '@' -> spawn = first Open cell (row-major), or (0,0) if none.
std::optional<Map> parse_map(const std::string& text);

} // namespace dc::world
