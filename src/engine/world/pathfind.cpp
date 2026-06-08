#include "engine/world/pathfind.h"
#include <queue>

namespace dc::world {

FlowField compute_flow_multi(const Map& map, const std::vector<int>& cols, const std::vector<int>& rows) {
    FlowField f;
    f.width = map.width;
    f.height = map.height;
    f.dist.assign(static_cast<std::size_t>(map.width) * map.height, -1);

    auto idx = [&](int c, int r) { return static_cast<std::size_t>(r) * f.width + c; };
    std::queue<std::pair<int,int>> q;
    const std::size_t ng = (cols.size() < rows.size()) ? cols.size() : rows.size();
    for (std::size_t i = 0; i < ng; ++i) {   // seed BFS from every goal at distance 0
        const int c = cols[i], r = rows[i];
        if (c < 0 || r < 0 || c >= f.width || r >= f.height) continue;
        if (map.at(c, r) == Cell::Solid) continue;
        if (f.dist[idx(c, r)] != -1) continue;
        f.dist[idx(c, r)] = 0;
        q.push({c, r});
    }

    const int dc[4] = { 1, -1, 0, 0 };
    const int dr[4] = { 0, 0, 1, -1 };
    while (!q.empty()) {
        auto [c, r] = q.front(); q.pop();
        const int d = f.dist[idx(c, r)];
        for (int k = 0; k < 4; ++k) {
            const int nc = c + dc[k], nr = r + dr[k];
            if (nc < 0 || nr < 0 || nc >= f.width || nr >= f.height) continue;
            if (map.at(nc, nr) == Cell::Solid) continue;
            if (f.dist[idx(nc, nr)] != -1) continue;   // already visited
            f.dist[idx(nc, nr)] = d + 1;
            q.push({nc, nr});
        }
    }
    return f;
}

FlowField compute_flow(const Map& map, int goal_col, int goal_row) {
    return compute_flow_multi(map, { goal_col }, { goal_row });
}

bool flow_step(const FlowField& flow, int col, int row,
               uint32_t& rng, int& out_col, int& out_row) {
    const int here = flow.at(col, row);
    if (here <= 0) return false;   // unreachable, or already at the goal

    // Collect the neighbors that are strictly closer (here - 1).
    const int dc[4] = { 1, -1, 0, 0 };
    const int dr[4] = { 0, 0, 1, -1 };
    int best_c[4], best_r[4], n = 0;
    for (int k = 0; k < 4; ++k) {
        const int nc = col + dc[k], nr = row + dr[k];
        const int d = flow.at(nc, nr);
        if (d == here - 1) { best_c[n] = nc; best_r[n] = nr; ++n; }
    }
    if (n == 0) return false;

    // Pick one at random (LCG), so crowds spread across equal-length routes.
    rng = rng * 1664525u + 1013904223u;
    const int pick = static_cast<int>((rng >> 8) % static_cast<uint32_t>(n));
    out_col = best_c[pick];
    out_row = best_r[pick];
    return true;
}

} // namespace dc::world
