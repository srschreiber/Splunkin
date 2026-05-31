# ASCII Map → 3D World + FPS Camera Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load an ASCII tile-grid map, generate floor/ceiling/pillar geometry as one static mesh, and fly around it with an FPS WASD + mouse-look camera — replacing the hello-triangle.

**Architecture:** Pure GL-free logic (map parse, mesh build, camera math) is unit-tested; GL resources (mesh, renderer, window) and integration are smoke-tested. Map → vertices (interleaved pos/normal/color) → one VBO → one draw call per frame with a view-projection matrix and a single directional light.

**Tech Stack:** C++17, clang++, SDL3, GLAD (gl core 3.3), cglm (header-only). New `make test` target with a custom `<cassert>`-based runner.

---

## Methodology note

`map`, `map_mesh`, and `camera` are GL-free and get genuine TDD (failing test →
implement → pass). GL units and the wired loop keep `--smoke` verification (one
rendered frame, exit 0). The test harness is built first (Task 1) so later tasks
have something to run tests against.

## File Structure

- `tests/*_test.cpp` + `scripts/test.sh` + `Makefile` `test` target — unit harness (Task 1).
- `src/engine/world/map.{h,cpp}` — `Cell`, `Map`, `parse_map` (Task 2).
- `src/engine/world/map_mesh.{h,cpp}` — `build_map_mesh` (Task 3).
- `src/engine/renderer/camera.{h,cpp}` — FPS camera math (Task 4).
- `src/engine/input/input.{h,cpp}` — keyboard/mouse input (Task 5).
- `src/engine/platform/window.{h,cpp}` — depth buffer, relative mouse, `pump_events(Input&)`, `framebuffer_size` (Task 6).
- `src/engine/renderer/mesh.{h,cpp}` — GL VAO/VBO resource (Task 7).
- `assets/shaders/world.{vert,frag}` + `src/engine/renderer/renderer.{h,cpp}` rewrite (Task 8).
- `assets/maps/test.txt` + `src/main.cpp` rewrite (Task 9).
- Delete `assets/shaders/tri.{vert,frag}` (Task 8).

---

### Task 1: Unit test harness (`make test`)

**Files:**
- Create: `tests/sanity_test.cpp`
- Create: `scripts/test.sh`
- Modify: `Makefile`

- [ ] **Step 1: Write a trivial failing test**

`tests/sanity_test.cpp`:
```cpp
#include <cassert>
#include <cstdio>

int main() {
    assert(2 + 2 == 4);
    std::printf("PASS sanity\n");
    return 0;
}
```

- [ ] **Step 2: Write `scripts/test.sh`**

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
TP="$ROOT/third_party"
OUT="$ROOT/build/tests"
mkdir -p "$OUT"

CXX=${CXX:-clang++}
STD="-std=c++17"
INC="-I$ROOT/src -I$TP -I$TP/install/include"

# Each *_test.cpp is compiled with the module sources it needs.
# Modules under test are GL-free, so no SDL/GL linkage is required.
declare -a EXTRA  # per-test extra sources, set below
build_and_run() { # test_file extra_srcs...
  local tf="$1"; shift
  local name; name="$(basename "$tf" .cpp)"
  local bin="$OUT/$name"
  "$CXX" $STD $INC "$tf" "$@" -o "$bin"
  "$bin"
}

build_and_run "$ROOT/tests/sanity_test.cpp"

echo "all tests passed"
```

- [ ] **Step 3: Add `test` target to `Makefile`** (TAB-indented recipe)

Add these two lines to `.PHONY` and a target. The `.PHONY` line becomes:
```makefile
.PHONY: all setup build run clean test
```
And add at the end of the file:
```makefile
test:
	./scripts/test.sh
```

- [ ] **Step 4: Run it — verify pass**

Run: `chmod +x scripts/test.sh && make test`
Expected: `PASS sanity` then `all tests passed`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add tests/sanity_test.cpp scripts/test.sh Makefile
git commit -m "test: add minimal unit test harness and make test target"
```

---

### Task 2: Map parsing (`world/map`)

**Files:**
- Create: `src/engine/world/map.h`
- Create: `src/engine/world/map.cpp`
- Create: `tests/map_test.cpp`
- Modify: `scripts/test.sh`

