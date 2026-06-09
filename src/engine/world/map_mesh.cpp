#include "engine/world/map_mesh.h"
#include <cmath>

namespace dc::world {

namespace {

constexpr float LAYER_WALL  = 1.0f;
constexpr int   TERRAIN_SUB = 4;   // floor subdivisions per tile (smaller facets = smoother slopes)

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

// One triangle with a computed flat (per-face) normal — for the faceted terrain.
void push_tri(std::vector<float>& out, const float a[3], const float b[3], const float c[3]) {
    const float ux = b[0]-a[0], uy = b[1]-a[1], uz = b[2]-a[2];
    const float vx = c[0]-a[0], vy = c[1]-a[1], vz = c[2]-a[2];
    float nx = uy*vz - uz*vy, ny = uz*vx - ux*vz, nz = ux*vy - uy*vx;
    const float len = std::sqrt(nx*nx + ny*ny + nz*nz);
    if (len > 1e-6f) { nx/=len; ny/=len; nz/=len; } else { nx=0; ny=1; nz=0; }
    push_vertex(out, a[0],a[1],a[2], nx,ny,nz, 0,0, 0);
    push_vertex(out, b[0],b[1],b[2], nx,ny,nz, 0,0, 0);
    push_vertex(out, c[0],c[1],c[2], nx,ny,nz, 0,0, 0);
}

// Chests are Solid (block movement + pathfinding) but render as open floor.
bool is_chest(const Map& map, int c, int r) {
    for (const auto& ch : map.chests) if (ch.col == c && ch.row == r) return true;
    return false;
}
bool renders_open(const Map& map, int c, int r) {
    return map.at(c, r) == Cell::Open || is_chest(map, c, r);
}

} // namespace

std::vector<float> build_map_mesh(const Map& map, const Terrain& terrain) {
    std::vector<float> v;
    const float T = TILE;
    const float H = WALL_HEIGHT;
    const float VWALL = H / T;   // vertical UV repeat up a wall (keeps texels square)

    for (int row = 0; row < map.height; ++row) {
        for (int col = 0; col < map.width; ++col) {
            if (renders_open(map, col, row)) continue;   // floor is the terrain mesh; no ceiling
            const float x0 = col * T, x1 = (col + 1) * T;
            const float z0 = row * T, z1 = (row + 1) * T;
            // Anchor the wall on the ground at this Solid cell's terrain height.
            const float y0 = terrain.height((col + 0.5f) * T, (row + 0.5f) * T);
            const float y1 = y0 + H;

            if (renders_open(map, col, row - 1)) {   // -Z
                float p0[3]{x1,y0,z0}, p1[3]{x0,y0,z0}, p2[3]{x0,y1,z0}, p3[3]{x1,y1,z0};
                push_quad(v, p0,p1,p2,p3, 0,0,-1, 1.0f, VWALL, LAYER_WALL);
            }
            if (renders_open(map, col, row + 1)) {   // +Z
                float p0[3]{x0,y0,z1}, p1[3]{x1,y0,z1}, p2[3]{x1,y1,z1}, p3[3]{x0,y1,z1};
                push_quad(v, p0,p1,p2,p3, 0,0,1, 1.0f, VWALL, LAYER_WALL);
            }
            if (renders_open(map, col - 1, row)) {   // -X
                float p0[3]{x0,y0,z0}, p1[3]{x0,y0,z1}, p2[3]{x0,y1,z1}, p3[3]{x0,y1,z0};
                push_quad(v, p0,p1,p2,p3, -1,0,0, 1.0f, VWALL, LAYER_WALL);
            }
            if (renders_open(map, col + 1, row)) {   // +X
                float p0[3]{x1,y0,z1}, p1[3]{x1,y0,z0}, p2[3]{x1,y1,z0}, p3[3]{x1,y1,z1};
                push_quad(v, p0,p1,p2,p3, 1,0,0, 1.0f, VWALL, LAYER_WALL);
            }
        }
    }
    return v;
}

std::vector<float> build_terrain_mesh(const Map& map, const Terrain& terrain) {
    std::vector<float> v;
    const float T = TILE;
    const float step = T / TERRAIN_SUB;
    auto H = [&](float x, float z) { return terrain.height(x, z); };

    for (int row = 0; row < map.height; ++row) {
        for (int col = 0; col < map.width; ++col) {
            if (!renders_open(map, col, row)) continue;   // floor only under open/chest tiles
            const float bx = col * T, bz = row * T;
            for (int j = 0; j < TERRAIN_SUB; ++j) {
                for (int i = 0; i < TERRAIN_SUB; ++i) {
                    const float sx0 = bx + i * step,     sz0 = bz + j * step;
                    const float sx1 = bx + (i+1) * step, sz1 = bz + (j+1) * step;
                    float c00[3]{ sx0, H(sx0,sz0), sz0 };
                    float c01[3]{ sx0, H(sx0,sz1), sz1 };
                    float c11[3]{ sx1, H(sx1,sz1), sz1 };
                    float c10[3]{ sx1, H(sx1,sz0), sz0 };
                    push_tri(v, c00, c01, c11);   // CCW from above -> +Y-ish normals
                    push_tri(v, c00, c11, c10);
                }
            }
        }
    }
    return v;
}

} // namespace dc::world
