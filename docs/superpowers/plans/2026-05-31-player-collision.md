# Player Entity + Camera Attachment + Collision Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a `Player` that owns the transform + physics (gravity/jump) and collides with the map (sliding along walls); make the camera a lens that renders from the player's eye.

**Architecture:** New GL-free `world/collision` and `entity/player` modules are unit-tested. `Camera` is slimmed to a lens that derives view/proj from a `Player`. `Renderer::render` takes `(mesh, camera, player, w, h)`. main wires player movement + jump into the loop.

**Tech Stack:** C++17, clang++, SDL3, GLAD, cglm (header-only). Existing `make test` harness extended.

---

## Methodology note

`collision` and `player` are GL-free and get genuine TDD. `camera` is GL-free too
(unit-tested). `renderer` (GL) is syntax-checked; full integration is verified by
`--smoke` in the final task. Tasks 3–4 intentionally break `make build` (camera +
renderer signatures change) until Task 5 rewrites `main.cpp`; those tasks verify
via `-fsyntax-only` and unit tests. Do the tasks in order.

## File Structure

- `src/engine/world/collision.{h,cpp}` + `tests/collision_test.cpp` — circle-vs-grid test (Task 1).
- `src/engine/entity/player.{h,cpp}` + `tests/player_test.cpp` — transform + physics + slide (Task 2).
- `src/engine/renderer/camera.{h,cpp}` + `tests/camera_test.cpp` rewrite — lens from Player (Task 3).
- `src/engine/renderer/renderer.{h,cpp}` — `render(mesh, camera, player, w, h)` (Task 4).
- `src/main.cpp` rewrite + delete `src/engine/entity/entity.h` stub (Task 5).

---

### Task 1: Collision helper (`world/collision`)

**Files:**
- Create: `src/engine/world/collision.h`
- Create: `src/engine/world/collision.cpp`
- Create: `tests/collision_test.cpp`
- Modify: `scripts/test.sh`

- [ ] **Step 1: Write `src/engine/world/collision.h`**

```cpp
#pragma once
#include "engine/world/map.h"

namespace dc::world {

// True if a circle centered at world (x,z) with radius r overlaps any Solid
// cell. Out-of-bounds counts as Solid (via Map::at).
bool circle_hits_solid(const Map& map, float x, float z, float r);

} // namespace dc::world
```

- [ ] **Step 2: Write `tests/collision_test.cpp` (failing test)**

```cpp
#include "engine/world/collision.h"
#include "engine/world/map.h"
#include <cassert>
#include <cstdio>

using namespace dc::world;

int main() {
    // 3x3, center cell Solid. TILE=2 -> center solid AABB is x[2,4], z[2,4].
    auto m = parse_map(
        "...\n"
        ".#.\n"
        "...\n");
    assert(m.has_value());

    // Circle centered inside the solid cell -> hit.
    assert(circle_hits_solid(*m, 3.0f, 3.0f, 0.4f) == true);

    // Circle in the open cell above (center world (3,1)); nearest solid point is
    // (3,2), distance 1.0 > 0.4 -> no hit.
    assert(circle_hits_solid(*m, 3.0f, 1.0f, 0.4f) == false);

    // Move it close to the boundary: center (3, 1.7), nearest (3,2) dist 0.3 < 0.4 -> hit.
    assert(circle_hits_solid(*m, 3.0f, 1.7f, 0.4f) == true);

    // Far outside the grid -> out-of-bounds cells are Solid -> hit.
    assert(circle_hits_solid(*m, -5.0f, -5.0f, 0.4f) == true);

    std::printf("PASS collision\n");
    return 0;
}
```

- [ ] **Step 3: Add to `scripts/test.sh`**, inserting BEFORE the `echo "all tests passed"` line:
```bash
build_and_run "$ROOT/tests/collision_test.cpp" \
  "$ROOT/src/engine/world/collision.cpp" "$ROOT/src/engine/world/map.cpp"
```

