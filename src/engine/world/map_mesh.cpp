#include "engine/world/map_mesh.h"

namespace dc::world {

namespace {

constexpr float LAYER_FLOOR   = 0.0f;
constexpr float LAYER_WALL    = 1.0f;
constexpr float LAYER_CEILING = 2.0f;

void push_vertex(std::vector<float>& out,
                 float x, float y, float z,
                 float nx, float ny, float nz,
                 float u, float v, float layer) {
    out.insert(out.end(), {x, y, z, nx, ny, nz, u, v, layer});
}

// Quad with corners p0..p3 in CCW order as seen from the +normal side.
// UVs: p0=(0,0), p1=(umax,0), p2=(umax,vmax), p3=(0,vmax).
// Triangles: (p0,p1,p2) and (p0,p2,p3).
void push_quad(std::vector<float>& out,
               const float p0[3], const float p1[3], const float p2[3], const float p3[3],
               float nx, float ny, float nz, float umax, float vmax, float layer) {
    push_vertex(out, p0[0],p0[1],p0[2], nx,ny,nz, 0.0f, 0.0f, layer);
    push_vertex(out, p1[0],p1[1],p1[2], nx,ny,nz, umax, 0.0f, layer);
    push_vertex(out, p2[0],p2[1],p2[2], nx,ny,nz, umax, vmax, layer);
    push_vertex(out, p0[0],p0[1],p0[2], nx,ny,nz, 0.0f, 0.0f, layer);
    push_vertex(out, p2[0],p2[1],p2[2], nx,ny,nz, umax, vmax, layer);
    push_vertex(out, p3[0],p3[1],p3[2], nx,ny,nz, 0.0f, vmax, layer);
}

} // namespace

std::vector<float> build_map_mesh(const Map& map) {
    std::vector<float> v;
    const float T = TILE;
    const float H = WALL_HEIGHT;
    const float VWALL = H / T;   // vertical UV repeat up a wall (keeps texels square)

    // Chests are Solid (block movement + pathfinding) but should NOT render as
    // walls — treat their tiles as open floor for meshing only.
    auto is_chest = [&](int c, int r) {
        for (const auto& ch : map.chests) if (ch.col == c && ch.row == r) return true;
        return false;
    };
    auto renders_open = [&](int c, int r) {
        return map.at(c, r) == Cell::Open || is_chest(c, r);
    };

    for (int row = 0; row < map.height; ++row) {
        for (int col = 0; col < map.width; ++col) {
            const float x0 = col * T, x1 = (col + 1) * T;
            const float z0 = row * T, z1 = (row + 1) * T;

            if (renders_open(col, row)) {
                float f0[3]{x0,0,z0}, f1[3]{x0,0,z1}, f2[3]{x1,0,z1}, f3[3]{x1,0,z0};
                push_quad(v, f0,f1,f2,f3, 0,1,0, 1.0f, 1.0f, LAYER_FLOOR);
                float c0[3]{x0,H,z0}, c1[3]{x1,H,z0}, c2[3]{x1,H,z1}, c3[3]{x0,H,z1};
                push_quad(v, c0,c1,c2,c3, 0,-1,0, 1.0f, 1.0f, LAYER_CEILING);
                continue;
            }

            if (renders_open(col, row - 1)) {   // -Z
                float p0[3]{x1,0,z0}, p1[3]{x0,0,z0}, p2[3]{x0,H,z0}, p3[3]{x1,H,z0};
                push_quad(v, p0,p1,p2,p3, 0,0,-1, 1.0f, VWALL, LAYER_WALL);
            }
            if (renders_open(col, row + 1)) {   // +Z
                float p0[3]{x0,0,z1}, p1[3]{x1,0,z1}, p2[3]{x1,H,z1}, p3[3]{x0,H,z1};
                push_quad(v, p0,p1,p2,p3, 0,0,1, 1.0f, VWALL, LAYER_WALL);
            }
            if (renders_open(col - 1, row)) {   // -X
                float p0[3]{x0,0,z0}, p1[3]{x0,0,z1}, p2[3]{x0,H,z1}, p3[3]{x0,H,z0};
                push_quad(v, p0,p1,p2,p3, -1,0,0, 1.0f, VWALL, LAYER_WALL);
            }
            if (renders_open(col + 1, row)) {   // +X
                float p0[3]{x1,0,z1}, p1[3]{x1,0,z0}, p2[3]{x1,H,z0}, p3[3]{x1,H,z1};
                push_quad(v, p0,p1,p2,p3, 1,0,0, 1.0f, VWALL, LAYER_WALL);
            }
        }
    }
    return v;
}

} // namespace dc::world
