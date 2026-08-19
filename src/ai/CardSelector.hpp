#pragma once

#include <cstddef>
#include <vector>

#include "CardEvaluator.hpp"
#include "DecisionRequest.hpp"

// Reusable "which of these legal candidates is best" utilities, ported as
// concepts from WindBot's CardContainer/CardSelector helpers. Every function
// here only ever ranks within the candidate vector it's given — the caller
// is always an engine-provided legal candidate list (e.g.
// SelectCardRequest::candidates), never anything invented.

namespace goat::ai {

// Prefer the opponent's single most valuable known candidate (removal wants
// their best card); if every candidate belongs to us instead (a cost-payment
// or self-selection prompt), prefer our own least valuable one. Candidates
// with an unknown identity (code == 0, an unrevealed face-down) are treated
// as low-priority — never targeted ahead of a known card, since we can't
// legitimately judge what they are.
size_t best_removal_target(goat::CardDatabase& database, const std::vector<FieldCard>& candidates, uint8_t self_player);

// Least valuable candidate overall — tribute fodder, discard selection.
size_t worst_card(goat::CardDatabase& database, const std::vector<FieldCard>& candidates);

// Most valuable candidate overall.
size_t best_card(goat::CardDatabase& database, const std::vector<FieldCard>& candidates);

// Indices of the `count` least/most valuable candidates (or all of them, if
// fewer than `count` exist), most-relevant first — for prompts that need
// more than one pick (multi-tribute, multi-card select).
std::vector<size_t> rank_worst(goat::CardDatabase& database, const std::vector<FieldCard>& candidates, size_t count);
std::vector<size_t> rank_best(goat::CardDatabase& database, const std::vector<FieldCard>& candidates, size_t count);

// Same opponent-preferring policy as best_removal_target, extended to more
// than one required pick.
std::vector<size_t> rank_removal_targets(goat::CardDatabase& database, const std::vector<FieldCard>& candidates, uint8_t self_player, size_t count);

} // namespace goat::ai