- [ ] **Step 4: Run — verify it FAILS**

Run: `make test`
Expected: FAIL — `collision.cpp` missing / undefined `circle_hits_solid`.

- [ ] **Step 5: Write `src/engine/world/collision.cpp`**

```cpp
#include "engine/world/collision.h"
#include <cmath>

namespace dc::world {

bool circle_hits_solid(const Map& map, float x, float z, float r) {
    const int c0 = static_cast<int>(std::floor((x - r) / TILE));
    const int c1 = static_cast<int>(std::floor((x + r) / TILE));
    const int r0 = static_cast<int>(std::floor((z - r) / TILE));
    const int r1 = static_cast<int>(std::floor((z + r) / TILE));

    for (int row = r0; row <= r1; ++row) {
        for (int col = c0; col <= c1; ++col) {
            if (map.at(col, row) != Cell::Solid) continue;
            const float minx = col * TILE, maxx = (col + 1) * TILE;
            const float minz = row * TILE, maxz = (row + 1) * TILE;
            const float cx = x < minx ? minx : (x > maxx ? maxx : x);
            const float cz = z < minz ? minz : (z > maxz ? maxz : z);
            const float dx = x - cx, dz = z - cz;
            if (dx * dx + dz * dz < r * r) return true;
        }
    }
    return false;
}

} // namespace dc::world
```

- [ ] **Step 6: Run — verify PASS**

Run: `make test`
Expected: `PASS collision` plus prior PASS lines, `all tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/engine/world/collision.h src/engine/world/collision.cpp tests/collision_test.cpp scripts/test.sh
git commit -m "feat(world): circle-vs-grid collision helper with unit tests"
```

---

### Task 2: Player entity (`entity/player`)

**Files:**
- Create: `src/engine/entity/player.h`
- Create: `src/engine/entity/player.cpp`
- Create: `tests/player_test.cpp`
- Modify: `scripts/test.sh`

- [ ] **Step 1: Write `src/engine/entity/player.h`**

```cpp
#pragma once
#include <cglm/cglm.h>
#include "engine/world/map.h"

namespace dc::entity {

inline constexpr float PLAYER_RADIUS = 0.4f;   // world units
inline constexpr float GRAVITY       = 20.0f;  // units/s^2
inline constexpr float JUMP_SPEED    = 6.0f;   // units/s (initial jump velocity)
inline constexpr float MOVE_SPEED    = 4.0f;   // units/s

struct Player {
    vec3  position = {0.0f, 0.0f, 0.0f};   // EYE position (authoritative)
    float yaw   = 0.0f;                    // radians
    float pitch = 0.0f;                    // radians, clamped +-89 deg
    float vel_y = 0.0f;                    // vertical velocity
    bool  on_ground = true;

    // Look direction unit vector from yaw/pitch.
    void front(vec3 out) const;
    // Mouse delta (pixels): yaw += dx*sens, pitch -= dy*sens, clamp pitch.
    void add_look(float dx, float dy);
    // forward/strafe in {-1,0,1}; jump=true attempts a jump this frame.
    // Horizontal motion slides against the map; vertical applies gravity/jump.
    void update(float forward, float strafe, bool jump, float dt,
                const dc::world::Map& map);
};

} // namespace dc::entity
```

- [ ] **Step 2: Write `tests/player_test.cpp` (failing test)**

