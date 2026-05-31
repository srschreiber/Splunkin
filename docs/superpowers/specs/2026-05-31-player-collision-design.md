# Player Entity + Camera Attachment + Collision Design

**Date:** 2026-05-31
**Status:** Approved

## Goal

Introduce a `Player` that owns the world transform and physics. The camera
becomes a lens that renders from the player's eye. The player walks the dungeon
with WASD + mouse-look, is blocked by solid cells (sliding along walls), and has
gravity + a jump. Maps stay hardcoded.

This is Sub-project A of three: **A) player + camera + collision**, then
B) textures, then C) networking.

## Decisions

| Decision | Choice |
|---|---|
| Order | A (this) → B textures → C networking |
| Vertical motion | Gravity + jump (Space) |
| Collision response | Slide along walls (per-axis resolution) |
| Player anchor | Eye position is authoritative; collision circle centered on it |
| Player shape | Circle, radius 0.4 (TILE = 2.0) |

## The Refactor

Currently `Camera` owns position/yaw/pitch and does `move()`/`look()`. Split:

- **`Player`** (new) — authoritative transform + physics: eye position, yaw,
  pitch, vertical velocity, ground flag. Owns movement intent, collision, and
  gravity/jump. GL-free, unit-tested.
- **`Camera`** (slimmed) — pure lens: builds view/projection matrices from a
  `Player`. Loses `move()`/`look()`. GL-free, unit-tested.
- **`Renderer::render`** — signature becomes `(mesh, camera, player, w, h)`.

The `src/engine/entity/entity.h` stub is replaced by the concrete `Player`.
`camera_test` is rewritten to cover view/proj only; movement/look/physics
coverage moves to a new `player_test`.

## Components

### Collision (`world/collision.{h,cpp}`)
```cpp
namespace dc::world {
// True if a circle at (x,z) with radius r overlaps any Solid cell.
// Out-of-bounds counts as Solid (Map::at already enforces this).
bool circle_hits_solid(const Map& map, float x, float z, float r);
}
```
Implementation: convert the circle's AABB `[x-r, x+r] x [z-r, z+r]` to the range
of tile indices it covers; for each Solid cell in range, clamp `(x,z)` to the
cell's world AABB `[col*TILE,(col+1)*TILE] x [row*TILE,(row+1)*TILE]` to get the
closest point, and return true if its squared distance to `(x,z)` is `< r*r`.

### Player (`entity/player.{h,cpp}`)
```cpp
#include <cglm/cglm.h>
#include "engine/world/map.h"

namespace dc::entity {

inline constexpr float PLAYER_RADIUS = 0.4f;   // world units
inline constexpr float GRAVITY       = 20.0f;  // units/s^2
inline constexpr float JUMP_SPEED    = 6.0f;   // units/s (initial jump velocity)
inline constexpr float MOVE_SPEED    = 4.0f;   // units/s

struct Player {
    vec3  position = {0,0,0};   // EYE position (authoritative)
    float yaw   = 0.0f;         // radians
    float pitch = 0.0f;         // radians, clamped +-89 deg
    float vel_y = 0.0f;         // vertical velocity
    bool  on_ground = true;

    void front(vec3 out) const;                   // look dir from yaw/pitch
    void add_look(float dx, float dy);            // mouse delta; yaw += dx*sens,
                                                  // pitch -= dy*sens, clamp pitch
    // forward/strafe in {-1,0,1}; jump = true to attempt a jump this frame.
    void update(float forward, float strafe, bool jump, float dt,
                const dc::world::Map& map);
};

} // namespace dc::entity
```
`mouse_sens` is an internal constant (0.0025 rad/px), matching the old camera.

