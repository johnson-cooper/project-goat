# Runs goat-sim twice with an identical --seed/--ai-seed pair (both seats on
# the "goat" agent) and asserts the result file and full engine log are
# byte-for-byte identical — proving CPU decisions don't introduce hidden
# nondeterminism beyond the engine's own already-deterministic seeding.
# Invoked via `cmake -P` from CMakeLists.txt's `ai_deterministic` test so this
# stays a pure black-box check of the goat-sim binary, matching every other
# test in this project that exercises it directly.

set(DECK_A "${SOURCE_DIR}/decks/starter/flc1-1st-goat.ydk")
set(DECK_B "${SOURCE_DIR}/decks/starter/flc3-1st-burn.ydk")
set(RESULT_A "${BINARY_DIR}/ai_determinism_result_a.txt")
set(RESULT_B "${BINARY_DIR}/ai_determinism_result_b.txt")

execute_process(
  COMMAND "${GOAT_SIM_EXECUTABLE}" duel "${DECK_A}" "${DECK_B}"
          --agent1 goat --agent2 goat --seed 4242 --ai-seed 99 --max-turns 150
          --result-file "${RESULT_A}"
  WORKING_DIRECTORY "${SOURCE_DIR}"
  OUTPUT_VARIABLE LOG_A
  RESULT_VARIABLE STATUS_A)
if(NOT STATUS_A EQUAL 0)
  message(FATAL_ERROR "first deterministic run failed with exit code ${STATUS_A}")
endif()

execute_process(
  COMMAND "${GOAT_SIM_EXECUTABLE}" duel "${DECK_A}" "${DECK_B}"
          --agent1 goat --agent2 goat --seed 4242 --ai-seed 99 --max-turns 150
          --result-file "${RESULT_B}"
  WORKING_DIRECTORY "${SOURCE_DIR}"
  OUTPUT_VARIABLE LOG_B
  RESULT_VARIABLE STATUS_B)
if(NOT STATUS_B EQUAL 0)
  message(FATAL_ERROR "second deterministic run failed with exit code ${STATUS_B}")
endif()

if(NOT LOG_A STREQUAL LOG_B)
  message(FATAL_ERROR "non-deterministic engine log between two identical-seed runs")
endif()

file(READ "${RESULT_A}" CONTENT_A)
file(READ "${RESULT_B}" CONTENT_B)
if(NOT CONTENT_A STREQUAL CONTENT_B)
  message(FATAL_ERROR "non-deterministic result: '${CONTENT_A}' vs '${CONTENT_B}'")
endif()

message(STATUS "ai_deterministic: identical log and result across two runs")