```cpp
#include "engine/entity/player.h"
#include "engine/world/map.h"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace dc::entity;
using dc::world::EYE_HEIGHT;

static bool approx(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

// A 10x7 room: solid border, open interior cols 1..8 rows 1..5.
static const char* ROOM =
    "##########\n"
    "#........#\n"
    "#........#\n"
    "#........#\n"
    "#........#\n"
    "#........#\n"
    "##########\n";

int main() {
    auto map = dc::world::parse_map(ROOM);
    assert(map.has_value());

    // add_look clamps pitch to +-89 deg.
    {
        Player p;
        p.add_look(0.0f, -1.0e6f);
        assert(p.pitch <= glm_rad(89.0f) + 1e-3f);
        p.add_look(0.0f, 2.0e6f);
        assert(p.pitch >= -glm_rad(89.0f) - 1e-3f);
    }

    // Blocked axis: standing in cell col1 (x in [2,4]) near the left wall (col0,
    // x<=2), moving -X is blocked; position.x unchanged.
    {
        Player p;
        p.position[0] = 2.5f; p.position[1] = EYE_HEIGHT; p.position[2] = 7.0f;
        p.yaw = static_cast<float>(M_PI);   // walk dir = -X
        p.update(1.0f, 0.0f, false, 0.1f, *map);   // 0.4 units toward wall
        assert(approx(p.position[0], 2.5f));        // blocked, did not move
    }

    // Free axis slides: from the same spot, moving +Z (no wall) advances Z and
    // leaves X unchanged.
    {
        Player p;
        p.position[0] = 2.5f; p.position[1] = EYE_HEIGHT; p.position[2] = 7.0f;
        p.yaw = static_cast<float>(M_PI / 2.0);   // walk dir = +Z
        p.update(1.0f, 0.0f, false, 0.1f, *map);
        assert(p.position[2] > 7.0f);             // moved along Z
        assert(approx(p.position[0], 2.5f));      // X unchanged
    }

    // Diagonal not faster: horizontal displacement magnitude == MOVE_SPEED*dt.
    {
        Player p;
        p.position[0] = 10.0f; p.position[1] = EYE_HEIGHT; p.position[2] = 7.0f;
        p.yaw = 0.0f;
        const float x0 = p.position[0], z0 = p.position[2];
        const float dt = 0.05f;
        p.update(1.0f, 1.0f, false, dt, *map);    // forward + strafe
        const float dx = p.position[0] - x0, dz = p.position[2] - z0;
        assert(approx(std::sqrt(dx * dx + dz * dz), MOVE_SPEED * dt));
    }

    // Gravity: dropped from above rest, eventually lands at EYE_HEIGHT, grounded.
    {
        Player p;
        p.position[0] = 10.0f; p.position[1] = EYE_HEIGHT + 2.0f; p.position[2] = 7.0f;
        p.on_ground = false;
        for (int i = 0; i < 200; ++i) p.update(0.0f, 0.0f, false, 0.05f, *map);
        assert(approx(p.position[1], EYE_HEIGHT));
        assert(p.on_ground == true);
    }

    // Jump + no double-jump.
    {
        Player p;
        p.position[0] = 10.0f; p.position[1] = EYE_HEIGHT; p.position[2] = 7.0f;
        p.on_ground = true;
        p.update(0.0f, 0.0f, true, 0.1f, *map);   // jump from ground
        assert(p.on_ground == false);
        assert(p.position[1] > EYE_HEIGHT);
        const float v1 = p.vel_y;                 // upward, reduced by one g*dt
        assert(v1 > 0.0f);
        p.update(0.0f, 0.0f, true, 0.1f, *map);   // jump pressed again mid-air
        assert(p.vel_y < v1);                     // no re-launch; gravity reduced it
    }

    std::printf("PASS player\n");
    return 0;
}
```

- [ ] **Step 3: Add to `scripts/test.sh`**, inserting BEFORE the `echo "all tests passed"` line:
```bash
build_and_run "$ROOT/tests/player_test.cpp" \
  "$ROOT/src/engine/entity/player.cpp" "$ROOT/src/engine/world/collision.cpp" \
  "$ROOT/src/engine/world/map.cpp"
```

- [ ] **Step 4: Run — verify it FAILS**

Run: `make test`
Expected: FAIL — `player.cpp` missing / undefined `Player` members.

- [ ] **Step 5: Write `src/engine/entity/player.cpp`**

