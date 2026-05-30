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
