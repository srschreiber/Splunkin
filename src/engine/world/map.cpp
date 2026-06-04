#include "engine/world/map.h"
#include <algorithm>

namespace dc::world {

Cell Map::at(int col, int row) const {
    if (col < 0 || row < 0 || col >= width || row >= height) return Cell::Solid;
    return cells[static_cast<std::size_t>(row) * width + col];
}

std::optional<Map> parse_map(const std::string& text) {
    // Split into lines, stripping trailing '\r'. Track the last non-blank line
    // so trailing blank lines are ignored.
    std::vector<std::string> lines;
    std::string cur;
    int last_nonblank = -1;
    auto flush = [&]() {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        if (!cur.empty()) last_nonblank = static_cast<int>(lines.size());
        lines.push_back(cur);
        cur.clear();
    };
    for (char c : text) {
        if (c == '\n') flush();
        else cur.push_back(c);
    }
    flush();  // final line (no trailing newline)

    if (last_nonblank < 0) return std::nullopt;  // no content
    lines.resize(last_nonblank + 1);             // drop trailing blanks

    int width = 0;
    for (const auto& l : lines) width = std::max(width, static_cast<int>(l.size()));
    if (width == 0) return std::nullopt;

    Map m;
    m.width = width;
    m.height = static_cast<int>(lines.size());
    m.cells.assign(static_cast<std::size_t>(width) * m.height, Cell::Open);

    bool spawn_set = false;
    for (int row = 0; row < m.height; ++row) {
        const std::string& l = lines[row];
        for (int col = 0; col < width; ++col) {
            char c = (col < static_cast<int>(l.size())) ? l[col] : ' ';
            // N/E/S/W are wall cells that also carry a torch, so they're Solid.
            bool is_torch = (c == 'N' || c == 'E' || c == 'S' || c == 'W');
            Cell cell = (c == '#' || is_torch) ? Cell::Solid : Cell::Open;
            m.cells[static_cast<std::size_t>(row) * width + col] = cell;
            if (c == '@' && !spawn_set) {
                m.spawn_col = col;
                m.spawn_row = row;
                spawn_set = true;
            } else if (c == 'C') {
                m.chests.push_back({col, row});
            } else if (c == 'X') {
                m.enemies.push_back({col, row});
            } else if (is_torch) {
                Dir d = (c == 'N') ? Dir::North : (c == 'E') ? Dir::East
                      : (c == 'S') ? Dir::South : Dir::West;
                m.torches.push_back({col, row, d});
            }
        }
    }

    if (!spawn_set) {
        for (int row = 0; row < m.height && !spawn_set; ++row)
            for (int col = 0; col < width && !spawn_set; ++col)
                if (m.at(col, row) == Cell::Open) {
                    m.spawn_col = col; m.spawn_row = row; spawn_set = true;
                }
    }
    return m;
}

} // namespace dc::world