- [ ] **Step 1: Write `src/engine/world/map.h`**

```cpp
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
```

- [ ] **Step 2: Write `tests/map_test.cpp` (failing test)**

```cpp
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
```

- [ ] **Step 3: Add map_test to `scripts/test.sh`**

Insert before the `echo "all tests passed"` line:
```bash
build_and_run "$ROOT/tests/map_test.cpp" "$ROOT/src/engine/world/map.cpp"
```

- [ ] **Step 4: Run — verify it FAILS to link/compile**

Run: `make test`
Expected: FAIL — `map.cpp` doesn't exist yet / undefined `parse_map`.

- [ ] **Step 5: Write `src/engine/world/map.cpp`**

```cpp
#include "engine/world/map.h"

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
            Cell cell = (c == '#') ? Cell::Solid : Cell::Open;
            m.cells[static_cast<std::size_t>(row) * width + col] = cell;
            if (c == '@' && !spawn_set) {
                m.spawn_col = col;
                m.spawn_row = row;
                spawn_set = true;
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
```
(Requires `#include <algorithm>` for `std::max`; add it to the includes.)

- [ ] **Step 6: Run — verify PASS**

Run: `make test`
Expected: `PASS map`, `PASS sanity`, `all tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/engine/world/map.h src/engine/world/map.cpp tests/map_test.cpp scripts/test.sh
git commit -m "feat(world): ASCII map parser with unit tests"
```

---

### Task 3: Map mesh generation (`world/map_mesh`)

**Files:**
- Create: `src/engine/world/map_mesh.h`
- Create: `src/engine/world/map_mesh.cpp`
- Create: `tests/map_mesh_test.cpp`
- Modify: `scripts/test.sh`

- [ ] **Step 1: Write `src/engine/world/map_mesh.h`**

```cpp
#pragma once
#include <vector>
#include "engine/world/map.h"

namespace dc::world {

// Builds an interleaved vertex array for the map.
// Layout per vertex (9 floats): pos.x,y,z, normal.x,y,z, color.r,g,b.
// Triangles, 6 vertices per quad. Floor & ceiling per Open cell; wall faces
// emitted only where a Solid cell borders an Open cell (out-of-bounds = Solid).
std::vector<float> build_map_mesh(const Map& map);

} // namespace dc::world
```

- [ ] **Step 2: Write `tests/map_mesh_test.cpp` (failing test)**

```cpp
#include "engine/world/map_mesh.h"
#include "engine/world/map.h"
#include <cassert>
#include <cstdio>

using namespace dc::world;

int main() {
    // 3x3 with a single Solid cell in the center, surrounded by Open.
    auto m = parse_map(
        "...\n"
        ".#.\n"
        "...\n");
    assert(m.has_value());

    auto verts = build_map_mesh(*m);

    // 8 Open cells -> 8 floor + 8 ceiling quads. Center Solid has 4 open
    // neighbors -> 4 wall quads. Total 20 quads * 6 verts * 9 floats.
    const std::size_t expected = (8 + 8 + 4) * 6 * 9;
    assert(verts.size() == expected);

    // A fully solid map produces no geometry (no open cells, no exposed faces
    // since every solid neighbor is solid; out-of-bounds is solid too).
    auto solid = parse_map("##\n##\n");
    assert(solid.has_value());
    assert(build_map_mesh(*solid).empty());

    // A single Open cell surrounded by out-of-bounds (1x1 ".") -> 1 floor +
    // 1 ceiling, and 0 walls (no solid cells exist). 2 quads.
    auto one = parse_map(".\n");
    assert(one.has_value());
    assert(build_map_mesh(*one).size() == 2 * 6 * 9);

    std::printf("PASS map_mesh\n");
    return 0;
}
```

- [ ] **Step 3: Add map_mesh_test to `scripts/test.sh`**

Insert before `echo "all tests passed"`:
```bash
build_and_run "$ROOT/tests/map_mesh_test.cpp" \
  "$ROOT/src/engine/world/map_mesh.cpp" "$ROOT/src/engine/world/map.cpp"
```

- [ ] **Step 4: Run — verify it FAILS**

Run: `make test`
Expected: FAIL — `map_mesh.cpp` missing / undefined `build_map_mesh`.

