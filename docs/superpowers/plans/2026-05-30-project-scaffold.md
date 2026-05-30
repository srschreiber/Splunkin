# Project Scaffold Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stand up a buildable C++ skeleton for a Barony-inspired dungeon crawler whose first runnable target opens an SDL3 + OpenGL 3.3 window and draws a triangle through the core game loop, with every other engine subsystem stubbed.

**Architecture:** A single executable. A thin `Makefile` delegates to `scripts/`. `setup.sh` vendors single-header libs, generates GLAD, and builds submodules (SDL3, cglm, ReactPhysics3D, enet) into `third_party/install/`. `build.sh` compiles `src/**` with clang++ (C++17) and links SDL3 statically. `platform/` and `renderer/` are real; all other `engine/` subsystems (including `net/`, which is inert) are stub headers.

**Tech Stack:** C++17, clang++, SDL3, GLAD2 (gl core 3.3), cglm (header-only), stb_image, stb_truetype, cgltf, Nuklear, enet, ReactPhysics3D. Build tools required on PATH: `git`, `clang++`, `cmake`, `python3`/`pip`, `pkg-config`.

---

## Methodology note

There is no application logic yet, so classic unit tests don't apply. Each task's
"failing test → implement → passing test" cycle uses **verification commands**:
run the build/check first to see it fail, implement, run again to see it pass.
The GUI milestone is made verifiable headlessly via a `--smoke` flag on the
binary that initializes, renders exactly one frame, and exits `0`.

## File Structure

- `Makefile` — thin targets (`setup`, `build`, `run`, `clean`) that call scripts.
- `scripts/setup.sh` — fetch headers, generate GLAD, init+build submodules.
- `scripts/build.sh` — compile + link → `build/dungeoncrawl`.
- `scripts/run.sh` — build then run (passes through `$ARGS`).
- `scripts/clean.sh` — remove `build/`.
- `.gitignore`, `.gitmodules` — repo config.
- `src/main.cpp` — entry point and core loop.
- `src/engine/platform/window.{h,cpp}` — SDL window + GL context lifecycle.
- `src/engine/renderer/shader.{h,cpp}` — compile/link GLSL from files.
- `src/engine/renderer/renderer.{h,cpp}` — triangle VAO/VBO + draw.
- `src/engine/{input,audio,assets,world,entity,ui,game}/*.h` — stub headers.
- `src/engine/net/{udp_channel,tcp_channel}.h` — inert interface sketches.
- `assets/shaders/tri.{vert,frag}` — triangle shaders.

---

### Task 1: Repo skeleton, .gitignore, directory tree

**Files:**
- Create: `.gitignore`
- Create: `.gitmodules` (empty placeholder, populated in Task 2)
- Create: directory tree with `.gitkeep` placeholders

- [ ] **Step 1: Verify the tree does not yet exist (failing check)**

Run: `test -f .gitignore && echo PRESENT || echo MISSING`
Expected: `MISSING`

- [ ] **Step 2: Create `.gitignore`**

```gitignore
/build/
/third_party/install/
/third_party/glad/
/third_party/*.h
# vendored submodule sources are tracked via .gitmodules, their build artifacts are not
__pycache__/
*.o
.DS_Store
```

- [ ] **Step 3: Create directory tree with placeholders**

Run:
```bash
mkdir -p scripts src/main src/engine/{platform,renderer,input,audio,assets,world,entity,ui,net,game} \
         assets/shaders third_party
touch .gitmodules
find src/engine -type d -empty -exec touch {}/.gitkeep \;
```

- [ ] **Step 4: Verify the tree exists (passing check)**

Run: `test -f .gitignore && test -d src/engine/net && test -d scripts && echo OK`
Expected: `OK`

- [ ] **Step 5: Commit**

```bash
git add .gitignore .gitmodules src assets scripts
git commit -m "chore: scaffold directory tree and gitignore"
```

---

### Task 2: setup.sh — vendor deps, generate GLAD, build submodules

**Files:**
- Create: `scripts/setup.sh`
- Modify: `.gitmodules` (via `git submodule add`)

- [ ] **Step 1: Verify deps absent (failing check)**

