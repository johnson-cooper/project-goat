#pragma once

#include "../Executor.hpp"

// Deck-specific executor for the Chaos Control / Chaos Turbo family — the
// dominant archetype across this project's NPC roster (present at every
// tier: decks/starter/flc1-3rd-chaos-con.ydk, flc2-3rd-chaos-con.ydk,
// flc3-2nd-chaos-con.ydk, flc2-1st-chaos-tur.ydk). Both variants share the
// same core engine (banish LIGHT+DARK monsters from the graveyard to
// Special Summon Black Luster Soldier - Envoy of the Beginning / Chaos
// Sorcerer), so one executor table covers both — see
// executors/DeckArchetype.cpp for the detection that picks it.

namespace goat::ai {

ExecutorList build_chaos_control_executors();

} // namespace goat::ai