- [ ] **Step 5: Write `src/engine/world/map_mesh.cpp`**

```cpp
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
                push_quad(v, a, b, c, d, 0,0,-1, WALL_COLOR);
            }
            // +Z face (toward row+1), normal +Z.
            if (map.at(col, row + 1) == Cell::Open) {
                float a[3]{x0,0,z1}, b[3]{x0,H,z1}, c[3]{x1,H,z1}, d[3]{x1,0,z1};
                push_quad(v, a, b, c, d, 0,0,1, WALL_COLOR);
            }
            // -X face (toward col-1), normal -X.
            if (map.at(col - 1, row) == Cell::Open) {
                float a[3]{x0,0,z0}, b[3]{x0,H,z0}, c[3]{x0,H,z1}, d[3]{x0,0,z1};
                push_quad(v, a, b, c, d, -1,0,0, WALL_COLOR);
            }
            // +X face (toward col+1), normal +X.
            if (map.at(col + 1, row) == Cell::Open) {
                float a[3]{x1,0,z1}, b[3]{x1,H,z1}, c[3]{x1,H,z0}, d[3]{x1,0,z0};
                push_quad(v, a, b, c, d, 1,0,0, WALL_COLOR);
            }
        }
    }
    return v;
}

} // namespace dc::world
```

- [ ] **Step 6: Run — verify PASS**

Run: `make test`
Expected: `PASS map_mesh` (plus prior PASS lines), `all tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/engine/world/map_mesh.h src/engine/world/map_mesh.cpp tests/map_mesh_test.cpp scripts/test.sh
git commit -m "feat(world): map mesh generation with face culling and unit tests"
```

---

### Task 4: FPS camera (`renderer/camera`)

**Files:**
- Create: `src/engine/renderer/camera.h`
- Create: `src/engine/renderer/camera.cpp`
- Create: `tests/camera_test.cpp`
- Modify: `scripts/test.sh`

- [ ] **Step 1: Write `src/engine/renderer/camera.h`**

```cpp
#pragma once
#include <cglm/cglm.h>

namespace dc::renderer {

struct Camera {
    vec3  position = {0.0f, 0.0f, 0.0f};
    float yaw   = 0.0f;            // radians
    float pitch = 0.0f;            // radians, clamped to +-89 degrees
    float fov_y = 1.2217305f;      // 70 degrees in radians
    float move_speed = 4.0f;       // units / second
    float mouse_sens = 0.0025f;    // radians / pixel

    // Full look direction unit vector from yaw/pitch.
    void front(vec3 out) const;

    // Apply a mouse delta (pixels). yaw += dx*sens; pitch -= dy*sens; clamp pitch.
    void look(float dx, float dy);

    // Move in the XZ plane: forward along horizontal look dir, strafe to the
    // right. Y (height) is left unchanged. amounts are typically -1, 0, or 1.
    void move(float forward, float strafe, float dt);

    void view_matrix(mat4 out) const;
    void proj_matrix(mat4 out, float aspect) const;
};

} // namespace dc::renderer
```

- [ ] **Step 2: Write `tests/camera_test.cpp` (failing test)**

```cpp
#include "engine/renderer/camera.h"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace dc::renderer;

static bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

int main() {
    // Default yaw=0,pitch=0 -> front points along +X.
    {
        Camera c;
        vec3 f; c.front(f);
        assert(approx(f[0], 1.0f) && approx(f[1], 0.0f) && approx(f[2], 0.0f));
    }

    // Pitch clamps to +-89 degrees (~1.55334 rad).
    {
        Camera c;
        c.look(0.0f, -100000.0f);  // huge upward delta
        assert(c.pitch <= glm_rad(89.0f) + 1e-3f);
        c.look(0.0f,  200000.0f);  // huge downward delta
        assert(c.pitch >= -glm_rad(89.0f) - 1e-3f);
    }

    // Moving forward with yaw=0 advances +X by move_speed*dt, leaves Y fixed.
    {
        Camera c;
        c.position[1] = 1.6f;
        c.move(1.0f, 0.0f, 0.5f);   // dt=0.5, speed=4 -> +2 along +X
        assert(approx(c.position[0], 2.0f));
        assert(approx(c.position[1], 1.6f));   // height unchanged
        assert(approx(c.position[2], 0.0f));
    }

    // Strafing right with yaw=0 (front=+X, up=+Y): right = front x up = +Z... 
    // we define right = normalize(cross(front, up)); cross(+X,+Y) = +Z? No:
    // cross(+X,+Y) = +Z. So positive strafe moves +Z.
    {
        Camera c;
        c.move(0.0f, 1.0f, 0.5f);   // +2 along right
        assert(approx(c.position[2], 2.0f));
        assert(approx(c.position[0], 0.0f));
    }

    std::printf("PASS camera\n");
    return 0;
}
```