Run: `test -f third_party/stb_image.h && echo PRESENT || echo MISSING`
Expected: `MISSING`

- [ ] **Step 2: Add submodules**

Run:
```bash
git submodule add https://github.com/libsdl-org/SDL.git third_party/SDL
git submodule add https://github.com/recp/cglm.git third_party/cglm
git submodule add https://github.com/DanielChappuis/reactphysics3d.git third_party/reactphysics3d
git submodule add https://github.com/lsalzman/enet.git third_party/enet
git submodule update --init --recursive
```

- [ ] **Step 3: Write `scripts/setup.sh`**

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
TP="$ROOT/third_party"
PREFIX="$TP/install"

require() { command -v "$1" >/dev/null 2>&1 || { echo "MISSING TOOL: $1" >&2; exit 1; }; }
for t in git clang++ cmake python3 pkg-config; do require "$t"; done

echo "==> Fetching single-header libraries"
fetch() { # url dest
  if [ ! -f "$2" ]; then curl -fsSL "$1" -o "$2"; echo "  fetched $(basename "$2")"; fi
}
fetch https://raw.githubusercontent.com/nothings/stb/master/stb_image.h     "$TP/stb_image.h"
fetch https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h  "$TP/stb_truetype.h"
fetch https://raw.githubusercontent.com/jkuhlmann/cgltf/master/cgltf.h      "$TP/cgltf.h"
fetch https://raw.githubusercontent.com/Immediate-Mode-UI/Nuklear/master/nuklear.h "$TP/nuklear.h"

echo "==> Generating GLAD (gl core 3.3)"
if [ ! -f "$TP/glad/include/glad/gl.h" ]; then
  python3 -m pip install --quiet --user glad2 || python3 -m pip install --quiet glad2
  python3 -m glad --api gl:core=3.3 --out-path "$TP/glad" c
fi

echo "==> Initializing submodules"
git submodule update --init --recursive

build_cmake() { # srcdir [extra cmake args...]
  local src="$1"; shift
  local bld="$src/build"
  cmake -S "$src" -B "$bld" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF "$@"
  cmake --build "$bld" --config Release --parallel
  cmake --install "$bld" --config Release
}

echo "==> Building SDL3 (static)"
build_cmake "$TP/SDL" -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_TEST=OFF

echo "==> Installing cglm headers (header-only use)"
build_cmake "$TP/cglm" -DCGLM_STATIC=ON

echo "==> Building enet (static, present but unused)"
build_cmake "$TP/enet"

echo "==> Building ReactPhysics3D (static, present but unused)"
build_cmake "$TP/reactphysics3d"

