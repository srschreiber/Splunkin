# glTF Model Loading + Third-Person View Design (Sub-project D1)

**Date:** 2026-05-31
**Status:** Approved

## Goal

Load `assets/models/player.glb` via `cgltf` and render it as the player's
**third-person avatar** standing in the dungeon — flat-shaded solid color, lit by
the existing directional light, viewed from a camera locked behind the player.
No animation yet (the "walk" animation is D2). Structured so animation drops in
cleanly later.

This is Sub-project D1 of the roadmap: A (player+collision) ✅ · B (textures) ✅ ·
**D1 (this)** · C (networking) · D2 (model animation).

## What the asset actually is (verified from player.glb)

- 6 meshes (the body boxes), each indexed triangles with `POSITION`, `NORMAL`,
  `TEXCOORD_0`. **No `JOINTS_0`/`WEIGHTS_0`** → rigid parts, not vertex-skinned.
- A node hierarchy + 1 skin + 1 animation named `walk` (translation/rotation/scale
  channels on nodes). The rig drives **node transforms** (rigid), which is the
  easy-to-animate structure.
- **0 materials, 0 textures, 0 images** — exported without a material. So the
  model has UVs but no texture; D1 renders it flat-shaded. (Texture later, after a
  material is exported from Blender.)

## Decisions

| Decision | Choice |
|---|---|
| Loader | `cgltf` (vendored), embedded-buffer `.glb` |
| Geometry | Indexed (`glDrawElements`); model path is separate from the map's non-indexed mesh |
| Shading | Flat solid color × existing directional light (no texture — file has none) |
| Part structure | Per-part GPU mesh + node world-transform (rest pose) — animation-ready |
| View | Third-person, camera locked behind the player |
| Controls | Mouse turns the player (existing `player.yaw/pitch`); camera sits behind |
| Camera collision | None this milestone (camera may clip a wall directly behind) |
| Placement | Avatar at the player each frame; feet on floor; `player.position` semantics unchanged |

## Approach: preserve parts for easy animation

The model is 6 rigid parts in a node hierarchy. The loader keeps **per-part data**
(its mesh + its node world-transform from `cgltf_node_transform_world`). D1 draws
each part with `u_model = placement · node_world`. D2 will replace `node_world`
with an animated transform per frame — minimal change. Vertices stay in their
node-local space (not pre-baked), so the same buffers animate later.

## Components

### cgltf implementation TU (`renderer/cgltf_impl.cpp`)
```cpp
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
```
Single TU emitting cgltf's definitions (header vendored at `third_party/cgltf.h`;
build/test already pass `-Ithird_party`).

### Model (`renderer/model.h`, `renderer/model.cpp`)
GL-free CPU read split from GL upload so the parse is unit-testable.
```cpp
#include <cstdint>
#include <vector>
#include <cglm/cglm.h>

namespace dc::renderer {

// CPU-side, GL-free. Interleaved vertex = pos(3) + normal(3) + uv(2) = 8 floats.
struct PartData {
    std::vector<float>    vertices;     // 8 floats/vertex
    std::vector<uint32_t> indices;
    mat4                  node_world;   // rest-pose world transform of this part's node
};
struct ModelData {
    std::vector<PartData> parts;
};
// Parses a .glb/.gltf, reads each mesh node's primitive (POSITION/NORMAL/TEXCOORD_0
// + indices) and its world transform. Returns false on failure. GL-free.
bool read_model(const char* path, ModelData& out);

// GL resource: one indexed mesh per part.
struct Part {
    uint32_t vao = 0, vbo = 0, ebo = 0;
    int index_count = 0;
    mat4 node_world;
};
struct Model {
    std::vector<Part> parts;
    void upload(const ModelData& data);  // GL: builds VAO/VBO/EBO per part
    void destroy();
};

} // namespace dc::renderer
```
- `read_model`: `cgltf_parse_file` + `cgltf_load_buffers`. For each node with a
  mesh, for its primitive: read POSITION/NORMAL/TEXCOORD_0 via
  `cgltf_accessor_read_float`, indices via `cgltf_accessor_unpack_indices` (to
  `uint32_t`), and `cgltf_node_transform_world(node, m)` for `node_world`.
  Interleave into `vertices`. Missing TEXCOORD_0 → fill (0,0).
- `Model::upload`: per part, VAO + VBO (8-float interleave: loc0 pos@0, loc1
  normal@3, loc2 uv@6) + EBO; `index_count = indices.size()`.
- Vertex attributes for the model path: loc0 pos(3), loc1 normal(3), loc2 uv(2),
  stride 8 floats.

