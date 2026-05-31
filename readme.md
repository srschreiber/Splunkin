# dungeoncrawl

A Barony-inspired 3D dungeon crawler in C++. This is the project scaffold: a
single binary that opens an SDL3 + OpenGL 3.3 core window and draws a triangle
through the core `process_input → update(dt) → render` loop. Every other engine
subsystem is stubbed.

See the design spec and implementation plan under
[`docs/superpowers/`](docs/superpowers/).

## Requirements

These build tools must be on your `PATH` (macOS):

- `git`, `clang++`, `cmake`, `python3` + `pip`, `pkg-config`

## Build & run

```sh
make setup    # one-time: fetch vendored headers, generate GLAD, build submodules
make          # compile + link → build/dungeoncrawl
make run      # build, then launch the game (opens a window; Esc or close to quit)
make clean    # remove build/
```

`make setup` clones and compiles SDL3 from source via CMake the first time, which
takes several minutes. Subsequent `make` builds are fast.

### Smoke check (no window interaction)

```sh
./build/dungeoncrawl --smoke
```

Initializes SDL + GL, renders exactly one frame, prints the GL version, and exits
`0`. Useful for verifying the toolchain without a human at the keyboard.

> **Run from the repo root.** The renderer loads shaders via the relative path
> `assets/shaders/tri.{vert,frag}`, so the binary must be launched with the repo
> root as the working directory. `make run` handles this for you. (Making asset
> paths location-independent is a planned follow-up.)

> On macOS the GL version reports as `GL 4.1 Metal ...` — requesting a 3.3 core
> context yields the highest supported core profile (4.1) via the Metal-backed
> driver. The 3.3 shaders run fine on it.

## Layout

| Path | Purpose |
|---|---|
| `Makefile` | thin entry point; delegates to `scripts/` |
| `scripts/` | `setup.sh`, `build.sh`, `run.sh`, `clean.sh` |
| `src/main.cpp` | entry point + core loop |
| `src/engine/platform/` | SDL window + GL context (implemented) |
| `src/engine/renderer/` | shader loader + triangle (implemented) |
| `src/engine/{input,audio,assets,world,entity,ui,game}/` | stub headers |
| `src/engine/net/` | inert UDP (gameplay) + TCP (login) channel sketches |
| `assets/shaders/` | GLSL shaders |
| `third_party/` | vendored single-header libs, generated GLAD, and submodules |

## Build notes

- C++17, clang++. `build.sh` discovers all `src/**/*.cpp` and recompiles them on
  every build (intentional clean rebuild for the scaffold; no incremental
  dependency tracking yet).
- Dependencies are vendored: single-header libs (`stb_image`, `stb_truetype`,
  `cgltf`, `nuklear`) and a generated GLAD live under `third_party/`; SDL3, cglm,
  enet, and ReactPhysics3D are git submodules built as static libs into
  `third_party/install/` by `setup.sh`.
- `net/`, `enet`, and ReactPhysics3D are present but not yet wired into anything.
