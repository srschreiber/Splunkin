# ASCII Map → 3D World + FPS Camera Design

**Date:** 2026-05-31
**Status:** Approved

## Goal

Load an ASCII tile-grid map from a text file, generate 3D geometry (floor,
ceiling, and floor-to-ceiling solid columns that act as walls and pillars), and
let the player fly around it with an FPS-style WASD + mouse-look camera. This
replaces the bring-up "hello triangle" with the first real rendered world.

## Decisions

| Decision | Choice |
|---|---|
| Camera | FPS: WASD moves in the XZ plane, mouse looks (yaw/pitch), eye locked to floor height |
| Collision | None this milestone (noclip — camera passes through solids) |
| Map format | ASCII tile grid: `#` = solid column, `.`/space = open floor, `@` = spawn |
| Geometry | Single combined static mesh, one VBO, one draw call per frame |
| Lighting | One fixed directional light + flat per-face vertex colors (no textures) |
| Math | cglm (header-only) for view/projection matrices |
| Tests | Minimal custom harness (`<cassert>` + `scripts/test.sh` + `make test`), no external framework |

## Out of Scope (future milestones)

- Collision / physics
- Textures, PBR, multiple/dynamic lights, shadows
- Multiple map files / level switching UI
- Entities, doors, stairs, height variation

## Approach

**Single static mesh (chosen).** Parse the map once → generate one interleaved
vertex buffer (floor + ceiling + culled wall faces) → upload to a single
VAO/VBO → draw in one call per frame with a view-projection matrix.

Rejected: GL instancing (premature for a static dungeon, adds machinery) and
per-cell immediate draws (too many draw calls).

## Module Structure

```
src/engine/
├── world/
│   ├── map.{h,cpp}        # Cell enum, Map struct, parse_map(text)   [pure logic, unit-tested]
│   └── map_mesh.{h,cpp}   # build_map_mesh(Map) -> std::vector<float> [pure logic, unit-tested]
├── renderer/
│   ├── camera.{h,cpp}     # pos/yaw/pitch, view+proj, movement        [pure math, unit-tested]
│   ├── mesh.{h,cpp}       # GL resource: upload/draw/destroy          [GL, smoke-tested]
│   ├── shader.{h,cpp}     # reused as-is
│   └── renderer.{h,cpp}   # owns world shader + depth state; render(mesh, camera)
├── input/
│   └── input.{h,cpp}      # keyboard-held state + per-frame mouse delta [replaces stub]
└── platform/window.{h,cpp}# + depth buffer attr, relative mouse, feeds Input
assets/
├── shaders/world.{vert,frag}
└── maps/test.txt
```

The triangle path is removed: delete `assets/shaders/tri.{vert,frag}` and the
triangle geometry/draw in `renderer.cpp`. It was the bring-up milestone only.

## Components

### Map (`world/map.h`, `world/map.cpp`)
```cpp
namespace dc::world {

inline constexpr float TILE       = 2.0f;  // world units per tile
inline constexpr float WALL_HEIGHT= 3.0f;  // floor (y=0) to ceiling
inline constexpr float EYE_HEIGHT = 1.6f;  // camera height above floor

enum class Cell : uint8_t { Open, Solid };

struct Map {
    int width = 0;
    int height = 0;
    std::vector<Cell> cells;       // row-major, size width*height
    int spawn_col = 0;             // tile coords of spawn
    int spawn_row = 0;
    Cell at(int col, int row) const; // Solid for out-of-bounds (so perimeter is closed)
};

std::optional<Map> parse_map(const std::string& text);
}
```
Parsing rules:
- Split on `\n`; trailing `\r` stripped; trailing blank lines ignored.
- `width` = longest line; shorter lines padded with `Open`.
- `#` → `Solid`; `.` or space → `Open`; `@` → `Open` and records spawn (first `@` wins).
- Empty input (no non-blank lines) → `std::nullopt`.
- No `@` → spawn defaults to the first `Open` cell (row-major scan); if none, (0,0).
- `at()` returns `Solid` for out-of-bounds coordinates.