**Horizontal movement (per-axis slide):**
1. `walk = normalize(cos(yaw), 0, sin(yaw))`; `right = normalize(cross(walk, up))`.
2. `delta = walk*forward + right*strafe`; if `|delta| > 0`, normalize it (so
   diagonal isn't faster), then scale by `MOVE_SPEED * dt`.
3. Resolve X: if `!circle_hits_solid(map, position.x + delta.x, position.z, PLAYER_RADIUS)`,
   apply `position.x += delta.x`.
4. Resolve Z: if `!circle_hits_solid(map, position.x, position.z + delta.z, PLAYER_RADIUS)`,
   apply `position.z += delta.z`.
   (Resolving axes independently produces wall-sliding.)

**Vertical movement (gravity + jump):**
- Rest height = `world::EYE_HEIGHT` (floor at y=0).
- If `jump && on_ground`: `vel_y = JUMP_SPEED; on_ground = false`.
- `vel_y -= GRAVITY * dt; position.y += vel_y * dt`.
- If `position.y <= EYE_HEIGHT`: snap `position.y = EYE_HEIGHT`, `vel_y = 0`,
  `on_ground = true`.
- Ceiling clamp: if `position.y > WALL_HEIGHT - 0.2`, set
  `position.y = WALL_HEIGHT - 0.2` and `vel_y = 0` (head bonk).

**Pitch clamp:** `add_look` clamps pitch to `±glm_rad(89)`.

### Camera (`renderer/camera.{h,cpp}` — slimmed)
```cpp
#include <cglm/cglm.h>
namespace dc::entity { struct Player; }

namespace dc::renderer {
struct Camera {
    float fov_y  = 1.2217305f;   // 70 deg
    float near_z = 0.05f;
    float far_z  = 100.0f;
    void view_matrix(mat4 out, const dc::entity::Player& p) const; // glm_look(eye, front, up)
    void proj_matrix(mat4 out, float aspect) const;               // glm_perspective
};
}
```
`view_matrix` reads `p.position` (eye) and `p.front(...)`; up = (0,1,0).

### Renderer (`renderer/renderer.{h,cpp}` change)
`void render(const Mesh& mesh, const Camera& camera, const dc::entity::Player& player, int fb_w, int fb_h);`
Computes `viewproj = proj * view(player)`, sets the cached `u_viewproj`, draws.

### Input + main
- `Input::key_down` already exists; main reads `SDL_SCANCODE_SPACE` for jump.
- Loop:
  ```
  running = window.pump_events(input);
  dt = ...;
  player.add_look(input.mouse_dx, input.mouse_dy);
  forward = W - S; strafe = D - A; jump = key_down(SPACE);
  player.update(forward, strafe, jump, dt, *map);
  window.framebuffer_size(w, h);
  renderer.render(mesh, camera, player, w, h);
  window.swap();
  ```
- Spawn: `player.position = ((spawn_col+0.5)*TILE, EYE_HEIGHT, (spawn_row+0.5)*TILE)`.

## Testing

New/changed GL-free unit tests:

- **`tests/collision_test.cpp`**: circle in open space → false; circle centered in
  / overlapping a solid cell → true; circle near a solid but farther than r →
  false; out-of-bounds → true.
- **`tests/player_test.cpp`**:
  - `add_look` clamps pitch to ±89°.
  - Walking straight into a wall: blocked axis doesn't move, other axis (parallel
    to wall) does → slide.
  - Diagonal movement magnitude == straight movement magnitude (normalized).
  - Gravity: starting above rest with `on_ground=false`, after enough time
    `position.y == EYE_HEIGHT` and `on_ground == true`.
  - Jump: `update(...,jump=true)` while grounded sets `vel_y > 0` and lifts off;
    a second `jump=true` mid-air does nothing (no double jump).
- **`tests/camera_test.cpp`** (rewritten): `proj_matrix` is a sane perspective
  (e.g. finite, `out[3][3] == 0`); `view_matrix` from a player at a known eye/yaw
  transforms a known world point to the expected camera-space sign (look down +X
  → a point ahead has negative camera-space Z). Keep it simple and deterministic.

GL/integration verified by `--smoke` (one frame, exit 0) and `make run` (human:
walk with WASD, look with mouse, jump with Space, slide along walls, bonk ceiling).

## Out of Scope

- Stairs / multi-height floors / feet-vs-eye offset (eye is the anchor for now)
- Crouch, run, head-bob, acceleration/friction (instant velocity is fine)
- Collision against anything but the static map grid (no entities yet)
- Textures, networking (later sub-projects)