```cpp
#include "engine/entity/player.h"
#include "engine/world/collision.h"
#include <cmath>

namespace dc::entity {

namespace { constexpr float MOUSE_SENS = 0.0025f; }

void Player::front(vec3 out) const {
    out[0] = std::cos(pitch) * std::cos(yaw);
    out[1] = std::sin(pitch);
    out[2] = std::cos(pitch) * std::sin(yaw);
    glm_vec3_normalize(out);
}

void Player::add_look(float dx, float dy) {
    yaw   += dx * MOUSE_SENS;
    pitch -= dy * MOUSE_SENS;
    const float limit = glm_rad(89.0f);
    if (pitch >  limit) pitch =  limit;
    if (pitch < -limit) pitch = -limit;
}

void Player::update(float forward, float strafe, bool jump, float dt,
                    const dc::world::Map& map) {
    // --- Horizontal intent ---
    vec3 up = {0.0f, 1.0f, 0.0f};
    vec3 walk = { std::cos(yaw), 0.0f, std::sin(yaw) };
    glm_vec3_normalize(walk);
    vec3 right;
    glm_vec3_cross(walk, up, right);
    glm_vec3_normalize(right);

    vec3 delta = {0.0f, 0.0f, 0.0f}, tmp;
    glm_vec3_scale(walk,  forward, tmp); glm_vec3_add(delta, tmp, delta);
    glm_vec3_scale(right, strafe,  tmp); glm_vec3_add(delta, tmp, delta);
    if (glm_vec3_norm(delta) > 1e-6f) {
        glm_vec3_normalize(delta);
        glm_vec3_scale(delta, MOVE_SPEED * dt, delta);
    } else {
        delta[0] = delta[1] = delta[2] = 0.0f;
    }

    // --- Per-axis slide against the map ---
    if (!dc::world::circle_hits_solid(map, position[0] + delta[0], position[2], PLAYER_RADIUS))
        position[0] += delta[0];
    if (!dc::world::circle_hits_solid(map, position[0], position[2] + delta[2], PLAYER_RADIUS))
        position[2] += delta[2];

    // --- Vertical: gravity + jump ---
    if (jump && on_ground) {
        vel_y = JUMP_SPEED;
        on_ground = false;
    }
    vel_y -= GRAVITY * dt;
    position[1] += vel_y * dt;

    const float ceil_limit = dc::world::WALL_HEIGHT - 0.2f;
    if (position[1] > ceil_limit) {
        position[1] = ceil_limit;
        if (vel_y > 0.0f) vel_y = 0.0f;
    }
    if (position[1] <= dc::world::EYE_HEIGHT) {
        position[1] = dc::world::EYE_HEIGHT;
        vel_y = 0.0f;
        on_ground = true;
    }
}

} // namespace dc::entity
```

- [ ] **Step 6: Run — verify PASS**

Run: `make test`
Expected: `PASS player` plus prior PASS lines, `all tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/engine/entity/player.h src/engine/entity/player.cpp tests/player_test.cpp scripts/test.sh
git commit -m "feat(entity): Player with slide collision, gravity, and jump (unit-tested)"
```

---

### Task 3: Slim Camera to a lens (rewrite camera + its test)

**Files:**
- Modify (full replace): `src/engine/renderer/camera.h`
- Modify (full replace): `src/engine/renderer/camera.cpp`
- Modify (full replace): `tests/camera_test.cpp`
- Modify: `scripts/test.sh` (update the camera_test build line)

NOTE: This task INTENTIONALLY breaks `make build` (renderer.cpp and main.cpp still
call the old Camera API). The gate here is `make test` (the rewritten camera_test
passes) plus `-fsyntax-only` on camera.cpp. Do NOT touch renderer.cpp/main.cpp.

- [ ] **Step 1: Replace `src/engine/renderer/camera.h`**

