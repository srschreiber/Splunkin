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
  # Try installing glad2; handle externally-managed-environment on macOS
  python3 -m pip install --quiet --user glad2 \
    || python3 -m pip install --quiet glad2 \
    || python3 -m pip install --quiet --break-system-packages glad2
  # Try module invocation first, fall back to console script
  if python3 -m glad --api gl:core=3.3 --out-path "$TP/glad" c 2>/dev/null; then
    echo "  generated via python3 -m glad"
  else
    glad --api gl:core=3.3 --out-path "$TP/glad" c
    echo "  generated via glad console script"
  fi
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
