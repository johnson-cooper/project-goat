#pragma once

#include <optional>
#include <vector>

#include "CardEvaluator.hpp"
#include "DecisionRequest.hpp"
#include "DuelObservation.hpp"

// Attack-target/attacker selection, ported as a concept from WindBot's
// greedy battle-phase algorithm (sort defenders by defense power, beat the
// strongest beatable one) and simplified to what GOAT's own
// MSG_SELECT_BATTLECMD prompt actually asks for: which one of our own
// attack-eligible monsters attacks next. Whatever subsequent target-select
// prompt the engine issues (if any) is handled generically by CardSelector,
// since this project's wire format doesn't expose a distinct "this is an
// attack target" hint the way WindBot's network protocol does.

namespace goat::ai {

// Naive, always-safe lethal check: true only when the opponent has no known
// monster on board and our attackers' combined ATK meets or exceeds their
// life points. Never assumes a hidden face-down opponent monster won't
// block, so this can under-detect lethal but never over-claims it.
bool has_potential_lethal(goat::CardDatabase& database, const DuelObservation& observation, const std::vector<FieldCard>& attackable);

// Index into `attackable` for the next attack to declare, or std::nullopt if
// no attack currently looks worthwhile (e.g. every attacker would be an
// obvious, ungained suicide into a stronger known opponent board and there's
// no lethal to chase).
std::optional<size_t> choose_attacker(goat::CardDatabase& database, const DuelObservation& observation, const std::vector<FieldCard>& attackable);

} // namespace goat::ai
