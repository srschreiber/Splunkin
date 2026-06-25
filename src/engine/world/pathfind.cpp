#include "engine/world/pathfind.h"
#include <queue>

namespace dc::world {

FlowField compute_flow_multi(const Map& map, const std::vector<int>& cols, const std::vector<int>& rows,
                             const std::vector<float>* heights, float max_climb,
                             const std::vector<int>* cost) {
    FlowField f;
    f.width = map.width;
    f.height = map.height;
    f.dist.assign(static_cast<std::size_t>(map.width) * map.height, -1);

    auto idx = [&](int c, int r) { return static_cast<std::size_t>(r) * f.width + c; };
    // Can an enemy travel FROM cell a TO cell b? Free to drop, capped climbing up.
    auto can_move = [&](int ac, int ar, int bc, int br) {
        if (!heights) return true;
        return (*heights)[idx(bc, br)] - (*heights)[idx(ac, ar)] <= max_climb;
    };
    auto tile_cost = [&](int c, int r) { return cost ? (*cost)[idx(c, r)] : 0; };

    // Dijkstra over step weight (1 + extra cost of the tile being entered). When `cost` is null
    // every weight is 1, so this degrades to the original uniform BFS (just via a heap).
    using Node = std::pair<int, std::pair<int,int>>;   // (dist, (col,row))
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> pq;
    const std::size_t ng = (cols.size() < rows.size()) ? cols.size() : rows.size();
    for (std::size_t i = 0; i < ng; ++i) {   // seed from every goal at distance 0
        const int c = cols[i], r = rows[i];
        if (c < 0 || r < 0 || c >= f.width || r >= f.height) continue;
        if (map.at(c, r) == Cell::Solid) continue;
        if (f.dist[idx(c, r)] != -1) continue;
        f.dist[idx(c, r)] = 0;
        pq.push({0, {c, r}});
    }

    const int dc[4] = { 1, -1, 0, 0 };
    const int dr[4] = { 0, 0, 1, -1 };
    while (!pq.empty()) {
        auto [d, cr] = pq.top(); pq.pop();
        const int c = cr.first, r = cr.second;
        if (d > f.dist[idx(c, r)]) continue;   // stale heap entry
        for (int k = 0; k < 4; ++k) {
            const int nc = c + dc[k], nr = r + dr[k];
            if (nc < 0 || nr < 0 || nc >= f.width || nr >= f.height) continue;
            if (map.at(nc, nr) == Cell::Solid) continue;
            // The field expands outward from the goal, but an actor travels INWARD (nc,nr)->(c,r),
            // so validate THAT direction (one-way climb rule).
            if (!can_move(nc, nr, c, r)) continue;
            const int nd = d + 1 + tile_cost(nc, nr);   // cost to stand on (nc,nr) then step toward goal
            const int cur = f.dist[idx(nc, nr)];
            if (cur == -1 || nd < cur) {
                f.dist[idx(nc, nr)] = nd;
                pq.push({nd, {nc, nr}});
            }
        }
    }
    return f;
}

FlowField compute_flow(const Map& map, int goal_col, int goal_row,
                       const std::vector<float>* heights, float max_climb,
                       const std::vector<int>* cost) {
    return compute_flow_multi(map, { goal_col }, { goal_row }, heights, max_climb, cost);
}

bool flow_step(const FlowField& flow, int col, int row,
               uint32_t& rng, int& out_col, int& out_row,
               const std::vector<float>* heights, float max_climb) {
    const int here = flow.at(col, row);
    if (here <= 0) return false;   // unreachable, or already at the goal

    auto idx = [&](int c, int r) { return static_cast<std::size_t>(r) * flow.width + c; };
    // Descend the gradient: among steppable neighbors that are strictly closer than here, take
    // the LOWEST-distance one(s). (With a uniform field that's just here-1; with a weighted
    // water-cost field it routes down the cheapest direction, around the river.)
    const int dc[4] = { 1, -1, 0, 0 };
    const int dr[4] = { 0, 0, 1, -1 };
    int best_c[4], best_r[4], n = 0, best_d = here;
    for (int k = 0; k < 4; ++k) {
        const int nc = col + dc[k], nr = row + dr[k];
        const int d = flow.at(nc, nr);
        if (d < 0 || d >= here) continue;   // unreachable or not closer
        if (heights && (*heights)[idx(nc, nr)] - (*heights)[idx(col, row)] > max_climb) continue;
        if (d < best_d) { best_d = d; n = 0; }      // new strictly-better tier resets the pool
        if (d == best_d) { best_c[n] = nc; best_r[n] = nr; ++n; }
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