```cpp
#pragma once
#include <cglm/cglm.h>

namespace dc::entity { struct Player; }

namespace dc::renderer {

// A lens. Produces view/projection matrices from a Player's eye + orientation.
struct Camera {
    float fov_y  = 1.2217305f;   // 70 degrees in radians
    float near_z = 0.05f;
    float far_z  = 100.0f;

    void view_matrix(mat4 out, const dc::entity::Player& p) const;
    void proj_matrix(mat4 out, float aspect) const;
};

} // namespace dc::renderer
```

- [ ] **Step 2: Replace `src/engine/renderer/camera.cpp`**

```cpp
#include "engine/renderer/camera.h"
#include "engine/entity/player.h"

namespace dc::renderer {

void Camera::view_matrix(mat4 out, const dc::entity::Player& p) const {
    vec3 f;  p.front(f);
    vec3 up = {0.0f, 1.0f, 0.0f};
    vec3 eye = { p.position[0], p.position[1], p.position[2] };  // copy (glm_look wants non-const)
    glm_look(eye, f, up, out);
}

void Camera::proj_matrix(mat4 out, float aspect) const {
    glm_perspective(fov_y, aspect, near_z, far_z, out);
}

} // namespace dc::renderer
```

- [ ] **Step 3: Replace `tests/camera_test.cpp`**

```cpp
#include "engine/renderer/camera.h"
#include "engine/entity/player.h"
#include <cassert>
#include <cstdio>
#include <cmath>

using namespace dc::renderer;
using dc::entity::Player;

static bool approx(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

int main() {
    Camera cam;

    // Projection is a standard OpenGL RH perspective: out[3][3]==0, out[2][3]==-1.
    {
        mat4 proj;
        cam.proj_matrix(proj, 16.0f / 9.0f);
        assert(approx(proj[3][3], 0.0f));
        assert(approx(proj[2][3], -1.0f));
    }

    // View: player at origin looking +X (yaw=0). A world point ahead (+X) lands
    // in front of the camera => negative view-space Z.
    {
        Player p;            // position {0,0,0}, yaw 0, pitch 0
        mat4 view;
        cam.view_matrix(view, p);
        vec4 world = {5.0f, 0.0f, 0.0f, 1.0f}, vp;
        glm_mat4_mulv(view, world, vp);
        assert(vp[2] < 0.0f);
    }

    std::printf("PASS camera\n");
    return 0;
}
```

- [ ] **Step 4: Update the camera_test line in `scripts/test.sh`**

Find the existing line:
```bash
build_and_run "$ROOT/tests/camera_test.cpp" "$ROOT/src/engine/renderer/camera.cpp"
```
Replace it with (camera.cpp now depends on Player, which depends on collision + map):
```bash
build_and_run "$ROOT/tests/camera_test.cpp" \
  "$ROOT/src/engine/renderer/camera.cpp" "$ROOT/src/engine/entity/player.cpp" \
  "$ROOT/src/engine/world/collision.cpp" "$ROOT/src/engine/world/map.cpp"
```

- [ ] **Step 5: Run — verify camera test passes and camera TU syntax-checks**

Run: `make test`
Expected: `PASS camera` plus all prior PASS lines, `all tests passed`.

Run: `clang++ -std=c++17 -Isrc -Ithird_party/install/include -fsyntax-only src/engine/renderer/camera.cpp && echo OK`
Expected: `OK`.

- [ ] **Step 6: Commit**

```bash
git add src/engine/renderer/camera.h src/engine/renderer/camera.cpp tests/camera_test.cpp scripts/test.sh
git commit -m "refactor(renderer): slim Camera to a lens deriving view/proj from Player"
```

---

### Task 4: Renderer takes the Player

**Files:**
- Modify (full replace): `src/engine/renderer/renderer.h`
- Modify (full replace): `src/engine/renderer/renderer.cpp`

NOTE: Still intentionally breaks `make build` (main.cpp not yet updated). Gate is
`-fsyntax-only` on renderer.cpp. Do NOT touch main.cpp.

- [ ] **Step 1: Replace `src/engine/renderer/renderer.h`**