echo "==> setup complete. Run 'make' to build."
```

- [ ] **Step 4: Make executable and run setup**

Run: `chmod +x scripts/setup.sh && ./scripts/setup.sh`
Expected: ends with `setup complete`; no `MISSING TOOL` errors.

- [ ] **Step 5: Verify deps present (passing check)**

Run:
```bash
test -f third_party/stb_image.h && \
test -f third_party/glad/include/glad/gl.h && \
test -f third_party/install/include/SDL3/SDL.h && \
ls third_party/install/lib/pkgconfig/sdl3.pc && echo OK
```
Expected: `OK`

- [ ] **Step 6: Commit**

```bash
git add .gitmodules scripts/setup.sh third_party/.gitkeep 2>/dev/null; git add -A
git commit -m "build: add setup.sh vendoring deps, GLAD, and submodules"
```

---

### Task 3: Makefile + build/run/clean scripts (minimal main links)

**Files:**
- Create: `scripts/build.sh`
- Create: `scripts/run.sh`
- Create: `scripts/clean.sh`
- Create: `Makefile`
- Create: `src/main.cpp` (minimal — full loop comes in Task 7)

- [ ] **Step 1: Verify no binary (failing check)**

Run: `test -f build/dungeoncrawl && echo PRESENT || echo MISSING`
Expected: `MISSING`

- [ ] **Step 2: Write minimal `src/main.cpp`**

```cpp
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    bool smoke = (argc > 1 && std::strcmp(argv[1], "--smoke") == 0);
    std::printf("dungeoncrawl: boot (smoke=%s)\n", smoke ? "true" : "false");
    return 0;
}
```

- [ ] **Step 3: Write `scripts/build.sh`**

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
TP="$ROOT/third_party"
PREFIX="$TP/install"
OUT="$ROOT/build"
OBJ="$OUT/obj"
mkdir -p "$OBJ"

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
SDL_CFLAGS="$(pkg-config --cflags sdl3)"
SDL_LIBS="$(pkg-config --static --libs sdl3)"

CXX=${CXX:-clang++}
STD="-std=c++17"
WARN="-Wall -Wextra"
INC="-I$ROOT/src -I$TP -I$TP/glad/include -I$PREFIX/include $SDL_CFLAGS"

# Compile GLAD (C) once.
if [ ! -f "$OBJ/gl.o" ]; then
  clang -c "$TP/glad/src/gl.c" -I"$TP/glad/include" -o "$OBJ/gl.o"
fi

# Compile all C++ sources.
OBJS="$OBJ/gl.o"
while IFS= read -r src; do
  obj="$OBJ/$(echo "$src" | sed 's#[/.]#_#g').o"
  "$CXX" $STD $WARN $INC -c "$src" -o "$obj"
  OBJS="$OBJS $obj"
done < <(find "$ROOT/src" -name '*.cpp')

# Link.
"$CXX" $STD $OBJS $SDL_LIBS -o "$OUT/dungeoncrawl"
echo "built: $OUT/dungeoncrawl"
```

- [ ] **Step 4: Write `scripts/run.sh`, `scripts/clean.sh`**

`scripts/run.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
./scripts/build.sh
exec ./build/dungeoncrawl ${ARGS:-}
```

`scripts/clean.sh`:
```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
rm -rf build
echo "cleaned build/"
```

- [ ] **Step 5: Write `Makefile`**

```makefile
.PHONY: all setup build run clean
all: build

setup:
	./scripts/setup.sh

build:
	./scripts/build.sh

run:
	./scripts/run.sh

clean:
	./scripts/clean.sh
```

(Note: recipe lines must be TAB-indented, not spaces.)

- [ ] **Step 6: Build and verify (passing check)**

Run: `chmod +x scripts/*.sh && make build && ./build/dungeoncrawl --smoke`
Expected: `built: .../build/dungeoncrawl` then `dungeoncrawl: boot (smoke=true)`, exit 0.

- [ ] **Step 7: Commit**

```bash
git add Makefile scripts/build.sh scripts/run.sh scripts/clean.sh src/main.cpp
git commit -m "build: add Makefile and build/run/clean scripts with minimal main"
```

---

### Task 4: Stub engine headers (including inert net/)

**Files:**
- Create: `src/engine/input/input.h`, `audio/audio.h`, `assets/assets.h`,
  `world/world.h`, `entity/entity.h`, `ui/ui.h`, `game/game.h`
- Create: `src/engine/net/udp_channel.h`, `src/engine/net/tcp_channel.h`

- [ ] **Step 1: Verify a stub header syntax-checks as absent (failing check)**

Run: `test -f src/engine/net/udp_channel.h && echo PRESENT || echo MISSING`
Expected: `MISSING`

- [ ] **Step 2: Write the seven subsystem stubs**

`src/engine/input/input.h`:
```cpp
#pragma once
// Input subsystem — TODO: map SDL events to actions.
namespace dc::input { struct Input { bool init(); void shutdown(); }; }
```
`src/engine/audio/audio.h`:
```cpp
#pragma once
// Audio subsystem — TODO: SDL audio device + mixing.
namespace dc::audio { struct Audio { bool init(); void shutdown(); }; }
```
`src/engine/assets/assets.h`:
```cpp
#pragma once
// Assets subsystem — TODO: load PNG (stb_image), glTF (cgltf), fonts (stb_truetype).
namespace dc::assets { struct Assets { bool init(); void shutdown(); }; }
```
`src/engine/world/world.h`:
```cpp
#pragma once
// World subsystem — TODO: dungeon grid, tiles, collision geometry.
namespace dc::world { struct World { void update(float dt); }; }
```
`src/engine/entity/entity.h`:
```cpp
#pragma once
// Entity subsystem — TODO: actors, components (no ECS yet, YAGNI).
namespace dc::entity { struct Entity { void update(float dt); }; }
```
`src/engine/ui/ui.h`:
```cpp
#pragma once
// UI subsystem — TODO: wrap Nuklear immediate-mode UI.
namespace dc::ui { struct Ui { bool init(); void shutdown(); }; }
```
`src/engine/game/game.h`:
```cpp
#pragma once
// Game subsystem — TODO: top-level game state, ties subsystems together.
namespace dc::game { struct Game { void update(float dt); }; }
```

