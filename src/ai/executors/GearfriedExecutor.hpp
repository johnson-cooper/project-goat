#pragma once

#include "../Executor.hpp"

// Deck-specific executor for the Gearfried Warrior-beatdown family
// (decks/starter/flc1-2nd-gearfried.ydk) — a generic-toolbox Warrior deck
// built around Reinforcement of the Army's search flexibility rather than a
// single combo piece, so there's less unique sequencing logic than Chaos
// Control; this mainly prioritizes the deck's card-advantage search engine
// and its dedicated removal monster ahead of the generic table.

namespace goat::ai {

ExecutorList build_gearfried_executors();

} // namespace goat::ai
