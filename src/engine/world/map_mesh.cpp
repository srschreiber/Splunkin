#include "engine/world/map_mesh.h"

namespace dc::world {

namespace {

struct Color { float r, g, b; };
constexpr Color FLOOR_COLOR   {0.30f, 0.27f, 0.24f};
constexpr Color CEILING_COLOR {0.18f, 0.18f, 0.22f};
constexpr Color WALL_COLOR    {0.55f, 0.50f, 0.45f};

void push_vertex(std::vector<float>& v,
                 float x, float y, float z,
                 float nx, float ny, float nz,
                 Color c) {
    v.insert(v.end(), {x, y, z, nx, ny, nz, c.r, c.g, c.b});
}

// Emit a quad as two triangles (a,b,c) (a,c,d), winding CCW when viewed from
// the side the normal points toward.
void push_quad(std::vector<float>& v,
               const float a[3], const float b[3],
               const float c[3], const float d[3],
               float nx, float ny, float nz, Color col) {
    push_vertex(v, a[0],a[1],a[2], nx,ny,nz, col);
    push_vertex(v, b[0],b[1],b[2], nx,ny,nz, col);
    push_vertex(v, c[0],c[1],c[2], nx,ny,nz, col);
    push_vertex(v, a[0],a[1],a[2], nx,ny,nz, col);
    push_vertex(v, c[0],c[1],c[2], nx,ny,nz, col);
    push_vertex(v, d[0],d[1],d[2], nx,ny,nz, col);
}

} // namespace

std::vector<float> build_map_mesh(const Map& map) {
    std::vector<float> v;
    const float T = TILE;
    const float H = WALL_HEIGHT;

    for (int row = 0; row < map.height; ++row) {
        for (int col = 0; col < map.width; ++col) {
            const float x0 = col * T,     x1 = (col + 1) * T;
            const float z0 = row * T,     z1 = (row + 1) * T;

            if (map.at(col, row) == Cell::Open) {
                // Floor (normal +Y), CCW viewed from above.
                float fa[3]{x0,0,z0}, fb[3]{x0,0,z1}, fc[3]{x1,0,z1}, fd[3]{x1,0,z0};
                push_quad(v, fa, fb, fc, fd, 0,1,0, FLOOR_COLOR);
                // Ceiling (normal -Y), CCW viewed from below.
                float ca[3]{x0,H,z0}, cb[3]{x1,H,z0}, cc[3]{x1,H,z1}, cd[3]{x0,H,z1};
                push_quad(v, ca, cb, cc, cd, 0,-1,0, CEILING_COLOR);
                continue;
            }

            // Solid cell: emit a wall quad on each side that borders Open.
            // -Z face (toward row-1), normal -Z.
            if (map.at(col, row - 1) == Cell::Open) {
                float a[3]{x1,0,z0}, b[3]{x1,H,z0}, c[3]{x0,H,z0}, d[3]{x0,0,z0};
                push_quad(v, a, d, c, b, 0,0,-1, WALL_COLOR);
            }
            // +Z face (toward row+1), normal +Z.
            if (map.at(col, row + 1) == Cell::Open) {
                float a[3]{x0,0,z1}, b[3]{x0,H,z1}, c[3]{x1,H,z1}, d[3]{x1,0,z1};
                push_quad(v, a, d, c, b, 0,0,1, WALL_COLOR);
            }
            // -X face (toward col-1), normal -X.
            if (map.at(col - 1, row) == Cell::Open) {
                float a[3]{x0,0,z0}, b[3]{x0,H,z0}, c[3]{x0,H,z1}, d[3]{x0,0,z1};
                push_quad(v, a, d, c, b, -1,0,0, WALL_COLOR);
            }
            // +X face (toward col+1), normal +X.
            if (map.at(col + 1, row) == Cell::Open) {
                float a[3]{x1,0,z1}, b[3]{x1,H,z1}, c[3]{x1,H,z0}, d[3]{x1,0,z0};
                push_quad(v, a, d, c, b, 1,0,0, WALL_COLOR);
            }
        }
    }
    return v;
}

} // namespace dc::world
