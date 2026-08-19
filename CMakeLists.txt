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

set(AI_SOURCES
  src/ai/DecisionRequest.cpp src/ai/DecisionResponse.cpp src/ai/ObservationBuilder.cpp
  src/ai/AnnounceCardSolver.cpp src/ai/RandomAgent.cpp src/ai/Heuristics.cpp src/ai/CardEvaluator.cpp
  src/ai/BattleEvaluator.cpp src/ai/CardSelector.cpp src/ai/Executor.cpp
  src/ai/executors/GenericGoatExecutors.cpp src/ai/executors/ChaosControlExecutor.cpp
  src/ai/executors/GearfriedExecutor.cpp src/ai/executors/BurnExecutor.cpp
  src/ai/executors/DeckArchetype.cpp src/ai/GoatAgent.cpp)

add_executable(goat-sim src/main.cpp src/cards/CardDatabase.cpp src/deck/Banlist.cpp ${AI_SOURCES})
target_link_libraries(goat-sim PRIVATE ocgcore sqlite3)

if(WIN32)
  add_executable(goat-client WIN32 src/client/main.cpp src/game/Progression.cpp src/game/Catalog.cpp src/game/DeckBuilder.cpp src/cards/CardDatabase.cpp src/deck/Banlist.cpp)
  target_include_directories(goat-client PRIVATE src)
  target_link_libraries(goat-client PRIVATE ole32 windowscodecs gdi32 sqlite3)
endif()

# Cross-platform client (Windows/macOS/Linux), built on raylib instead of
# Win32 GDI/WIC. Vendored like every other external/ dependency — see
# scripts/update-dependencies.ps1 — rather than fetched separately via
# FetchContent, so there's one consistent way this project acquires
# third-party source instead of two.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/raylib/CMakeLists.txt)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  add_subdirectory(external/raylib)
  # SUPPORT_FILEFORMAT_JPG is disabled in raylib's default config.h (only PNG
  # is on by default) — this project's card/pack art is all .jpg, so without
  # this every LoadImage() on them silently fails and nothing but the title
  # screen's .png background ever renders. Matches scripts/build-client-rl.sh's
  # CUSTOM_CFLAGS override for the same reason, applied here at the CMake
  # target level since that script's ad hoc Makefile invocation isn't used
  # when building through CMake.
  target_compile_definitions(raylib PRIVATE SUPPORT_FILEFORMAT_JPG=1)

  add_executable(goat-client-rl src/client_rl/main.cpp src/client_rl/ProcessBridge.cpp src/client_rl/AudioManager.cpp src/game/Progression.cpp src/game/Catalog.cpp src/game/DeckBuilder.cpp src/cards/CardDatabase.cpp src/deck/Banlist.cpp)
  target_include_directories(goat-client-rl PRIVATE src)
  target_link_libraries(goat-client-rl PRIVATE raylib sqlite3)
endif()