### Map mesh (`world/map_mesh.h`, `world/map_mesh.cpp`)
```cpp
namespace dc::world {
// Interleaved layout per vertex: pos.x,y,z, normal.x,y,z, color.r,g,b  (9 floats)
std::vector<float> build_map_mesh(const Map& map);
}
```
Generation (all triangles, 6 verts per quad, CCW front faces):
- For each `Open` cell: a floor quad at `y=0` (normal +Y, floor color) and a
  ceiling quad at `y=WALL_HEIGHT` (normal −Y, ceiling color).
- For each `Solid` cell, for each of the 4 horizontal directions: if the
  neighbor (via `Map::at`, so out-of-bounds counts as Solid) is `Open`, emit a
  vertical wall quad spanning `y=0..WALL_HEIGHT` on that face, normal pointing
  toward the open cell, wall color.
- Colors (flat, baked per vertex): floor `~(0.30,0.27,0.24)`, ceiling
  `~(0.18,0.18,0.22)`, walls `~(0.55,0.50,0.45)`. Exact values are not
  load-bearing; the directional light makes faces distinguishable.

### Camera (`renderer/camera.h`, `renderer/camera.cpp`)
```cpp
namespace dc::renderer {
struct Camera {
    vec3  position{0,0,0};   // cglm vec3
    float yaw   = 0.0f;      // radians
    float pitch = 0.0f;      // radians, clamped to +-89deg
    float fov_y = glm_rad(70.0f);
    float move_speed  = 4.0f;   // units/sec
    float mouse_sens  = 0.0025f; // radians per pixel

    void look(float dx, float dy);                 // applies mouse delta, clamps pitch
    void move(float forward, float strafe, float dt); // XZ-plane walk, Y fixed
    void view_matrix(mat4 out) const;
    void proj_matrix(mat4 out, float aspect) const;
    void front(vec3 out) const;                    // full look direction
};
}
```
- `front`: `x=cos(pitch)cos(yaw)`, `y=sin(pitch)`, `z=cos(pitch)sin(yaw)`.
- `move`: walk dir = normalize(front with y=0); strafe dir = normalize(cross(walk,up));
  `position += (walk*forward + strafe*strafe_amt) * move_speed * dt`; Y untouched.
- `look`: `yaw += dx*sens`, `pitch -= dy*sens`, clamp pitch to ±`glm_rad(89)`.
- `view_matrix`: `glm_look(position, front, up)`. `proj_matrix`: `glm_perspective(fov_y, aspect, 0.05, 100.0)`.

### Mesh (`renderer/mesh.h`, `renderer/mesh.cpp`)
GL resource owning a VAO + VBO and vertex count.
```cpp
namespace dc::renderer {
struct Mesh {
    uint32_t vao = 0, vbo = 0;
    int vertex_count = 0;
    void upload(const std::vector<float>& interleaved); // 9 floats/vertex: pos,normal,color
    void draw() const;        // glDrawArrays(GL_TRIANGLES, 0, vertex_count)
    void destroy();
};
}
```
Attribute layout: loc 0 = pos (3), loc 1 = normal (3), loc 2 = color (3), stride 9 floats.

### Renderer (`renderer/renderer.h`, `renderer/renderer.cpp`)
Owns the world shader program and depth state.
```cpp
namespace dc::renderer {
struct Renderer {
    uint32_t program = 0;
    bool init();                                   // loads world.{vert,frag}, glEnable(GL_DEPTH_TEST)
    void render(const Mesh& mesh, const Camera& camera, int fb_w, int fb_h);
    void shutdown();
};
}
```
`render`: clear color+depth, `glUseProgram`, compute `viewproj = proj*view`
(aspect = fb_w/fb_h), set `u_viewproj`, `mesh.draw()`.

### Shaders (`assets/shaders/world.vert`, `world.frag`)
- `world.vert`: in pos/normal/color; `gl_Position = u_viewproj * vec4(pos,1)`;
  pass normal + color to fragment.
- `world.frag`: `diffuse = max(dot(normalize(n), L), 0)` for a fixed light
  direction `L`; `frag = color * (ambient + (1-ambient)*diffuse)`, ambient ≈ 0.35.