- [ ] **Step 3: Write the two inert net stubs**

`src/engine/net/udp_channel.h`:
```cpp
#pragma once
#include <cstdint>
// UDP gameplay channel — TODO: enet host, (un)reliable packets. NOT wired to anything.
namespace dc::net {
struct UdpChannel {
    bool bind(uint16_t port);
    void send(const void* data, uint32_t len);
    void poll();
    void shutdown();
};
} // namespace dc::net
```
`src/engine/net/tcp_channel.h`:
```cpp
#pragma once
#include <cstdint>
#include <string>
// TCP login/lobby channel — TODO: connect, auth handshake. NOT wired to anything.
namespace dc::net {
struct TcpChannel {
    bool connect(const std::string& host, uint16_t port);
    void send(const void* data, uint32_t len);
    void shutdown();
};
} // namespace dc::net
```

- [ ] **Step 4: Verify all stubs syntax-check (passing check)**

Run:
```bash
for h in $(find src/engine -name '*.h'); do \
  clang++ -std=c++17 -Isrc -fsyntax-only -x c++ "$h" || { echo "FAIL $h"; exit 1; }; \
done; echo OK
```
Expected: `OK`

- [ ] **Step 5: Commit**

```bash
git add src/engine
git commit -m "feat: add stub engine subsystem headers and inert net channels"
```

---

### Task 5: Platform subsystem — SDL3 window + GL 3.3 context

**Files:**
- Create: `src/engine/platform/window.h`
- Create: `src/engine/platform/window.cpp`

- [ ] **Step 1: Verify absent (failing check)**

Run: `test -f src/engine/platform/window.cpp && echo PRESENT || echo MISSING`
Expected: `MISSING`

- [ ] **Step 2: Write `src/engine/platform/window.h`**

```cpp
#pragma once
#include <cstdint>

struct SDL_Window;

namespace dc::platform {

struct Window {
    SDL_Window* sdl_window = nullptr;
    void*       gl_context = nullptr; // SDL_GLContext
    int         width = 1280;
    int         height = 720;

    // Initializes SDL video, creates the window and a GL 3.3 core context,
    // and loads GL function pointers via GLAD. Returns false on failure.
    bool init(const char* title);

    // Returns false when the user requested quit (e.g. window close).
    bool pump_events();

    void swap();
    void shutdown();
};

} // namespace dc::platform
```

- [ ] **Step 3: Write `src/engine/platform/window.cpp`**

