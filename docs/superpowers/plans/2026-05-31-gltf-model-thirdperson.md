# glTF Model Loading + Third-Person View Implementation Plan (D1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Load `assets/models/player.glb` via cgltf and render it as the player's flat-shaded third-person avatar standing in the dungeon (camera locked behind), no animation yet.

**Architecture:** A GL-free `read_model` (cgltf) parses 6 rigid mesh-parts (pos/normal/uv + indices + each part's node world transform) → unit-tested. A GL `Model` uploads indexed buffers. A new lit model shader draws parts with `u_model = placement·node_world`. The renderer splits into `begin_frame`/`draw_map`/`draw_model`; the camera offsets the eye behind the player.

**Tech Stack:** C++17, clang++, SDL3, GLAD, cglm, cgltf (vendored). Existing `make test` harness.

---

## Methodology note

`read_model` is GL-free → genuine TDD (the test parses the real `player.glb`). GL
parts (`Model::upload`, shaders, renderer split) are syntax-checked and verified by
`--smoke` + `make run`. The model's GL-free parse (`model.cpp`) and GL upload
(`model_gl.cpp`) are **separate TUs** so the unit test can compile the parser
without a GL/glad include. Tasks 5 changes the renderer signature and leaves
`make build` broken until Task 6 rewrites `main.cpp`; it's verified by
`-fsyntax-only`. Do tasks in order.

## File Structure

- `src/engine/renderer/cgltf_impl.cpp` — one TU: `CGLTF_IMPLEMENTATION` (Task 1).
- `src/engine/renderer/model.h` — `PartData`/`ModelData`/`read_model` + `Part`/`Model` (Task 1).
- `src/engine/renderer/model.cpp` — `read_model` only, GL-free (Task 1).
- `tests/model_load_test.cpp` — parse `player.glb` (Task 1).
- `src/engine/renderer/model_gl.cpp` — `Model::upload`/`destroy`, GL (Task 2).
- `assets/shaders/model.{vert,frag}` — lit solid-color model shader (Task 3).
- `src/engine/renderer/camera.{h,cpp}` — third-person eye offset (Task 4).
- `src/engine/renderer/renderer.{h,cpp}` — `begin_frame`/`draw_map`/`draw_model` (Task 5).
- `src/main.cpp` — load model, new draw loop (Task 6, milestone).

---

### Task 1: glTF parse → ModelData (`read_model`, GL-free, TDD)

**Files:**
- Create: `src/engine/renderer/cgltf_impl.cpp`
- Create: `src/engine/renderer/model.h`
- Create: `src/engine/renderer/model.cpp`
- Create: `tests/model_load_test.cpp`
- Modify: `scripts/test.sh`

- [ ] **Step 1: Create `src/engine/renderer/cgltf_impl.cpp`**

```cpp
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
```

- [ ] **Step 2: Create `src/engine/renderer/model.h`**

```cpp
#pragma once
#include <cstdint>
#include <vector>
#include <cglm/cglm.h>

namespace dc::renderer {

// CPU-side, GL-free. Interleaved vertex = pos(3) + normal(3) + uv(2) = 8 floats.
struct PartData {
    std::vector<float>    vertices;
    std::vector<uint32_t> indices;
    mat4                  node_world;   // rest-pose world transform of this part's node
};
struct ModelData {
    std::vector<PartData> parts;
};

// Parses a .glb/.gltf: every node with a mesh becomes one part per primitive,
// reading POSITION/NORMAL/TEXCOORD_0 + indices and the node's world transform.
// Returns false on failure. GL-free.
bool read_model(const char* path, ModelData& out);

// GL resource: one indexed mesh per part. (Defined in model_gl.cpp.)
struct Part {
    uint32_t vao = 0, vbo = 0, ebo = 0;
    int index_count = 0;
    mat4 node_world;
};
struct Model {
    std::vector<Part> parts;
    void upload(const ModelData& data);
    void destroy();
};

} // namespace dc::renderer
```

- [ ] **Step 3: Create `tests/model_load_test.cpp` (failing test)**

```cpp
#include "engine/renderer/model.h"
#include <cassert>
#include <cstdio>
#include <cstddef>

using namespace dc::renderer;

int main() {
    ModelData md;
    bool ok = read_model("assets/models/player.glb", md);
    assert(ok);
    assert(md.parts.size() == 6);

    float miny = 1e9f, maxy = -1e9f, minx = 1e9f, maxx = -1e9f;
    for (const auto& part : md.parts) {
        assert(!part.vertices.empty());
        assert(!part.indices.empty());
        assert(part.vertices.size() % 8 == 0);
        const std::size_t vcount = part.vertices.size() / 8;
        for (std::size_t v = 0; v < vcount; ++v) {
            vec4 p = { part.vertices[v*8+0], part.vertices[v*8+1], part.vertices[v*8+2], 1.0f };
            vec4 w;
            glm_mat4_mulv(const_cast<vec4*>(&part.node_world[0]) ? part.node_world : part.node_world, p, w);
            if (w[1] < miny) miny = w[1];
            if (w[1] > maxy) maxy = w[1];
            if (w[0] < minx) minx = w[0];
            if (w[0] > maxx) maxx = w[0];
        }
    }
    const float height = maxy - miny;
    assert(height > 1.0f && height < 2.5f);   // ~1.8 tall
    assert(maxx > minx);                       // non-degenerate

    std::printf("PASS model_load\n");
    return 0;
}
```
(Note: `glm_mat4_mulv(part.node_world, p, w)` — write it plainly as
`glm_mat4_mulv(part.node_world, p, w);`. Ignore the defensive cast shown above;
use the simple call.)

Corrected mul line to use in the test:
```cpp
            glm_mat4_mulv(part.node_world, p, w);
```

- [ ] **Step 4: Add to `scripts/test.sh`**, before the `echo "all tests passed"` line:
```bash
build_and_run "$ROOT/tests/model_load_test.cpp" \
  "$ROOT/src/engine/renderer/model.cpp" "$ROOT/src/engine/renderer/cgltf_impl.cpp"
```

- [ ] **Step 5: Run — verify it FAILS**

Run: `make test`
Expected: FAIL — `model.cpp` missing / undefined `read_model`.

- [ ] **Step 6: Create `src/engine/renderer/model.cpp`**

```cpp
#include "engine/renderer/model.h"
#include "cgltf.h"
#include <cstring>

namespace dc::renderer {

bool read_model(const char* path, ModelData& out) {
    out.parts.clear();

    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path, &data) != cgltf_result_success) return false;
    if (cgltf_load_buffers(&options, data, path) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }

    for (cgltf_size n = 0; n < data->nodes_count; ++n) {
        const cgltf_node* node = &data->nodes[n];
        if (!node->mesh) continue;

        float world[16];
        cgltf_node_transform_world(node, world);

        for (cgltf_size p = 0; p < node->mesh->primitives_count; ++p) {
            const cgltf_primitive* prim = &node->mesh->primitives[p];

            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* nrm = nullptr;
            const cgltf_accessor* uv  = nullptr;
            for (cgltf_size a = 0; a < prim->attributes_count; ++a) {
                const cgltf_attribute* at = &prim->attributes[a];
                if (at->type == cgltf_attribute_type_position) pos = at->data;
                else if (at->type == cgltf_attribute_type_normal) nrm = at->data;
                else if (at->type == cgltf_attribute_type_texcoord && at->index == 0) uv = at->data;
            }
            if (!pos) continue;

            PartData part;
            std::memcpy(part.node_world, world, sizeof(float) * 16);

            const cgltf_size vcount = pos->count;
            part.vertices.reserve(vcount * 8);
            for (cgltf_size v = 0; v < vcount; ++v) {
                float pf[3] = {0, 0, 0}, nf[3] = {0, 1, 0}, tf[2] = {0, 0};
                cgltf_accessor_read_float(pos, v, pf, 3);
                if (nrm) cgltf_accessor_read_float(nrm, v, nf, 3);
                if (uv)  cgltf_accessor_read_float(uv, v, tf, 2);
                part.vertices.insert(part.vertices.end(),
                    { pf[0],pf[1],pf[2], nf[0],nf[1],nf[2], tf[0],tf[1] });
            }

            if (prim->indices) {
                const cgltf_size icount = prim->indices->count;
                part.indices.reserve(icount);
                for (cgltf_size i = 0; i < icount; ++i)
                    part.indices.push_back(
                        static_cast<uint32_t>(cgltf_accessor_read_index(prim->indices, i)));
            } else {
                // Non-indexed: synthesize a trivial index list.
                for (cgltf_size i = 0; i < vcount; ++i)
                    part.indices.push_back(static_cast<uint32_t>(i));
            }

            out.parts.push_back(std::move(part));
        }
    }

    cgltf_free(data);
    return !out.parts.empty();
}

} // namespace dc::renderer
```

- [ ] **Step 7: Run — verify PASS**

Run: `make test`
Expected: `PASS model_load` plus prior PASS lines, `all tests passed`.

- [ ] **Step 8: Commit**

```bash
git add src/engine/renderer/cgltf_impl.cpp src/engine/renderer/model.h src/engine/renderer/model.cpp tests/model_load_test.cpp scripts/test.sh
git commit -m "feat(renderer): cgltf model parser (GL-free) with load test"
```

---

### Task 2: Model GL upload/destroy (indexed)

**Files:**
- Create: `src/engine/renderer/model_gl.cpp`

- [ ] **Step 1: Create `src/engine/renderer/model_gl.cpp`**

```cpp
#include "engine/renderer/model.h"
#include <glad/gl.h>

namespace dc::renderer {

void Model::upload(const ModelData& data) {
    destroy();
    parts.reserve(data.parts.size());
    for (const auto& pd : data.parts) {
        Part part;
        part.index_count = static_cast<int>(pd.indices.size());
        std::memcpy(part.node_world, pd.node_world, sizeof(float) * 16);

        glGenVertexArrays(1, &part.vao);
        glGenBuffers(1, &part.vbo);
        glGenBuffers(1, &part.ebo);

        glBindVertexArray(part.vao);

        glBindBuffer(GL_ARRAY_BUFFER, part.vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(pd.vertices.size() * sizeof(float)),
                     pd.vertices.data(), GL_STATIC_DRAW);

        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, part.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     static_cast<GLsizeiptr>(pd.indices.size() * sizeof(uint32_t)),
                     pd.indices.data(), GL_STATIC_DRAW);

        const GLsizei stride = 8 * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, (void*)(6 * sizeof(float)));
        glEnableVertexAttribArray(2);

        glBindVertexArray(0);
        parts.push_back(part);
    }
}

void Model::destroy() {
    for (auto& part : parts) {
        if (part.ebo) glDeleteBuffers(1, &part.ebo);
        if (part.vbo) glDeleteBuffers(1, &part.vbo);
        if (part.vao) glDeleteVertexArrays(1, &part.vao);
    }
    parts.clear();
}

} // namespace dc::renderer
```
(`<cstring>` for `memcpy` is transitively available via model.h's includes; if the
compiler complains, add `#include <cstring>`.)

- [ ] **Step 2: Syntax-check + build**

Run:
```bash
clang++ -std=c++17 -Isrc -Ithird_party -Ithird_party/glad/include -Ithird_party/install/include -fsyntax-only src/engine/renderer/model_gl.cpp && echo OK
```
Expected: `OK`.

Run: `make build`
Expected: `built: .../build/dungeoncrawl` (model_gl.cpp links into the app; nothing calls it yet).

- [ ] **Step 3: Commit**

```bash
git add src/engine/renderer/model_gl.cpp
git commit -m "feat(renderer): Model GL upload/destroy (indexed per-part buffers)"
```

---

### Task 3: Model shaders

**Files:**
- Create: `assets/shaders/model.vert`
- Create: `assets/shaders/model.frag`

- [ ] **Step 1: Create `assets/shaders/model.vert`**

```glsl
#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec2 a_uv;
uniform mat4 u_viewproj;
uniform mat4 u_model;
out vec3 v_normal;
void main() {
    mat3 nm = mat3(transpose(inverse(u_model)));
    v_normal = normalize(nm * a_normal);
    gl_Position = u_viewproj * u_model * vec4(a_pos, 1.0);
}
```

- [ ] **Step 2: Create `assets/shaders/model.frag`**

```glsl
#version 330 core
in vec3 v_normal;
uniform vec3 u_color;
out vec4 frag_color;
void main() {
    vec3 light_dir = normalize(vec3(0.4, 1.0, 0.3));
    float diffuse = max(dot(normalize(v_normal), light_dir), 0.0);
    float shade = 0.35 + 0.65 * diffuse;
    frag_color = vec4(u_color * shade, 1.0);
}
```

- [ ] **Step 3: Commit**

```bash
git add assets/shaders/model.vert assets/shaders/model.frag
git commit -m "feat(renderer): flat-lit model shader"
```

---

### Task 4: Camera third-person offset

**Files:**
- Modify: `src/engine/renderer/camera.h`
- Modify: `src/engine/renderer/camera.cpp`

- [ ] **Step 1: In `src/engine/renderer/camera.h`, add a `distance` field** to `struct Camera` (after `far_z`):
```cpp
    float distance = 4.0f;   // third-person: how far the eye sits behind the player
```

- [ ] **Step 2: Replace `Camera::view_matrix` in `src/engine/renderer/camera.cpp`**

```cpp
void Camera::view_matrix(mat4 out, const dc::entity::Player& p) const {
    vec3 f;  p.front(f);
    vec3 up = {0.0f, 1.0f, 0.0f};
    vec3 target = { p.position[0], p.position[1], p.position[2] };
    vec3 eye = { target[0] - f[0] * distance,
                 target[1] - f[1] * distance,
                 target[2] - f[2] * distance };
    glm_look(eye, f, up, out);
}
```

- [ ] **Step 3: Verify tests still pass + build**

Run: `make test`
Expected: all PASS incl. `PASS camera` (the existing view test — player at origin
looking +X, world point at +X=5 — still maps to negative view-space Z because the
eye now sits at −4 on X and the point is still ahead of it).

Run: `make build`
Expected: `built: .../build/dungeoncrawl` (renderer still calls the unchanged
`view_matrix` signature; nothing else changed yet).

- [ ] **Step 4: Commit**

```bash
git add src/engine/renderer/camera.h src/engine/renderer/camera.cpp
git commit -m "feat(renderer): third-person camera (eye offset behind player)"
```

---

### Task 5: Renderer split — begin_frame / draw_map / draw_model

**Files:**
- Modify (full replace): `src/engine/renderer/renderer.h`
- Modify (full replace): `src/engine/renderer/renderer.cpp`

NOTE: This INTENTIONALLY breaks `make build` (main.cpp still calls the old
`render()`). Gate: `-fsyntax-only` on renderer.cpp. Do NOT touch main.cpp.

- [ ] **Step 1: Replace `src/engine/renderer/renderer.h`**

```cpp
#pragma once
#include <cstdint>
#include <cglm/cglm.h>
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/model.h"

namespace dc::entity { struct Player; }

namespace dc::renderer {

struct Renderer {
    uint32_t world_program = 0;   // textured map
    uint32_t model_program = 0;   // flat-lit model
    uint32_t texture = 0;         // GL_TEXTURE_2D_ARRAY for the map
    int world_viewproj_loc = -1;
    int model_viewproj_loc = -1, model_model_loc = -1, model_color_loc = -1;
    mat4 viewproj;                // computed each frame in begin_frame

    bool init();
    // Set viewport, clear color+depth, compute the frame's view-projection.
    void begin_frame(const Camera& camera, const dc::entity::Player& player, int fb_w, int fb_h);
    // Draw the textured map mesh.
    void draw_map(const Mesh& mesh);
    // Draw a model at `placement` (each part uses placement * node_world), flat color.
    void draw_model(const Model& model, mat4 placement, vec3 color);
    void shutdown();
};

} // namespace dc::renderer
```

- [ ] **Step 2: Replace `src/engine/renderer/renderer.cpp`**

```cpp
#include "engine/renderer/renderer.h"
#include "engine/renderer/shader.h"
#include "engine/renderer/texture.h"
#include "engine/entity/player.h"

#include <glad/gl.h>

namespace dc::renderer {

bool Renderer::init() {
    world_program = load_program("assets/shaders/world.vert", "assets/shaders/world.frag");
    if (!world_program) return false;
    world_viewproj_loc = glGetUniformLocation(world_program, "u_viewproj");

    model_program = load_program("assets/shaders/model.vert", "assets/shaders/model.frag");
    if (!model_program) { shutdown(); return false; }
    model_viewproj_loc = glGetUniformLocation(model_program, "u_viewproj");
    model_model_loc    = glGetUniformLocation(model_program, "u_model");
    model_color_loc    = glGetUniformLocation(model_program, "u_color");

    const char* paths[3] = {
        "assets/textures/stonefloor0.png",
        "assets/textures/stonewall0.png",
        "assets/textures/stoneceiling0.png",
    };
    texture = load_texture_array(paths, 3);
    if (!texture) { shutdown(); return false; }

    glUseProgram(world_program);
    glUniform1i(glGetUniformLocation(world_program, "u_tex"), 0);

    glEnable(GL_DEPTH_TEST);
    return true;
}

void Renderer::begin_frame(const Camera& camera, const dc::entity::Player& player,
                           int fb_w, int fb_h) {
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.05f, 0.05f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = (fb_h > 0) ? static_cast<float>(fb_w) / fb_h : 1.0f;
    mat4 view, proj;
    camera.view_matrix(view, player);
    camera.proj_matrix(proj, aspect);
    glm_mat4_mul(proj, view, viewproj);
}

void Renderer::draw_map(const Mesh& mesh) {
    glUseProgram(world_program);
    glUniformMatrix4fv(world_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    mesh.draw();
}

void Renderer::draw_model(const Model& model, mat4 placement, vec3 color) {
    glUseProgram(model_program);
    glUniformMatrix4fv(model_viewproj_loc, 1, GL_FALSE, reinterpret_cast<const float*>(viewproj));
    glUniform3fv(model_color_loc, 1, color);
    for (const auto& part : model.parts) {
        mat4 m;
        glm_mat4_mul(placement, const_cast<vec4*>(part.node_world), m);
        glUniformMatrix4fv(model_model_loc, 1, GL_FALSE, reinterpret_cast<const float*>(m));
        glBindVertexArray(part.vao);
        glDrawElements(GL_TRIANGLES, part.index_count, GL_UNSIGNED_INT, nullptr);
    }
    glBindVertexArray(0);
}

void Renderer::shutdown() {
    if (texture) glDeleteTextures(1, &texture);
    if (world_program) glDeleteProgram(world_program);
    if (model_program) glDeleteProgram(model_program);
    world_program = model_program = texture = 0;
    world_viewproj_loc = model_viewproj_loc = model_model_loc = model_color_loc = -1;
}

} // namespace dc::renderer
```
(If `glm_mat4_mul(placement, const_cast<vec4*>(part.node_world), m)` triggers a
const/type warning, copy first: `mat4 nw; glm_mat4_copy((vec4*)part.node_world, nw); glm_mat4_mul(placement, nw, m);`)

- [ ] **Step 3: Syntax-check**

Run:
```bash
clang++ -std=c++17 -Isrc -Ithird_party -Ithird_party/glad/include -Ithird_party/install/include -fsyntax-only src/engine/renderer/renderer.cpp && echo OK
```
Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
git add src/engine/renderer/renderer.h src/engine/renderer/renderer.cpp
git commit -m "feat(renderer): split frame into begin_frame/draw_map/draw_model"
```

---

### Task 6: Wire model + third-person into main (milestone)

**Files:**
- Modify (full replace): `src/main.cpp`

- [ ] **Step 1: Confirm build is currently broken (expected before-state)**

Run: `make build 2>&1 | tail -5 || true`
Expected: error in main.cpp — `renderer.render(...)` no longer exists.

- [ ] **Step 2: Replace `src/main.cpp`**

```cpp
#include "engine/platform/window.h"
#include "engine/input/input.h"
#include "engine/renderer/renderer.h"
#include "engine/renderer/mesh.h"
#include "engine/renderer/camera.h"
#include "engine/renderer/model.h"
#include "engine/entity/player.h"
#include "engine/world/map.h"
#include "engine/world/map_mesh.h"

#include <SDL3/SDL.h>
#include <cglm/cglm.h>
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
    if (text.empty()) { std::fprintf(stderr, "could not read map: %s\n", map_path); return 1; }
    auto map = dc::world::parse_map(text);
    if (!map) { std::fprintf(stderr, "could not parse map: %s\n", map_path); return 1; }

    dc::renderer::ModelData model_data;
    if (!dc::renderer::read_model("assets/models/player.glb", model_data)) {
        std::fprintf(stderr, "could not load model: assets/models/player.glb\n");
        return 1;
    }

    dc::platform::Window window;
    if (!window.init("dungeoncrawl")) return 1;

    dc::renderer::Renderer renderer;
    if (!renderer.init()) { window.shutdown(); return 1; }

    dc::renderer::Mesh mesh;
    mesh.upload(dc::world::build_map_mesh(*map));

    dc::renderer::Model player_model;
    player_model.upload(model_data);

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
        if (dt > 0.05f) dt = 0.05f;

        player.add_look(input.mouse_dx, input.mouse_dy);
        float forward = (input.key_down(SDL_SCANCODE_W) ? 1.0f : 0.0f)
                      - (input.key_down(SDL_SCANCODE_S) ? 1.0f : 0.0f);
        float strafe  = (input.key_down(SDL_SCANCODE_D) ? 1.0f : 0.0f)
                      - (input.key_down(SDL_SCANCODE_A) ? 1.0f : 0.0f);
        bool jump = input.key_down(SDL_SCANCODE_SPACE);
        player.update(forward, strafe, jump, dt, *map);

        // Avatar placement: stand on the floor at the player's XZ, facing player.yaw.
        mat4 placement;
        glm_mat4_identity(placement);
        vec3 foot = { player.position[0], 0.0f, player.position[2] };
        glm_translate(placement, foot);
        glm_rotate_y(placement, -player.yaw, placement);  // sign/offset tuned visually

        int w, h; window.framebuffer_size(w, h);
        renderer.begin_frame(camera, player, w, h);
        renderer.draw_map(mesh);
        vec3 player_color = { 0.80f, 0.45f, 0.35f };
        renderer.draw_model(player_model, placement, player_color);
        window.swap();

        if (smoke) { std::printf("smoke: one frame rendered, exiting\n"); break; }
    }

    player_model.destroy();
    mesh.destroy();
    renderer.shutdown();
    window.shutdown();
    return 0;
}
```

- [ ] **Step 3: Build**

Run: `make build`
Expected: `built: .../build/dungeoncrawl`.

- [ ] **Step 4: Headless smoke (from repo ROOT)**

Run: `./build/dungeoncrawl --smoke`
Expected: a `GL ...` line, then `smoke: one frame rendered, exiting`, exit 0. Confirm `echo $?` → `0`.

- [ ] **Step 5: Full gate**

Run: `make test && make build && ./build/dungeoncrawl --smoke`
Expected: all unit suites PASS (incl. `model_load`), build OK, smoke exits 0.

- [ ] **Step 6: Interactive visual check (human, do not run from automation)**

`make run` → a shaded character stands in the room, seen from behind; mouse turns
the character (camera follows behind), WASD walks it around, Space jumps, you still
slide along walls. If the character faces backwards/sideways, adjust the
`glm_rotate_y(placement, -player.yaw, …)` sign or add a `+ glm_rad(90.0f)` offset.

- [ ] **Step 7: Commit**

```bash
git add src/main.cpp
git commit -m "feat: render player as third-person glTF avatar (milestone)"
```

---

## Self-Review

**Spec coverage:**
- cgltf loader, embedded .glb, per-part mesh + node_world → Task 1 ✓
- Indexed GL upload (`glDrawElements`) → Tasks 2, 5 ✓
- Flat-lit model shader (u_viewproj/u_model/u_color, inverse-transpose normal) → Task 3 ✓
- Third-person camera eye offset (distance behind) → Task 4 ✓
- Renderer begin_frame/draw_map/draw_model (supports multiple models) → Task 5 ✓
- main: load model, place avatar at player (feet on floor, faces yaw), camera-locked-behind controls reuse player yaw/pitch → Task 6 ✓
- model_load unit test (6 parts, non-empty, sane height) → Task 1 ✓

**Placeholder scan:** No TBD/placeholder steps; full code given. The yaw sign/offset
is an explicit "tune visually" note (the build/smoke don't depend on it), not a gap.
The Task 1 test had a defensive-cast typo in the mul line; the corrected plain call
`glm_mat4_mulv(part.node_world, p, w);` is given right after — use that.

**Type consistency:**
- 8-float model vertex (pos/normal/uv) consistent across `read_model` (Task 1),
  `Model::upload` attribs (Task 2), `model.vert` locations 0–2 (Task 3).
- `ModelData`/`PartData`/`read_model` (Task 1) used by `model_gl.cpp` (Task 2),
  renderer (Task 5), main (Task 6) identically.
- `Part.node_world`/`index_count`/`vao` used consistently Tasks 2/5.
- Renderer fields/methods (`begin_frame`, `draw_map`, `draw_model(const Model&, mat4, vec3)`)
  consistent Task 5 ↔ Task 6 calls.
- `Camera::view_matrix(mat4, const Player&)` signature unchanged (Task 4 only adds
  the `distance` field + reimplements the body) — renderer/camera_test unaffected.

**Ordering note:** Task 5 changes the renderer API and leaves `make build` broken
(main on the old `render()`); verified by `-fsyntax-only`. Task 6 restores the full
`make build` + `--smoke` gate. Tasks 1–4 each keep `make test`/`make build` green.