- [ ] **Step 3: Add camera_test to `scripts/test.sh`**

Insert before `echo "all tests passed"`:
```bash
build_and_run "$ROOT/tests/camera_test.cpp" "$ROOT/src/engine/renderer/camera.cpp"
```

- [ ] **Step 4: Run — verify it FAILS**

Run: `make test`
Expected: FAIL — `camera.cpp` missing / undefined symbols.

- [ ] **Step 5: Write `src/engine/renderer/camera.cpp`**

```cpp
#include "engine/renderer/camera.h"
#include <cmath>

namespace dc::renderer {

void Camera::front(vec3 out) const {
    out[0] = std::cos(pitch) * std::cos(yaw);
    out[1] = std::sin(pitch);
    out[2] = std::cos(pitch) * std::sin(yaw);
    glm_vec3_normalize(out);
}

void Camera::look(float dx, float dy) {
    yaw   += dx * mouse_sens;
    pitch -= dy * mouse_sens;
    const float limit = glm_rad(89.0f);
    if (pitch >  limit) pitch =  limit;
    if (pitch < -limit) pitch = -limit;
}

void Camera::move(float forward, float strafe, float dt) {
    vec3 up = {0.0f, 1.0f, 0.0f};

    // Horizontal forward (ignore pitch so walking stays level).
    vec3 walk = { std::cos(yaw), 0.0f, std::sin(yaw) };
    glm_vec3_normalize(walk);

    // Right = normalize(cross(walk, up)).
    vec3 right;
    glm_vec3_cross(walk, up, right);
    glm_vec3_normalize(right);

    vec3 delta = {0, 0, 0};
    vec3 tmp;
    glm_vec3_scale(walk,  forward, tmp); glm_vec3_add(delta, tmp, delta);
    glm_vec3_scale(right, strafe,  tmp); glm_vec3_add(delta, tmp, delta);

    glm_vec3_scale(delta, move_speed * dt, delta);
    position[0] += delta[0];
    position[2] += delta[2];   // Y intentionally unchanged (locked eye height)
}

void Camera::view_matrix(mat4 out) const {
    vec3 f; front(f);
    vec3 up = {0.0f, 1.0f, 0.0f};
    glm_look(const_cast<float*>(position), f, up, out);
}

void Camera::proj_matrix(mat4 out, float aspect) const {
    glm_perspective(fov_y, aspect, 0.05f, 100.0f, out);
}

} // namespace dc::renderer
```

- [ ] **Step 6: Run — verify PASS**

Run: `make test`
Expected: `PASS camera` (plus prior PASS lines), `all tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/engine/renderer/camera.h src/engine/renderer/camera.cpp tests/camera_test.cpp scripts/test.sh
git commit -m "feat(renderer): FPS camera math with unit tests"
```

---

### Task 5: Input subsystem (`input/input`)

**Files:**
- Create: `src/engine/input/input.h` (replaces the stub `input.h`)
- Create: `src/engine/input/input.cpp`

(No unit test: this wraps SDL state directly and is exercised by the smoke run.)

- [ ] **Step 1: Verify the stub is the current content (before state)**

Run: `cat src/engine/input/input.h`
Expected: the old stub (`struct Input { bool init(); void shutdown(); };`).

- [ ] **Step 2: Overwrite `src/engine/input/input.h`**

```cpp
#pragma once

union SDL_Event;

namespace dc::input {

// Per-frame input snapshot. Fed by the platform layer each frame.
struct Input {
    float mouse_dx = 0.0f;   // accumulated relative motion this frame
    float mouse_dy = 0.0f;
    bool  quit = false;

    void begin_frame();                  // zero mouse deltas (call before polling)
    void on_event(const SDL_Event& e);   // accumulate motion; set quit on QUIT/Esc
    bool key_down(int scancode) const;   // SDL_Scancode; wraps SDL_GetKeyboardState
};

} // namespace dc::input
```

