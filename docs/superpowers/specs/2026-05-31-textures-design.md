# Textured Map Rendering Design (Sub-project B)

**Date:** 2026-05-31
**Status:** Approved

## Goal

Texture the dungeon: floor, walls, and ceiling sample 32×32 stone textures
instead of flat colors. Keep the single static mesh + single draw call. Crisp
(nearest) filtering for a pixel-art look. Maps stay hardcoded.

This is Sub-project B of the roadmap: A (player+collision, done) → **B textures**
→ C networking → D character model.

## Decisions

| Decision | Choice |
|---|---|
| Multi-texture strategy | One `GL_TEXTURE_2D_ARRAY`, 3 layers (floor=0, wall=1, ceiling=2) |
| Filtering | `GL_NEAREST` (crisp); `GL_REPEAT` wrap |
| Ceiling texture | `stoneceiling0.png` = copy of `stonewall0.png` (so it can diverge later) |
| Vertex layout | `color(3)` replaced by `uv(2)+layer(1)` — still 9 floats |
| Tiling | One texture tile per cell face; walls repeat vertically |
| Draw calls | Still one (single mesh, single `glDrawArrays`) |

## Approach

All three textures are 32×32 RGBA, so they stack cleanly into a `GL_TEXTURE_2D_ARRAY`
with 3 layers. Each mesh vertex carries a UV and a material layer index. The
fragment shader samples `sampler2DArray` at `(uv, layer)` and multiplies by the
existing directional-light shade. Rejected: separate draw call per material
(more draws, mesh split) and a packed atlas (manual UV math, bleeding at edges).

## Assets

- `assets/textures/stonefloor0.png` (32×32 RGBA) — floor, layer 0
- `assets/textures/stonewall0.png` (32×32 RGBA) — walls, layer 1
- `assets/textures/stoneceiling0.png` (32×32 RGBA, copy of wall) — ceiling, layer 2

## Vertex Layout (unchanged 9-float stride)

`pos.x,y,z, normal.x,y,z, u,v, layer` — 9 floats/vertex.
Mesh attributes:
- loc 0 `a_pos`    vec3, offset 0
- loc 1 `a_normal` vec3, offset 3 floats
- loc 2 `a_uv`     vec2, offset 6 floats
- loc 3 `a_layer`  float, offset 8 floats

## UV Mapping

`TILE = 2.0`, `WALL_HEIGHT = 3.0`.
- **Floor / ceiling** quads: the four corners map to UV (0,0)…(1,1) — one tile of
  the texture per cell.
- **Wall** quads: `u` runs 0→1 across the cell's horizontal extent; `v` runs
  0→`WALL_HEIGHT/TILE` (= 1.5) bottom→top, so texels stay square and the texture
  repeats up the wall via `GL_REPEAT`.
- `layer`: floor → 0.0, wall → 1.0, ceiling → 2.0 (baked per vertex).

## Components

### Texture loader (`renderer/texture.{h,cpp}`)
```cpp
namespace dc::renderer {
// Loads `count` same-sized RGBA images into a GL_TEXTURE_2D_ARRAY (layer i = paths[i]).
// Nearest filtering, GL_REPEAT wrap. Returns the GL texture id, or 0 on failure.
uint32_t load_texture_array(const char* const* paths, int count);
}
```
- Uses `stbi_load(path, &w, &h, &n, 4)` (force RGBA). First image sets w/h;
  subsequent images must match (else fail, free, return 0).
- `glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, w, h, count, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr)`
  then `glTexSubImage3D(... layer i ...)` per image.
- `GL_TEXTURE_MIN_FILTER`/`MAG_FILTER` = `GL_NEAREST`; `WRAP_S`/`WRAP_T` = `GL_REPEAT`.
- `stbi_image_free` each decoded buffer.

### stb_image implementation TU (`renderer/stb_image_impl.cpp`)
```cpp
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
```
Single translation unit that emits stb_image's definitions (header is vendored at
`third_party/stb_image.h`; build.sh already includes `-Ithird_party`).

### Map mesh (`world/map_mesh.{h,cpp}` change)
Replace the per-vertex color with `(u, v, layer)`. The interleaved comment and
the `push_*` helpers change from a `Color` to a `(u,v,layer)` triple. Floor/ceiling
emit corner UVs 0..1; walls emit `u`=0..1, `v`=0..(WALL_HEIGHT/TILE). Layer
constants: `LAYER_FLOOR=0`, `LAYER_WALL=1`, `LAYER_CEILING=2`. Vertex count and
winding are unchanged (normal stays at offsets 3–5).

### Mesh (`renderer/mesh.cpp` change)
Attribute setup: loc 2 becomes `vec2` uv at offset `6*sizeof(float)`; add loc 3
`float` layer at offset `8*sizeof(float)`. Stride unchanged (`9*sizeof(float)`).

### Shaders
`world.vert`:
```glsl
#version 330 core
layout(location=0) in vec3 a_pos;
layout(location=1) in vec3 a_normal;
layout(location=2) in vec2 a_uv;
layout(location=3) in float a_layer;
uniform mat4 u_viewproj;
out vec3 v_normal;
out vec2 v_uv;
flat out float v_layer;
void main(){ v_normal=a_normal; v_uv=a_uv; v_layer=a_layer;
             gl_Position = u_viewproj * vec4(a_pos,1.0); }
```
`world.frag`:
```glsl
#version 330 core
in vec3 v_normal;
in vec2 v_uv;
flat in float v_layer;
uniform sampler2DArray u_tex;
out vec4 frag_color;
void main(){
    vec3 light_dir = normalize(vec3(0.4,1.0,0.3));
    float diffuse = max(dot(normalize(v_normal), light_dir), 0.0);
    float shade = 0.35 + 0.65*diffuse;
    vec4 tex = texture(u_tex, vec3(v_uv, v_layer));
    frag_color = vec4(tex.rgb * shade, 1.0);
}
```

### Renderer (`renderer/renderer.{h,cpp}` change)
- Add `uint32_t texture = 0;`.
- `init()`: after the program links, load the array
  `load_texture_array({"assets/textures/stonefloor0.png","assets/textures/stonewall0.png","assets/textures/stoneceiling0.png"}, 3)`;
  cache the `u_tex` sampler location and set it to texture unit 0 once.
- `render()`: `glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D_ARRAY, texture);`
  before drawing.
- `shutdown()`: `glDeleteTextures(1, &texture)`.

## Testing

- **`tests/map_mesh_test.cpp`** (updated, TDD): keep the vertex-count assertion
  (still `(8+8+4)*6*9` for the 3×3 center-solid map) and the winding assertion
  (normal at offsets 3–5). Add: floor-cell quads carry layer 0 at offset 8,
  ceiling layer 2, the center solid's wall quads layer 1; UVs lie in `[0, 1.5]`.
- **`tests/texture_decode_test.cpp`** (new, GL-free): `stbi_load` each of the three
  PNGs forcing 4 channels; assert each is 32×32 with 4 channels; `stbi_image_free`.
  This verifies the assets exist and the same-size array assumption. Compiled with
  a small `STB_IMAGE_IMPLEMENTATION` shim in the test (or links
  `renderer/stb_image_impl.cpp`).
- GL pieces (array upload, attribute layout, sampler binding, shader) are verified
  by `--smoke` (one frame, exit 0) and `make run` (human: textured surfaces).

## Out of Scope

- Mipmaps, anisotropic filtering
- Normal/specular/PBR maps
- Per-cell material variation (all walls share one texture)
- Texture hot-reload, runtime texture swapping
