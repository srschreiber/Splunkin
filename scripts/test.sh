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
build_and_run() { # test_file extra_srcs...
  local tf="$1"; shift
  local name; name="$(basename "$tf" .cpp)"
  local bin="$OUT/$name"
  "$CXX" $STD $INC "$tf" "$@" -o "$bin"
  "$bin"
}

build_and_run "$ROOT/tests/sanity_test.cpp"

build_and_run "$ROOT/tests/map_test.cpp" "$ROOT/src/engine/world/map.cpp"

build_and_run "$ROOT/tests/map_mesh_test.cpp" \
  "$ROOT/src/engine/world/map_mesh.cpp" "$ROOT/src/engine/world/map.cpp"

build_and_run "$ROOT/tests/camera_test.cpp" \
  "$ROOT/src/engine/renderer/camera.cpp" "$ROOT/src/engine/entity/player.cpp" \
  "$ROOT/src/engine/world/collision.cpp" "$ROOT/src/engine/world/map.cpp"

build_and_run "$ROOT/tests/collision_test.cpp" \
  "$ROOT/src/engine/world/collision.cpp" "$ROOT/src/engine/world/map.cpp"

build_and_run "$ROOT/tests/player_test.cpp" \
  "$ROOT/src/engine/entity/player.cpp" "$ROOT/src/engine/world/collision.cpp" \
  "$ROOT/src/engine/world/map.cpp"

build_and_run "$ROOT/tests/texture_decode_test.cpp" "$ROOT/src/engine/renderer/stb_image_impl.cpp"

build_and_run "$ROOT/tests/model_load_test.cpp" \
  "$ROOT/src/engine/renderer/model.cpp" "$ROOT/src/engine/renderer/cgltf_impl.cpp"

echo "all tests passed"