- [ ] **Step 3: Write `src/engine/input/input.cpp`**

```cpp
#include "engine/input/input.h"
#include <SDL3/SDL.h>

namespace dc::input {

void Input::begin_frame() {
    mouse_dx = 0.0f;
    mouse_dy = 0.0f;
}

void Input::on_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_QUIT) {
        quit = true;
    } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
        quit = true;
    } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
        mouse_dx += e.motion.xrel;
        mouse_dy += e.motion.yrel;
    }
}

bool Input::key_down(int scancode) const {
    const bool* state = SDL_GetKeyboardState(nullptr);
    return state[scancode];
}

} // namespace dc::input
```

- [ ] **Step 4: Verify it compiles** (no test target yet; syntax-check against SDL)

Run:
```bash
clang++ -std=c++17 -Isrc -Ithird_party/install/include -fsyntax-only src/engine/input/input.cpp && echo OK
```
Expected: `OK`. (If `SDL_GetKeyboardState` returns `const Uint8*` instead of `const bool*` in this SDL3 build, adjust the pointer type to match the header and note it. SDL3 uses `const bool*`.)

- [ ] **Step 5: Commit**

```bash
git add src/engine/input/input.h src/engine/input/input.cpp
git commit -m "feat(input): keyboard/mouse input snapshot replacing stub"
```

---

### Task 6: Platform changes — depth, relative mouse, Input feed

**Files:**
- Modify: `src/engine/platform/window.h`
- Modify: `src/engine/platform/window.cpp`

- [ ] **Step 1: Update `src/engine/platform/window.h`**

Replace the whole file with:
```cpp
#pragma once
#include <cstdint>

struct SDL_Window;

namespace dc::input { struct Input; }

namespace dc::platform {

struct Window {
    SDL_Window* sdl_window = nullptr;
    void*       gl_context = nullptr; // SDL_GLContext
    int         width = 1280;
    int         height = 720;

    // Initializes SDL video, creates the window and a GL 3.3 core context with
    // a depth buffer, loads GL via GLAD, and enables relative mouse mode.
    bool init(const char* title);

    // Polls events into `input`; returns false when quit was requested.
    bool pump_events(dc::input::Input& input);

    // Current framebuffer size in pixels (Retina-correct).
    void framebuffer_size(int& w, int& h) const;

    void swap();
    void shutdown();
};

} // namespace dc::platform
```

- [ ] **Step 2: Update `src/engine/platform/window.cpp`**

Replace the whole file with:
```cpp
#include "engine/platform/window.h"
#include "engine/input/input.h"

#include <SDL3/SDL.h>
#include <glad/gl.h>
#include <cstdio>

namespace dc::platform {

bool Window::init(const char* title) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    sdl_window = SDL_CreateWindow(title, width, height, SDL_WINDOW_OPENGL);
    if (!sdl_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        shutdown();
        return false;
    }
    gl_context = SDL_GL_CreateContext(sdl_window);
    if (!gl_context) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        shutdown();
        return false;
    }
    SDL_GL_MakeCurrent(sdl_window, static_cast<SDL_GLContext>(gl_context));

    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        shutdown();
        return false;
    }
    std::printf("GL %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    SDL_SetWindowRelativeMouseMode(sdl_window, true);

    int w, h; framebuffer_size(w, h);
    glViewport(0, 0, w, h);
    return true;
}

bool Window::pump_events(dc::input::Input& input) {
    input.begin_frame();
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        input.on_event(e);
    }
    return !input.quit;
}

void Window::framebuffer_size(int& w, int& h) const {
    SDL_GetWindowSizeInPixels(sdl_window, &w, &h);
}

void Window::swap() { SDL_GL_SwapWindow(sdl_window); }

void Window::shutdown() {
    if (gl_context) SDL_GL_DestroyContext(static_cast<SDL_GLContext>(gl_context));
    if (sdl_window) SDL_DestroyWindow(sdl_window);
    SDL_Quit();
    gl_context = nullptr;
    sdl_window = nullptr;
}

} // namespace dc::platform
```