enable_testing()
if(WIN32)
  add_test(NAME player_ipc COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File ${CMAKE_SOURCE_DIR}/scripts/test-player-ipc.ps1)
  set_tests_properties(player_ipc PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
add_test(NAME smoke COMMAND goat-sim duel ${CMAKE_SOURCE_DIR}/decks/vanilla-a.ydk ${CMAKE_SOURCE_DIR}/decks/vanilla-b.ydk --seed 12345 --max-turns 100)
set_tests_properties(smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# CPU vs CPU: the "goat" agent driving both seats across several
# deck/seed combinations, asserting each duel completes (exit 0) rather than
# hanging or hitting an unsupported/illegal decision. Mirrors `smoke`'s own
# pattern of testing goat-sim as a black box, no extra harness needed.
add_test(NAME ai_cpu_vs_cpu_goat_vs_goat_1 COMMAND goat-sim duel
  ${CMAKE_SOURCE_DIR}/decks/starter/flc1-1st-goat.ydk ${CMAKE_SOURCE_DIR}/decks/starter/flc3-1st-burn.ydk
  --agent1 goat --agent2 goat --seed 4242 --max-turns 150 --quiet)
set_tests_properties(ai_cpu_vs_cpu_goat_vs_goat_1 PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ai_cpu_vs_cpu_goat_vs_goat_2 COMMAND goat-sim duel
  ${CMAKE_SOURCE_DIR}/decks/starter/flc1-2nd-gearfried.ydk ${CMAKE_SOURCE_DIR}/decks/starter/flc2-1st-chaos-tur.ydk
  --agent1 goat --agent2 goat --seed 77 --max-turns 150 --quiet)
set_tests_properties(ai_cpu_vs_cpu_goat_vs_goat_2 PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ai_cpu_vs_cpu_goat_vs_goat_3 COMMAND goat-sim duel
  ${CMAKE_SOURCE_DIR}/decks/starter/water-fusion.ydk ${CMAKE_SOURCE_DIR}/decks/starter/flc3-3rd-goat.ydk
  --agent1 goat --agent2 goat --seed 909090 --max-turns 150 --quiet)
set_tests_properties(ai_cpu_vs_cpu_goat_vs_goat_3 PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
add_test(NAME ai_cpu_vs_cpu_random_vs_goat COMMAND goat-sim duel
  ${CMAKE_SOURCE_DIR}/decks/starter/flc2-3rd-goat.ydk ${CMAKE_SOURCE_DIR}/decks/starter/flc3-2nd-chaos-con.ydk
  --agent1 random --agent2 goat --seed 31337 --max-turns 150 --quiet)
set_tests_properties(ai_cpu_vs_cpu_random_vs_goat PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

# Determinism: the exact same --seed/--ai-seed pair run twice must produce an
# identical result and an identical full engine log, proving the "goat" agent
# introduces no hidden nondeterminism (unseeded RNG, container-iteration-order
# dependence, etc.) beyond the engine's own already-deterministic seeding.
add_test(NAME ai_deterministic COMMAND ${CMAKE_COMMAND}
  -DGOAT_SIM_EXECUTABLE=$<TARGET_FILE:goat-sim> -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DBINARY_DIR=${CMAKE_BINARY_DIR}
  -P ${CMAKE_SOURCE_DIR}/tests/ai_deterministic_test.cmake)

add_executable(card-database-test tests/card_database_test.cpp src/cards/CardDatabase.cpp)
target_include_directories(card-database-test PRIVATE src)
target_link_libraries(card-database-test PRIVATE sqlite3)
add_test(NAME card_database COMMAND card-database-test)
set_tests_properties(card_database PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(ai-decision-request-test tests/ai_decision_request_test.cpp src/ai/DecisionRequest.cpp)
target_include_directories(ai-decision-request-test PRIVATE src)
target_link_libraries(ai-decision-request-test PRIVATE ocgcore)
add_test(NAME ai_decision_request COMMAND ai-decision-request-test)
set_tests_properties(ai_decision_request PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(ai-observation-test tests/ai_observation_test.cpp src/ai/ObservationBuilder.cpp)
target_include_directories(ai-observation-test PRIVATE src)
target_link_libraries(ai-observation-test PRIVATE ocgcore)
add_test(NAME ai_observation COMMAND ai-observation-test)
set_tests_properties(ai_observation PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(ai-executor-test tests/ai_executor_test.cpp src/cards/CardDatabase.cpp ${AI_SOURCES})
target_include_directories(ai-executor-test PRIVATE src)
target_link_libraries(ai-executor-test PRIVATE ocgcore sqlite3)
add_test(NAME ai_executor COMMAND ai-executor-test)
set_tests_properties(ai_executor PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(ai-staple-test tests/ai_staple_test.cpp src/cards/CardDatabase.cpp ${AI_SOURCES})
target_include_directories(ai-staple-test PRIVATE src)
target_link_libraries(ai-staple-test PRIVATE ocgcore sqlite3)
add_test(NAME ai_staple COMMAND ai-staple-test)
set_tests_properties(ai_staple PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(ai-archetype-test tests/ai_archetype_test.cpp src/cards/CardDatabase.cpp ${AI_SOURCES})
target_include_directories(ai-archetype-test PRIVATE src)
target_link_libraries(ai-archetype-test PRIVATE ocgcore sqlite3)
add_test(NAME ai_archetype COMMAND ai-archetype-test)
set_tests_properties(ai_archetype PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

add_executable(ai-announce-card-test tests/ai_announce_card_test.cpp src/cards/CardDatabase.cpp src/ai/DecisionRequest.cpp src/ai/AnnounceCardSolver.cpp)
target_include_directories(ai-announce-card-test PRIVATE src)
target_link_libraries(ai-announce-card-test PRIVATE ocgcore sqlite3)
add_test(NAME ai_announce_card COMMAND ai-announce-card-test)
set_tests_properties(ai_announce_card PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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
set_tests_properties(progression PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})

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
