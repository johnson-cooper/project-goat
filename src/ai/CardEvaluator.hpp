#pragma once

#include <cstdint>

#include "../cards/CardDatabase.hpp"

// Generic value scoring for a single, already-known card. Used only to rank
// candidates an engine prompt has already offered — never to decide whether
// a card is a legal choice in the first place, and never applied to a
// face-down/unknown card (callers must check that separately).

namespace goat::ai {

// Effective battle-relevant power: ATK if the position is (or would be) an
// attack position, DEF otherwise. `position` uses the engine's POS_* bits;
// pass 0 (or any face-up-attack default) for cards not yet placed on the
// field (e.g. a hand card being considered for a Normal Summon).
int32_t effective_power(const goat::CardDefinition& card, uint8_t position);
int32_t effective_power(goat::CardDatabase& database, uint32_t code, uint8_t position);

// A rough generic "how valuable is this card" score, higher is more
// valuable/dangerous. Intentionally simple: level/ATK/DEF plus a bonus for
// being an Effect Monster (more likely to matter beyond raw stats) and a
// bonus for board-affecting Spell/Trap types (Continuous/Equip/Field), since
// those usually represent a bigger swing than a plain Normal Spell. This is
// a coarse generic fallback — card-specific executors in
// executors/GenericGoatExecutors.cpp should override it wherever a staple's
// real value isn't captured by generic stats.
int32_t generic_value(goat::CardDatabase& database, uint32_t code);

bool is_monster(const goat::CardDefinition& card);
bool is_spell(const goat::CardDefinition& card);
bool is_trap(const goat::CardDefinition& card);
// "Floodgate/continuous-effect" tier used by the problematic-card triage:
// Continuous/Equip/Field Spells or Traps, and Continuous-effect monsters,
// tend to keep affecting the board turn after turn rather than being a
// one-shot effect.
bool is_persistent_threat(const goat::CardDefinition& card);

} // namespace goat::ai