- [ ] **Step 2b: Note** — this breaks `main.cpp` (old `pump_events()` call and triangle renderer). That's expected; `make build` will fail until Task 9 rewrites main. Verify just this TU compiles:

Run:
```bash
clang++ -std=c++17 -Isrc -Ithird_party/install/include -fsyntax-only src/engine/platform/window.cpp && echo OK
```
Expected: `OK`.

- [ ] **Step 3: Commit**

```bash
git add src/engine/platform/window.h src/engine/platform/window.cpp
git commit -m "feat(platform): depth buffer, relative mouse, Input-fed pump_events"
```

---

### Task 7: Mesh GL resource (`renderer/mesh`)

**Files:**
- Create: `src/engine/renderer/mesh.h`
- Create: `src/engine/renderer/mesh.cpp`

(GL resource — verified by the smoke run in Task 9, not unit tested.)

- [ ] **Step 1: Write `src/engine/renderer/mesh.h`**

```cpp
#pragma once
#include <cstdint>
#include <vector>

namespace dc::renderer {

// Owns a VAO + VBO for an interleaved vertex array:
// 9 floats/vertex = pos(3) + normal(3) + color(3).
struct Mesh {
    uint32_t vao = 0;
    uint32_t vbo = 0;
    int vertex_count = 0;

    void upload(const std::vector<float>& interleaved);
    void draw() const;
    void destroy();
};

} // namespace dc::renderer
```

- [ ] **Step 2: Write `src/engine/renderer/mesh.cpp`**

```cpp
#include "engine/renderer/mesh.h"
#include <glad/gl.h>

namespace dc::renderer {

void Mesh::upload(const std::vector<float>& interleaved) {
    constexpr int FLOATS_PER_VERT = 9;
    vertex_count = static_cast<int>(interleaved.size() / FLOATS_PER_VERT);

    if (!vao) glGenVertexArrays(1, &vao);
    if (!vbo) glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(interleaved.size() * sizeof(float)),
                 interleaved.data(), GL_STATIC_DRAW);

    const GLsizei stride = FLOATS_PER_VERT * sizeof(float);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
}

void Mesh::draw() const {
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, vertex_count);
    glBindVertexArray(0);
}

void Mesh::destroy() {
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    vao = vbo = 0;
    vertex_count = 0;
}

} // namespace dc::renderer
```

- [ ] **Step 3: Syntax-check**

Run:
```bash
clang++ -std=c++17 -Isrc -Ithird_party -Ithird_party/glad/include -fsyntax-only src/engine/renderer/mesh.cpp && echo OK
```
Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
git add src/engine/renderer/mesh.h src/engine/renderer/mesh.cpp
git commit -m "feat(renderer): Mesh GL resource (VAO/VBO upload + draw)"
```

---

### Task 8: World shaders + renderer rewrite

**Files:**
- Create: `assets/shaders/world.vert`
- Create: `assets/shaders/world.frag`
- Delete: `assets/shaders/tri.vert`, `assets/shaders/tri.frag`
- Modify: `src/engine/renderer/renderer.h`
- Modify: `src/engine/renderer/renderer.cpp` (full rewrite)

- [ ] **Step 1: Write `assets/shaders/world.vert`**

```glsl
#version 330 core
layout (location = 0) in vec3 a_pos;
layout (location = 1) in vec3 a_normal;
layout (location = 2) in vec3 a_color;
uniform mat4 u_viewproj;
out vec3 v_normal;
out vec3 v_color;
void main() {
    v_normal = a_normal;
    v_color = a_color;
    gl_Position = u_viewproj * vec4(a_pos, 1.0);
}
```

- [ ] **Step 2: Write `assets/shaders/world.frag`**

```glsl
#version 330 core
in vec3 v_normal;
in vec3 v_color;
out vec4 frag_color;
void main() {
    vec3 light_dir = normalize(vec3(0.4, 1.0, 0.3));
    float diffuse = max(dot(normalize(v_normal), light_dir), 0.0);
    float ambient = 0.35;
    float shade = ambient + (1.0 - ambient) * diffuse;
    frag_color = vec4(v_color * shade, 1.0);
}
```

- [ ] **Step 3: Delete the triangle shaders**

Run: `git rm assets/shaders/tri.vert assets/shaders/tri.frag`

- [ ] **Step 4: Overwrite `src/engine/renderer/renderer.h`**

```cpp
#pragma once
#include <cstdint>
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"