```cpp
#pragma once
#include <cstdint>
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"

namespace dc::entity { struct Player; }

namespace dc::renderer {

struct Renderer {
    uint32_t program = 0;
    int u_viewproj_loc = -1;

    // Loads world.{vert,frag} and enables depth testing. Returns false on failure.
    bool init();
    // Clears, sets the view-projection uniform from camera+player, draws the mesh.
    void render(const Mesh& mesh, const Camera& camera,
                const dc::entity::Player& player, int fb_w, int fb_h);
    void shutdown();
};

} // namespace dc::renderer
```

- [ ] **Step 2: Replace `src/engine/renderer/renderer.cpp`**

```cpp
#include "engine/renderer/renderer.h"
#include "engine/renderer/shader.h"
#include "engine/entity/player.h"

#include <glad/gl.h>

namespace dc::renderer {

bool Renderer::init() {
    program = load_program("assets/shaders/world.vert", "assets/shaders/world.frag");
    if (!program) return false;
    u_viewproj_loc = glGetUniformLocation(program, "u_viewproj");
    glEnable(GL_DEPTH_TEST);
    return true;
}

void Renderer::render(const Mesh& mesh, const Camera& camera,
                      const dc::entity::Player& player, int fb_w, int fb_h) {
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;
    mat4 view, proj, viewproj;
    camera.view_matrix(view, player);
    camera.proj_matrix(proj, aspect);
    glm_mat4_mul(proj, view, viewproj);

    glUseProgram(program);
    glUniformMatrix4fv(u_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));

    mesh.draw();
}

void Renderer::shutdown() {
    if (program) glDeleteProgram(program);
    program = 0;
    u_viewproj_loc = -1;
}

} // namespace dc::renderer
```

- [ ] **Step 3: Syntax-check the renderer TU**