### Model shaders (`assets/shaders/model.vert`, `model.frag`)
`model.vert`:
```glsl
#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec2 a_uv;
uniform mat4 u_viewproj;
uniform mat4 u_model;
out vec3 v_normal;
void main() {
    mat3 nm = mat3(transpose(inverse(u_model)));   // correct for non-uniform box scale
    v_normal = normalize(nm * a_normal);
    gl_Position = u_viewproj * u_model * vec4(a_pos, 1.0);
}
```
`model.frag`:
```glsl
#version 330 core
in vec3 v_normal;
uniform vec3 u_color;     // flat base color (no texture yet)
out vec4 frag_color;
void main() {
    vec3 light_dir = normalize(vec3(0.4, 1.0, 0.3));
    float diffuse = max(dot(normalize(v_normal), light_dir), 0.0);
    float shade = 0.35 + 0.65 * diffuse;
    frag_color = vec4(u_color * shade, 1.0);
}
```

### Camera third-person (`renderer/camera.{h,cpp}` change)
Add `float distance = 4.0f;`. `view_matrix` offsets the eye behind the player:
```cpp
void Camera::view_matrix(mat4 out, const dc::entity::Player& p) const {
    vec3 f;  p.front(f);
    vec3 up = {0,1,0};
    vec3 target = { p.position[0], p.position[1], p.position[2] };
    vec3 eye = { target[0] - f[0]*distance,
                 target[1] - f[1]*distance,
                 target[2] - f[2]*distance };
    glm_look(eye, f, up, out);   // look from behind, along +front toward/through the player
}
```
Pitch orbits up/down, yaw swings around — all from existing `player.yaw/pitch`.

### Renderer (`renderer/renderer.{h,cpp}` change) — split frame into begin + draws
To draw the map *and* one-or-more models in a frame (and scale to other players
later), replace the single `render()` with:
```cpp
struct Renderer {
    uint32_t world_program = 0;   // map (textured)
    uint32_t model_program = 0;   // model (flat-lit)
    uint32_t texture = 0;
    int world_viewproj_loc = -1;
    int model_viewproj_loc = -1, model_model_loc = -1, model_color_loc = -1;
    mat4 viewproj;                // cached for the current frame

    bool init();   // loads world.{vert,frag} + model.{vert,frag} + texture array
    void begin_frame(const Camera&, const dc::entity::Player&, int w, int h); // viewport, clear, compute viewproj
    void draw_map(const Mesh& mesh);                                          // world program + texture
    void draw_model(const Model& model, const mat4 placement, const vec3 color); // model program, per-part u_model
    void shutdown();
};
```
- `begin_frame`: `glViewport`, clear color+depth, compute `viewproj = proj·view(player)`.
- `draw_map`: `glUseProgram(world_program)`, set `u_viewproj`, bind texture array, `mesh.draw()`.
- `draw_model`: `glUseProgram(model_program)`, set `u_viewproj`, `u_color`; for each
  part set `u_model = placement · part.node_world` and draw it (`glDrawElements`).

### main.cpp
- Load the model: `ModelData md; read_model("assets/models/player.glb", md);`
  (error + exit 1 on failure) → `Model player_model; player_model.upload(md);`.
- Each frame:
  ```
  renderer.begin_frame(camera, player, w, h);
  renderer.draw_map(mesh);
  mat4 placement;  // translate to (player.x, 0, player.z) then rotateY(player.yaw)
  renderer.draw_model(player_model, placement, color /* e.g. {0.8,0.4,0.3} */);
  window.swap();
  ```
- `placement`: `glm_translate` to `(player.position[0], 0, player.position[2])`,
  then `glm_rotate_y` by the player's facing. The exact yaw sign/offset to make the
  model face forward is determined during the visual check (a constant tweak).
- `player_model.destroy()` on exit.

## Testing

- **`tests/model_load_test.cpp`** (GL-free, cgltf + cglm): `read_model` on
  `assets/models/player.glb`; assert `parts.size() == 6`; each part has non-empty
  `vertices` and `indices`; compute the model's overall AABB by transforming each
  part's positions by its `node_world` and assert the height is sane (between 1.0
  and 2.5 units) and non-degenerate. Verifies parse + transforms without GL.
- GL pieces (upload, model shader, third-person camera, draw split) verified by
  `--smoke` (one frame, exit 0) and `make run` (human: a shaded character standing
  in the room, seen from behind; mouse turns it, WASD walks it, camera follows).

## Out of scope (later)

- The `walk` animation (D2: sample node TRS, recompute per-part `node_world`).
- Texturing the model (needs a material/texture exported from Blender).
- Per-vertex skinning (unneeded — rigid parts).
- Camera-collision / pull-in when a wall is behind the player.
- Multiple model instances (the `draw_model` API already supports it for when
  networking adds other players).
