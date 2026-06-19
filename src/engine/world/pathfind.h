#pragma once
#include <cstdint>
#include <vector>
#include "engine/world/map.h"

namespace dc::world {

// A breadth-first "distance to goal" field over the map's open cells. dist[i] is
// the number of steps from cell i to the goal (4-connected); -1 = solid or
// unreachable. Enemies descend the gradient to reach the goal. GL-free.
struct FlowField {
    int width = 0, height = 0;
    std::vector<int> dist;   // row-major width*height, -1 = blocked/unreachable

    int at(int col, int row) const {
        if (col < 0 || row < 0 || col >= width || row >= height) return -1;
        return dist[static_cast<std::size_t>(row) * width + col];
    }
};

// Optional terrain awareness for the flow field. When `heights` is non-null it holds a
// row-major width*height grid of each tile's ground elevation (sampled at tile centers).
// Traversal is then DIRECTIONAL: an enemy may DROP any amount (walk/fall off a cliff) but
// may only CLIMB up to `max_climb` world units per tile step — so steep cliffs are one-way
// and enemies route around to a ramp to get up. With `heights == nullptr` the field is a
// plain terrain-agnostic BFS (the original behavior).

// BFS from (goal_col, goal_row) across Open cells.
FlowField compute_flow(const Map& map, int goal_col, int goal_row,
                       const std::vector<float>* heights = nullptr, float max_climb = 1e9f);

// Multi-source BFS from several goal tiles at once (e.g. all players). Each cell's
// distance is to the NEAREST goal, so enemies descending the gradient head for the
// closest player automatically. `cols`/`rows` are parallel arrays of goal tiles.
FlowField compute_flow_multi(const Map& map, const std::vector<int>& cols, const std::vector<int>& rows,
                             const std::vector<float>* heights = nullptr, float max_climb = 1e9f);

// Picks one neighbor of (col,row) whose distance is strictly lower (a step that
// gets closer to the goal), chosen at RANDOM among the tied-best options so a
// crowd of enemies fans out across equal paths instead of single-filing. Returns
// false if there's no downhill neighbor (already at goal / stuck). `rng` is the
// sim's LCG state (advanced); keeping it in the state makes the host's sim the
// sole source of randomness for networking. If `heights` is given, a step is only
// taken when the enemy can actually traverse it (same one-way climb rule as the BFS).
bool flow_step(const FlowField& flow, int col, int row,
               uint32_t& rng, int& out_col, int& out_row,
               const std::vector<float>* heights = nullptr, float max_climb = 1e9f);

} // namespace dc::world
