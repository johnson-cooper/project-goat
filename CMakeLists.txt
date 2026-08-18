cmake_minimum_required(VERSION 3.20)
project(goat_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB LUA_SOURCES CONFIGURE_DEPENDS external/ygopro-core/lua/src/*.c)
list(REMOVE_ITEM LUA_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/external/ygopro-core/lua/src/lbitlib.c
  ${CMAKE_CURRENT_SOURCE_DIR}/external/ygopro-core/lua/src/lcorolib.c
  ${CMAKE_CURRENT_SOURCE_DIR}/external/ygopro-core/lua/src/ldblib.c
  ${CMAKE_CURRENT_SOURCE_DIR}/external/ygopro-core/lua/src/linit.c
  ${CMAKE_CURRENT_SOURCE_DIR}/external/ygopro-core/lua/src/loadlib.c
  ${CMAKE_CURRENT_SOURCE_DIR}/external/ygopro-core/lua/src/loslib.c
  ${CMAKE_CURRENT_SOURCE_DIR}/external/ygopro-core/lua/src/ltests.c
  ${CMAKE_CURRENT_SOURCE_DIR}/external/ygopro-core/lua/src/lua.c
  ${CMAKE_CURRENT_SOURCE_DIR}/external/ygopro-core/lua/src/luac.c
  ${CMAKE_CURRENT_SOURCE_DIR}/external/ygopro-core/lua/src/lutf8lib.c
  ${CMAKE_CURRENT_SOURCE_DIR}/external/ygopro-core/lua/src/onelua.c)

add_library(ygopro_lua STATIC ${LUA_SOURCES})
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
target_include_directories(ygopro_lua PUBLIC external/ygopro-core/lua external/ygopro-core/lua/src)
target_compile_options(ygopro_lua PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-include${CMAKE_CURRENT_SOURCE_DIR}/external/ygopro-core/lua/luaconf-customize.h>)

set(OCGCORE_SOURCES
  external/ygopro-core/card.cpp external/ygopro-core/duel.cpp
  external/ygopro-core/effect.cpp external/ygopro-core/field.cpp
  external/ygopro-core/interpreter.cpp
  external/ygopro-core/libcard.cpp external/ygopro-core/libdebug.cpp
  external/ygopro-core/libduel.cpp external/ygopro-core/libeffect.cpp
  external/ygopro-core/libgroup.cpp external/ygopro-core/ocgapi.cpp
  external/ygopro-core/operations.cpp external/ygopro-core/playerop.cpp
  external/ygopro-core/processor.cpp external/ygopro-core/processor_visit.cpp
  external/ygopro-core/scriptlib.cpp)

add_library(ocgcore STATIC ${OCGCORE_SOURCES})
target_include_directories(ocgcore PUBLIC external/ygopro-core external/ygopro-core/lua/src)
target_link_libraries(ocgcore PUBLIC ygopro_lua)

add_executable(goat-sim src/main.cpp src/cards/CardDatabase.cpp src/deck/Banlist.cpp)
target_link_libraries(goat-sim PRIVATE ocgcore sqlite3)

if(WIN32)
  add_executable(goat-client WIN32 src/client/main.cpp src/game/Progression.cpp src/game/Catalog.cpp src/game/DeckBuilder.cpp src/cards/CardDatabase.cpp src/deck/Banlist.cpp)
  target_include_directories(goat-client PRIVATE src)
  target_link_libraries(goat-client PRIVATE ole32 windowscodecs gdi32 sqlite3)
endif()

enable_testing()
if(WIN32)
  add_test(NAME player_ipc COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/scripts/test-player-ipc.ps1)
  set_tests_properties(player_ipc PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
add_test(NAME smoke COMMAND goat-sim duel ${CMAKE_SOURCE_DIR}/decks/vanilla-a.ydk ${CMAKE_SOURCE_DIR}/decks/vanilla-b.ydk --seed 12345 --max-turns 100)
set_tests_properties(smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(card-database-test tests/card_database_test.cpp src/cards/CardDatabase.cpp)
target_include_directories(card-database-test PRIVATE src)
target_link_libraries(card-database-test PRIVATE sqlite3)
add_test(NAME card_database COMMAND card-database-test)
set_tests_properties(card_database PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(banlist-test tests/banlist_test.cpp src/deck/Banlist.cpp)
target_include_directories(banlist-test PRIVATE src)
add_test(NAME banlist COMMAND banlist-test)
set_tests_properties(banlist PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(starter-decks-test tests/starter_decks_test.cpp src/game/Catalog.cpp src/game/DeckBuilder.cpp src/cards/CardDatabase.cpp src/deck/Banlist.cpp)
target_include_directories(starter-decks-test PRIVATE src)
target_link_libraries(starter-decks-test PRIVATE sqlite3)
add_test(NAME starter_decks COMMAND starter-decks-test)
set_tests_properties(starter_decks PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(progression-test tests/progression_test.cpp src/game/Progression.cpp)
target_include_directories(progression-test PRIVATE src)
add_test(NAME progression COMMAND progression-test)

add_executable(catalog-test tests/catalog_test.cpp src/game/Catalog.cpp src/game/Progression.cpp)
target_include_directories(catalog-test PRIVATE src)
add_test(NAME catalog COMMAND catalog-test)
set_tests_properties(catalog PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(catalog-images-test tests/catalog_images_test.cpp src/game/Catalog.cpp src/game/Progression.cpp)
target_include_directories(catalog-images-test PRIVATE src)
add_test(NAME catalog_images COMMAND catalog-images-test)
set_tests_properties(catalog_images PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(deck-builder-test tests/deck_builder_test.cpp src/game/DeckBuilder.cpp src/game/Progression.cpp src/cards/CardDatabase.cpp src/deck/Banlist.cpp)
target_include_directories(deck-builder-test PRIVATE src)
target_link_libraries(deck-builder-test PRIVATE sqlite3)
add_test(NAME deck_builder COMMAND deck-builder-test)
set_tests_properties(deck_builder PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(starter-images-test tests/starter_images_test.cpp src/game/Catalog.cpp src/game/DeckBuilder.cpp src/game/Progression.cpp src/cards/CardDatabase.cpp src/deck/Banlist.cpp)
target_include_directories(starter-images-test PRIVATE src)
target_link_libraries(starter-images-test PRIVATE sqlite3)
add_test(NAME starter_images COMMAND starter-images-test)
set_tests_properties(starter_images PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
