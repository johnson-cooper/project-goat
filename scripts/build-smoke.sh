#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
lua_sources=()
for source in external/ygopro-core/lua/src/*.c; do
  case "$source" in
    *lbitlib.c|*lcorolib.c|*ldblib.c|*linit.c|*loadlib.c|*loslib.c|*ltests.c|*lua.c|*luac.c|*lutf8lib.c|*onelua.c) ;;
    *) lua_sources+=("$source") ;;
  esac
done
g++ -std=c++17 -O0 -Iexternal/ygopro-core -Iexternal/ygopro-core/lua -Iexternal/ygopro-core/lua/src \
  -include external/ygopro-core/lua/luaconf-customize.h \
  src/main.cpp \
  src/cards/CardDatabase.cpp \
  src/deck/Banlist.cpp \
  external/ygopro-core/{card,duel,effect,field,interpreter,libcard,libdebug,libduel,libeffect,libgroup,ocgapi,operations,playerop,processor,processor_visit,scriptlib}.cpp \
  "${lua_sources[@]}" -lsqlite3 -o build/goat-sim.exe
g++ -std=c++17 -O0 -Isrc -mwindows src/client/main.cpp src/game/Progression.cpp src/game/Catalog.cpp src/game/DeckBuilder.cpp src/cards/CardDatabase.cpp src/deck/Banlist.cpp -lsqlite3 -lole32 -lwindowscodecs -lgdi32 -o build/goat-client.exe
./build/goat-sim.exe duel decks/vanilla-a.ydk decks/vanilla-b.ydk --seed 12345 --max-turns 100
