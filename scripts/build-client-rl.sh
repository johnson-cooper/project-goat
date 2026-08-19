#!/usr/bin/env bash
# Builds the cross-platform client (goat-client-rl, raylib-based) and its
# raylib dependency from source. Mirrors build-smoke.sh's ad hoc g++
# approach (bypassing CMake) for the same reason: see that script and
# README.md for why CMake isn't reliably available in every dev setup here.
#
# Toolchain note (Windows): this must be run with a *genuine* MinGW-w64 GCC
# on PATH (one that reports `-dumpmachine` as x86_64-w64-mingw32 and defines
# _WIN32), not a Cygwin-hosted GCC (x86_64-pc-cygwin, which only defines
# __CYGWIN__). raylib's GLFW backend gates its actual Win32 implementation
# behind _WIN32 specifically; under a Cygwin-hosted compiler those files
# compile as empty stubs and linking fails with dozens of undefined
# `_glfwPlatform*` references. If your MSYS2 install's default gcc reports
# x86_64-pc-cygwin, look for a separate mingw64 environment (e.g. run this
# via that install's own `usr/bin/bash.exe --login`, which puts
# /mingw64/bin on PATH ahead of everything else).
set -euo pipefail
cd "$(dirname "$0")/.."

if [ ! -f external/raylib/CMakeLists.txt ]; then
  echo "external/raylib is missing — run scripts/update-dependencies.ps1 first." >&2
  exit 1
fi

if [ "$(g++ -dumpmachine)" != "x86_64-w64-mingw32" ]; then
  echo "warning: g++ -dumpmachine reports $(g++ -dumpmachine), not x86_64-w64-mingw32 — see the toolchain note at the top of this script if the raylib build fails with undefined _glfwPlatform* references." >&2
fi

# SUPPORT_FILEFORMAT_JPG is disabled in raylib's default config.h (only PNG
# is on by default) — this project's card/pack art is all .jpg
# (external/card_images, external/packart, external/extra/card_back.jpg), so
# without this override LoadImage() silently fails on every one of them (a
# "IMAGE: Data format not supported" warning, no crash) and nothing but the
# title screen's .png background ever renders.
if [ ! -f external/raylib/src/libraylib.a ]; then
  make -C external/raylib/src PLATFORM=PLATFORM_DESKTOP CUSTOM_CFLAGS="-DSUPPORT_FILEFORMAT_JPG=1" -j"$(nproc 2>/dev/null || echo 4)"
fi

g++ -std=c++17 -O0 -Isrc/client_rl -Iexternal/raylib/src -Isrc \
  src/client_rl/main.cpp src/client_rl/ProcessBridge.cpp src/client_rl/AudioManager.cpp \
  src/game/Progression.cpp src/game/Catalog.cpp src/game/DeckBuilder.cpp \
  src/cards/CardDatabase.cpp src/deck/Banlist.cpp \
  external/raylib/src/libraylib.a -lsqlite3 -lopengl32 -lgdi32 -lwinmm \
  -o build/goat-client-rl.exe

# Runtime DLLs goat-client-rl needs beside it (same idea as build-smoke.sh
# does for goat-client.exe, just resolved from wherever this g++ lives).
compiler_bin="$(dirname "$(command -v g++)")"
for dll in libgcc_s_seh-1.dll libwinpthread-1.dll libsqlite3-0.dll libstdc++-6.dll; do
  [ -f "$compiler_bin/$dll" ] && cp -f "$compiler_bin/$dll" build/
done

echo "Built build/goat-client-rl.exe"