### Input (`input/input.h`, `input/input.cpp`)
```cpp
namespace dc::input {
struct Input {
    float mouse_dx = 0, mouse_dy = 0;   // accumulated this frame
    bool  quit = false;
    void begin_frame();                 // zeroes mouse deltas
    void on_event(const SDL_Event& e);  // accumulates motion, sets quit on QUIT/Esc
    bool key_down(int scancode) const;  // wraps SDL_GetKeyboardState
};
}
```

### Platform (`platform/window.{h,cpp}` changes)
- `init`: add `SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24)` before window creation;
  after context current, `SDL_SetWindowRelativeMouseMode(sdl_window, true)`.
- Add `void framebuffer_size(int& w, int& h) const;` (via `SDL_GetWindowSizeInPixels`)
  so the renderer gets the real pixel size (Retina-correct).
- Replace `pump_events()` with `bool pump_events(dc::input::Input& input)`:
  `input.begin_frame()`, loop `SDL_PollEvent` → `input.on_event(e)`, return `!input.quit`.

## Data Flow

`assets/maps/test.txt` → `parse_map` → `Map` → `build_map_mesh` → `vector<float>`
→ `Mesh::upload`. Per frame: `Window::pump_events(input)` → read held keys for
`forward/strafe`, mouse deltas for look → `Camera::move/look` → `Renderer::render(mesh, camera, w, h)` → `Window::swap`.

## main.cpp Loop

1. `Window::init`, `Renderer::init`.
2. Read map path from `argv` (first non-flag arg) or default `assets/maps/test.txt`;
   read file; `parse_map`; on failure print error and exit 1.
3. `build_map_mesh` → `Mesh::upload`.
4. Place camera at spawn: `x=(spawn_col+0.5)*TILE`, `y=EYE_HEIGHT`, `z=(spawn_row+0.5)*TILE`.
5. Loop: `pump_events(input)`; build `forward`(W/S) and `strafe`(A/D) from
   `input.key_down(SDL_SCANCODE_*)`; `camera.look(input.mouse_dx, input.mouse_dy)`;
   `camera.move(forward, strafe, dt)`; `framebuffer_size`; `renderer.render(...)`; `swap()`.
6. `--smoke`: after one rendered frame, print a line and exit 0 (unchanged contract).

## Testing

Pure-logic modules get unit tests (no GL context needed):

- `tests/map_test.cpp`: parse a known multi-line grid → assert width/height,
  spawn position, specific cell types; ragged rows padded; `@` handling;
  empty input → `nullopt`; out-of-bounds `at()` is Solid.
- `tests/map_mesh_test.cpp`: 3×3 grid with a single center `#` surrounded by
  open → expect 8 floor + 8 ceiling + 4 wall quads → assert
  `vertices.size() == (8+8+4)*6*9`. A bordered grid case asserts perimeter
  faces are culled to inward-only. Spot-check a known vertex position/normal.
- `tests/camera_test.cpp`: `look` clamps pitch to ±89°; yaw wraps/accumulates;
  `move(forward=1, …)` advances along horizontal front and leaves Y fixed;
  strafe is perpendicular.

Harness: `scripts/test.sh` compiles each `tests/*_test.cpp` together with the
module(s) under test (`map.cpp`, `map_mesh.cpp`, `camera.cpp` — all GL-free,
cglm is header-only) using clang++ `-std=c++17 -Isrc -Ithird_party/install/include`,
runs each binary, and fails on the first non-zero exit. Each test file has a
`main()` using `<cassert>` and prints `PASS <name>`. Wired to `make test`.

GL units (`mesh`, `renderer`, `window`) and the integration are verified by the
existing `--smoke` path, which now renders one frame of the actual map and exits 0.

## test.txt (initial map)

A small enclosed room with a few interior pillars and a spawn, e.g.:

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

## Verification

- `make test` → all unit tests print PASS, exit 0.
- `make build` → links.
- `./build/dungeoncrawl --smoke` → prints GL version + smoke line, exit 0.
- `make run` → a window shows a flat-shaded room (distinct floor/ceiling/wall
  shading) with interior pillars; WASD moves, mouse looks; Esc quits.