namespace dc::renderer {

struct Renderer {
    uint32_t program = 0;

    // Loads world.{vert,frag} and enables depth testing. Returns false on failure.
    bool init();
    // Clears, sets the view-projection uniform from the camera, and draws the mesh.
    void render(const Mesh& mesh, const Camera& camera, int fb_w, int fb_h);
    void shutdown();
};

} // namespace dc::renderer
```

- [ ] **Step 5: Overwrite `src/engine/renderer/renderer.cpp`**

```cpp
#include "engine/renderer/renderer.h"
#include "engine/renderer/shader.h"

#include <glad/gl.h>

namespace dc::renderer {

bool Renderer::init() {
    program = load_program("assets/shaders/world.vert", "assets/shaders/world.frag");
    if (!program) return false;
    glEnable(GL_DEPTH_TEST);
    return true;
}

void Renderer::render(const Mesh& mesh, const Camera& camera, int fb_w, int fb_h) {
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;
    mat4 view, proj, viewproj;
    camera.view_matrix(view);
    camera.proj_matrix(proj, aspect);
    glm_mat4_mul(proj, view, viewproj);

    glUseProgram(program);
    int loc = glGetUniformLocation(program, "u_viewproj");
    glUniformMatrix4fv(loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));

    mesh.draw();
}

void Renderer::shutdown() {
    if (program) glDeleteProgram(program);
    program = 0;
}

} // namespace dc::renderer
```

- [ ] **Step 6: Syntax-check the renderer TU**

Run:
```bash
clang++ -std=c++17 -Isrc -Ithird_party -Ithird_party/glad/include -Ithird_party/install/include -fsyntax-only src/engine/renderer/renderer.cpp && echo OK
```
Expected: `OK`. (Still won't link into the app until main is rewritten in Task 9.)

- [ ] **Step 7: Commit**

```bash
git add assets/shaders/world.vert assets/shaders/world.frag src/engine/renderer/renderer.h src/engine/renderer/renderer.cpp
git commit -m "feat(renderer): world shader + view-projection mesh rendering; remove triangle"
```

---

### Task 9: Map file + main loop integration (milestone)

**Files:**
- Create: `assets/maps/test.txt`
- Modify: `src/main.cpp` (full rewrite)

- [ ] **Step 1: Create `assets/maps/test.txt`**

```
##########
#........#
#..#..#..#
#........#
#...@....#
#..#..#..#
#........#
##########
```

- [ ] **Step 2: Rewrite `src/main.cpp`**

```cpp
#include "engine/platform/window.h"
#include "engine/input/input.h"
#include "engine/renderer/renderer.h"
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"
#include "engine/world/map.h"
#include "engine/world/map_mesh.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

static std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