```cpp
#include "engine/platform/window.h"

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

    sdl_window = SDL_CreateWindow(title, width, height, SDL_WINDOW_OPENGL);
    if (!sdl_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    gl_context = SDL_GL_CreateContext(sdl_window);
    if (!gl_context) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(sdl_window, static_cast<SDL_GLContext>(gl_context));

    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(SDL_GL_GetProcAddress))) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        return false;
    }
    std::printf("GL %s\n", glGetString(GL_VERSION));
    glViewport(0, 0, width, height);
    return true;
}

bool Window::pump_events() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_EVENT_QUIT) return false;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) return false;
    }
    return true;
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

- [ ] **Step 4: Build to verify it compiles and links (passing check)**

Run: `make build`
Expected: `built: .../build/dungeoncrawl` (main.cpp doesn't use Window yet; this confirms the platform TU compiles and links against SDL3).

- [ ] **Step 5: Commit**

```bash
git add src/engine/platform/window.h src/engine/platform/window.cpp
git commit -m "feat(platform): SDL3 window + GL 3.3 core context with GLAD loading"
```

---

### Task 6: Renderer — shader loading + triangle

**Files:**
- Create: `assets/shaders/tri.vert`
- Create: `assets/shaders/tri.frag`
- Create: `src/engine/renderer/shader.h`
- Create: `src/engine/renderer/shader.cpp`
- Create: `src/engine/renderer/renderer.h`
- Create: `src/engine/renderer/renderer.cpp`

- [ ] **Step 1: Verify absent (failing check)**

Run: `test -f src/engine/renderer/renderer.cpp && echo PRESENT || echo MISSING`
Expected: `MISSING`

- [ ] **Step 2: Write the shaders**

`assets/shaders/tri.vert`:
```glsl
#version 330 core
layout (location = 0) in vec2 a_pos;
layout (location = 1) in vec3 a_color;
out vec3 v_color;
void main() {
    v_color = a_color;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
```
`assets/shaders/tri.frag`:
```glsl
#version 330 core
in vec3 v_color;
out vec4 frag_color;
void main() { frag_color = vec4(v_color, 1.0); }
```

- [ ] **Step 3: Write `src/engine/renderer/shader.h`**

```cpp
#pragma once
#include <cstdint>

namespace dc::renderer {

// Compiles and links a program from two GLSL source files. Returns 0 on failure.
uint32_t load_program(const char* vert_path, const char* frag_path);

} // namespace dc::renderer
```

- [ ] **Step 4: Write `src/engine/renderer/shader.cpp`**

```cpp
#include "engine/renderer/shader.h"

#include <glad/gl.h>
#include <cstdio>
#include <string>
#include <fstream>
#include <sstream>

namespace dc::renderer {

static std::string read_file(const char* path) {
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "shader: cannot open %s\n", path); return {}; }
    std::stringstream ss; ss << f.rdbuf(); return ss.str();
}

static uint32_t compile(GLenum type, const std::string& src) {
    uint32_t s = glCreateShader(type);
    const char* c = src.c_str();
    glShaderSource(s, 1, &c, nullptr);
    glCompileShader(s);
    int ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::fprintf(stderr, "shader compile error: %s\n", log);
        glDeleteShader(s); return 0;
    }
    return s;
}

uint32_t load_program(const char* vert_path, const char* frag_path) {
    std::string vsrc = read_file(vert_path);
    std::string fsrc = read_file(frag_path);
    if (vsrc.empty() || fsrc.empty()) return 0;
    uint32_t vs = compile(GL_VERTEX_SHADER, vsrc);
    uint32_t fs = compile(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) return 0;
    uint32_t prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    int ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024]; glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::fprintf(stderr, "program link error: %s\n", log);
        glDeleteProgram(prog); prog = 0;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

} // namespace dc::renderer
```

- [ ] **Step 5: Write `src/engine/renderer/renderer.h`**

```cpp
#pragma once
#include <cstdint>

namespace dc::renderer {

struct Renderer {
    uint32_t program = 0;
    uint32_t vao = 0;
    uint32_t vbo = 0;

    // Loads the triangle program and uploads geometry. Returns false on failure.
    bool init();
    void render();
    void shutdown();
};

} // namespace dc::renderer
```

- [ ] **Step 6: Write `src/engine/renderer/renderer.cpp`**

```cpp
#include "engine/renderer/renderer.h"
#include "engine/renderer/shader.h"

#include <glad/gl.h>

