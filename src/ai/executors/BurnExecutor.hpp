#pragma once

#include "../Executor.hpp"

// Deck-specific executor for the Burn/stall family
// (decks/starter/flc3-1st-burn.ydk) — wins via direct damage (Just
// Desserts, Secret Barrel, Ceasefire, Magic Cylinder, Nightmare Wheel) and
// board lockdown (Ojama Trio, Swords/Wall of Revealing Light) rather than
// combat; the deck runs almost no real attackers, so this mainly makes sure
// its damage/stall staples fire ahead of the generic table rather than
// relying on generic combat-oriented defaults.

namespace goat::ai {

ExecutorList build_burn_executors();

} // namespace goat::ai