Run:
```bash
clang++ -std=c++17 -Isrc -Ithird_party -Ithird_party/glad/include -Ithird_party/install/include -fsyntax-only src/engine/renderer/renderer.cpp && echo OK
```
Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
git add src/engine/renderer/renderer.h src/engine/renderer/renderer.cpp
git commit -m "feat(renderer): render from camera+player; cache u_viewproj location"
```

---

### Task 5: Wire Player into main (milestone)

**Files:**
- Modify (full replace): `src/main.cpp`
- Delete: `src/engine/entity/entity.h` (superseded stub)

- [ ] **Step 1: Confirm the build is currently broken (expected before-state)**

Run: `make build 2>&1 | tail -5 || true`
Expected: a compile error in `main.cpp` (old `camera.look`/`move`/`pump`/`render` calls). This confirms the pre-state.

- [ ] **Step 2: Delete the superseded entity stub**

Run: `git rm src/engine/entity/entity.h`

- [ ] **Step 3: Replace `src/main.cpp`**

```cpp
#include "engine/platform/window.h"
#include "engine/input/input.h"
#include "engine/renderer/renderer.h"
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"
#include "engine/entity/player.h"
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

    dc::entity::Player player;
    player.position[0] = (map->spawn_col + 0.5f) * dc::world::TILE;
    player.position[1] = dc::world::EYE_HEIGHT;
    player.position[2] = (map->spawn_row + 0.5f) * dc::world::TILE;

    dc::input::Input input;
    bool running = true;
    uint64_t prev = SDL_GetTicksNS();
    while (running) {
        running = window.pump_events(input);

        uint64_t now = SDL_GetTicksNS();
        float dt = static_cast<float>(now - prev) / 1.0e9f;
        prev = now;

        player.add_look(input.mouse_dx, input.mouse_dy);
        float forward = (input.key_down(SDL_SCANCODE_W) ? 1.0f : 0.0f)
                      - (input.key_down(SDL_SCANCODE_S) ? 1.0f : 0.0f);
        float strafe  = (input.key_down(SDL_SCANCODE_D) ? 1.0f : 0.0f)
                      - (input.key_down(SDL_SCANCODE_A) ? 1.0f : 0.0f);
        bool jump = input.key_down(SDL_SCANCODE_SPACE);
        player.update(forward, strafe, jump, dt, *map);

        int w, h; window.framebuffer_size(w, h);
        renderer.render(mesh, camera, player, w, h);
        window.swap();

        if (smoke) { std::printf("smoke: one frame rendered, exiting\n"); break; }
    }

    mesh.destroy();
    renderer.shutdown();
    window.shutdown();
    return 0;
}
```

- [ ] **Step 4: Build**

Run: `make build`
Expected: `built: .../build/dungeoncrawl`.

- [ ] **Step 5: Headless smoke run**

Run: `./build/dungeoncrawl --smoke`
Expected: a `GL ...` line, then `smoke: one frame rendered, exiting`, exit 0. Confirm `echo $?` → `0`. Run from repo root.

- [ ] **Step 6: Full gate**

Run: `make test && make build && ./build/dungeoncrawl --smoke`
Expected: all unit tests PASS (sanity, map, map_mesh, collision, player, camera), build succeeds, smoke exits 0.

- [ ] **Step 7: Interactive visual check (human, do not run from automation)**

`make run` → walk with WASD, look with mouse, jump with Space; you should slide
along walls instead of sticking, and bonk your head on the ceiling if you jump
into it. `make run` blocks until the window closes.

- [ ] **Step 8: Commit**

```bash
git add src/main.cpp
git commit -m "feat: attach camera to a Player with collision, gravity, and jump (milestone)"
```

---

## Self-Review

**Spec coverage:**
- `world/collision.circle_hits_solid` (circle-vs-grid, OOB=solid, closest-point) → Task 1 ✓
- `Player` transform + `front`/`add_look`/`update`, per-axis slide, gravity+jump, ceiling clamp, pitch clamp → Task 2 ✓
- Camera slimmed to lens (view from Player, proj), `move`/`look` removed → Task 3 ✓
- `Renderer::render(mesh, camera, player, w, h)` + cached uniform → Task 4 ✓
- main wires player movement + jump (Space), spawn at eye height; entity.h stub removed → Task 5 ✓
- Unit tests: collision, player (slide/diagonal/gravity/jump/pitch), camera (view/proj) → Tasks 1/2/3 ✓

**Placeholder scan:** No TBD/placeholder steps; all code shown in full.

**Type consistency:**
- `dc::world::circle_hits_solid(const Map&, float, float, float)` used identically in collision_test, player.cpp, player_test (indirectly) — Tasks 1/2.
- `dc::entity::Player` members (`position`, `yaw`, `pitch`, `vel_y`, `on_ground`) and methods (`front`, `add_look`, `update(forward,strafe,jump,dt,map)`) consistent across Tasks 2/3/4/5.
- Constants `PLAYER_RADIUS/GRAVITY/JUMP_SPEED/MOVE_SPEED` (entity) and `TILE/WALL_HEIGHT/EYE_HEIGHT` (world) used consistently.
- `Camera::view_matrix(mat4, const Player&)` / `proj_matrix(mat4, float)` consistent Tasks 3/4.
- `Renderer::render(const Mesh&, const Camera&, const Player&, int, int)` consistent Tasks 4/5.

**Ordering / breakage note:** Tasks 3–4 change `Camera`/`Renderer` signatures and
leave `make build` broken (main.cpp still on the old API) until Task 5. They are
verified by `make test` + `-fsyntax-only`. The full `make build` + `--smoke` gate
returns in Task 5. Execute in order.

**cglm note:** `glm_mat4_mulv(m, v, dest)` multiplies a mat4 by a vec4. cglm uses
RH perspective with [-1,1] depth (OpenGL default), so `proj[2][3] == -1` and
`proj[3][3] == 0`. `glm_look` takes a non-const eye (copied locally to avoid a cast).