int main(int argc, char** argv) {
    bool smoke = false;
    const char* map_path = "assets/maps/test.txt";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--smoke") == 0) smoke = true;
        else if (argv[i][0] != '-') map_path = argv[i];
    }

    std::string text = read_file(map_path);
    if (text.empty()) {
        std::fprintf(stderr, "could not read map: %s\n", map_path);
        return 1;
    }
    auto map = dc::world::parse_map(text);
    if (!map) {
        std::fprintf(stderr, "could not parse map: %s\n", map_path);
        return 1;
    }

    dc::platform::Window window;
    if (!window.init("dungeoncrawl")) return 1;

    dc::renderer::Renderer renderer;
    if (!renderer.init()) { window.shutdown(); return 1; }

    dc::renderer::Mesh mesh;
    mesh.upload(dc::world::build_map_mesh(*map));

    dc::renderer::Camera camera;
    camera.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
    camera.position[1] = dc::world::EYE_HEIGHT;
    camera.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;

    dc::input::Input input;
    bool running = true;
    uint64_t prev = SDL_GetTicksNS();
    while (running) {
        running = window.pump_events(input);

        uint64_t now = SDL_GetTicksNS();
        float dt = static_cast<float>(now - prev) / 1.0e9f;
        prev = now;

        camera.look(input.mouse_dx, input.mouse_dy);
        float forward = (input.key_down(SDL_SCANCODE_W) ? 1.0f : 0.0f)
                      - (input.key_down(SDL_SCANCODE_S) ? 1.0f : 0.0f);
        float strafe  = (input.key_down(SDL_SCANCODE_D) ? 1.0f : 0.0f)
                      - (input.key_down(SDL_SCANCODE_A) ? 1.0f : 0.0f);
        camera.move(forward, strafe, dt);

        int w, h; window.framebuffer_size(w, h);
        renderer.render(mesh, camera, w, h);
        window.swap();

        if (smoke) { std::printf("smoke: one frame rendered, exiting\n"); break; }
    }

    mesh.destroy();
    renderer.shutdown();
    window.shutdown();
    return 0;
}
```

- [ ] **Step 3: Build**

Run: `make build`
Expected: `built: .../build/dungeoncrawl`.

- [ ] **Step 4: Headless smoke run**

Run: `./build/dungeoncrawl --smoke`
Expected: a `GL ...` line, then `smoke: one frame rendered, exiting`, exit 0. Confirm `echo $?` → `0`.

- [ ] **Step 5: Full test + build gate**

Run: `make test && make build && ./build/dungeoncrawl --smoke`
Expected: all unit tests PASS, build succeeds, smoke exits 0.

- [ ] **Step 6: Interactive visual confirmation (human)**

Run: `make run`
Expected: a window shows an enclosed room with distinct floor/ceiling shading and
interior pillars; W/A/S/D move, mouse looks around, Esc quits. (Do not run from a
non-interactive context — it blocks until the window closes.)

- [ ] **Step 7: Commit**

```bash
git add assets/maps/test.txt src/main.cpp
git commit -m "feat: render ASCII map in 3D with FPS camera (milestone)"
```

---

## Self-Review

**Spec coverage:**
- ASCII map format + parse rules → Task 2 ✓
- Floor/ceiling per open cell, culled wall faces, baked colors → Task 3 ✓
- FPS camera (yaw/pitch, walk XZ, pitch clamp, view/proj) → Task 4 ✓
- Input (mouse delta + held keys, quit) → Task 5 ✓
- Platform: depth buffer, relative mouse, framebuffer_size, pump_events(Input&) → Task 6 ✓
- Mesh GL resource (9-float interleaved, attribs 0/1/2) → Task 7 ✓
- world shaders + renderer rewrite with u_viewproj + depth + directional light; triangle removed → Task 8 ✓
- test.txt + main loop wiring + spawn placement + --smoke contract → Task 9 ✓
- `make test` harness → Task 1 ✓
- Unit tests for map/map_mesh/camera → Tasks 2/3/4 ✓

**Placeholder scan:** No TBD/placeholder steps; all code shown in full. The `// TODO`-free.

**Type consistency:**
- Vertex layout = 9 floats (pos,normal,color) consistent across `build_map_mesh` (Task 3), `Mesh::upload` attribs (Task 7), and `world.vert` locations 0/1/2 (Task 8).
- `parse_map`→`std::optional<Map>`, `Map::at`, `spawn_col/row`, `TILE/WALL_HEIGHT/EYE_HEIGHT` used identically in Tasks 2/3/9.
- `Camera` members/methods (`position`, `yaw`, `pitch`, `front`, `look`, `move`, `view_matrix`, `proj_matrix`) consistent across Tasks 4/8/9.
- `Window::pump_events(Input&)` and `framebuffer_size(int&,int&)` consistent Tasks 6/9.
- `Input` (`begin_frame`, `on_event`, `key_down`, `mouse_dx/dy`, `quit`) consistent Tasks 5/6/9.
- `Renderer::render(const Mesh&, const Camera&, int, int)` consistent Tasks 8/9.

**Cross-cutting note for implementers:** Tasks 6 and 8 intentionally break `make build`
(old `pump_events()` call, removed triangle) until Task 9 rewrites `main.cpp`. Those
tasks therefore verify via `-fsyntax-only` on the changed TU, not a full build. The
full `make build` gate returns in Task 9. Do these tasks in order.

**cglm note:** cglm matrix/vector functions take the destination as the last
argument and operate on `float[N]` types (`vec3`, `mat4`). `glm_look` takes a
non-const eye pointer, hence the `const_cast` in `Camera::view_matrix`.