namespace dc::renderer {

bool Renderer::init() {
    program = load_program("assets/shaders/tri.vert", "assets/shaders/tri.frag");
    if (!program) return false;

    // pos.xy, color.rgb
    const float verts[] = {
        -0.6f, -0.5f,  1.0f, 0.0f, 0.0f,
         0.6f, -0.5f,  0.0f, 1.0f, 0.0f,
         0.0f,  0.6f,  0.0f, 0.0f, 1.0f,
    };
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    return true;
}

void Renderer::render() {
    glClearColor(0.08f, 0.08f, 0.12f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
}

void Renderer::shutdown() {
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (program) glDeleteProgram(program);
    program = vao = vbo = 0;
}

} // namespace dc::renderer
```

- [ ] **Step 7: Build to verify compile + link (passing check)**

Run: `make build`
Expected: `built: .../build/dungeoncrawl` (still not used by main; confirms renderer TUs build).

- [ ] **Step 8: Commit**

```bash
git add assets/shaders src/engine/renderer
git commit -m "feat(renderer): GLSL program loader and triangle geometry"
```

---

### Task 7: Wire the core loop in main.cpp — the milestone

**Files:**
- Modify: `src/main.cpp`

- [ ] **Step 1: Confirm current main is the minimal stub (failing check for milestone)**

Run: `./build/dungeoncrawl --smoke`
Expected: prints `dungeoncrawl: boot (smoke=true)` and exits — i.e. NO window, NO `GL ...` line yet. This is the "before" state.

- [ ] **Step 2: Rewrite `src/main.cpp` to run the loop**

```cpp
#include "engine/platform/window.h"
#include "engine/renderer/renderer.h"

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
    bool smoke = (argc > 1 && std::strcmp(argv[1], "--smoke") == 0);

    dc::platform::Window window;
    if (!window.init("dungeoncrawl")) return 1;

    dc::renderer::Renderer renderer;
    if (!renderer.init()) { window.shutdown(); return 1; }

    bool running = true;
    uint64_t prev = SDL_GetTicksNS();
    while (running) {
        // process_input
        running = window.pump_events();

        // update(dt)
        uint64_t now = SDL_GetTicksNS();
        float dt = static_cast<float>(now - prev) / 1.0e9f;
        prev = now;
        (void)dt; // subsystems are stubs for now

        // render
        renderer.render();
        window.swap();

        if (smoke) { std::printf("smoke: one frame rendered, exiting\n"); break; }
    }

    renderer.shutdown();
    window.shutdown();
    return 0;
}
```

- [ ] **Step 3: Build (passing check, part 1)**

Run: `make build`
Expected: `built: .../build/dungeoncrawl`.

- [ ] **Step 4: Headless smoke run (passing check, part 2)**

Run: `./build/dungeoncrawl --smoke`
Expected: a `GL 3.3 ...` line, then `smoke: one frame rendered, exiting`, exit code 0.
Confirm: `echo $?` prints `0`.

- [ ] **Step 5: Interactive visual confirmation**

Run: `make run`
Expected: a 1280×720 window opens showing a triangle (red/green/blue corners) on a
dark blue-grey background. Pressing Escape or clicking close exits cleanly with no
errors printed.

- [ ] **Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "feat: wire platform + renderer into core loop (hello triangle milestone)"
```

---

## Self-Review

**Spec coverage:**
- Directory layout → Task 1 ✓
- Vendored single-header deps, GLAD generation, submodules built static → Task 2 ✓
- Thin Makefile → scripts (setup/build/run/clean) → Task 3 ✓
- C++17 / clang++ → Task 3 build.sh ✓
- Stub subsystems incl. inert net/ (udp+tcp) → Task 4 ✓
- SDL3 window + GL 3.3 core context via GLAD → Task 5 ✓
- stb_truetype over FreeType → Task 2 fetches stb_truetype, no FreeType submodule ✓
- Hello triangle through process_input/update/render loop → Tasks 6–7 ✓
- Clean shutdown on quit → Task 5 pump_events + Task 7 shutdown ✓

**Placeholder scan:** No TBD/TODO-as-work placeholders in steps; `// TODO` comments in
stub headers are intentional product content, not plan gaps. All code steps show full code.

**Type consistency:** `dc::platform::Window` (`init`, `pump_events`, `swap`, `shutdown`),
`dc::renderer::Renderer` (`init`, `render`, `shutdown`), and `dc::renderer::load_program`
are used in main.cpp (Task 7) exactly as declared in Tasks 5–6. `--smoke` flag introduced
in Task 3 and reused consistently in Task 7.

**Known platform assumption:** build.sh links via the vendored `sdl3.pc` (macOS). Cross-platform
linking is explicitly out of scope per the spec.
