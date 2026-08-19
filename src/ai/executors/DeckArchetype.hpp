#pragma once

#include <cstdint>
#include <vector>

#include "../Executor.hpp"

// Picks which deck-specific executor table (if any) matches a given deck,
// by checking for that archetype's signature cards in the deck's own main
// deck — the same way a real player would recognize their own deck's
// strategy. This only ever selects among the small, explicitly-implemented
// set of archetypes below; anything else gets the generic table.

namespace goat::ai {

enum class DeckArchetype {
    Generic,
    ChaosControl,
    Gearfried,
    Burn,
};

DeckArchetype detect_deck_archetype(const std::vector<uint32_t>& main_deck_codes);

// Convenience: detect_deck_archetype(codes) followed by the matching
// build_*_executors() call.
ExecutorList build_executors_for_deck(const std::vector<uint32_t>& main_deck_codes);

} // namespace goat::ai
