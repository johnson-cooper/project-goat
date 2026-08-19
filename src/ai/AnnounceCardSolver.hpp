#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "../cards/CardDatabase.hpp"

// Answers MSG_ANNOUNCE_CARD: "declare a card name satisfying condition X",
// where X arrives as a small stack-machine bytecode program (the same
// OPCODE_* encoding ygopro-core's own card-effect scripts use for
// Duel.AnnounceCard's filter argument). This is a faithful port of
// ygopro-core's own field::process(Processors::AnnounceCard&) /
// is_declarable (external/ygopro-core/playerop.cpp) — not a
// reimplementation from scratch — since the response must satisfy the
// exact same predicate the engine will re-validate it against.

namespace goat::ai {

// Searches the full pinned card pool (CardDatabase::all_codes()) for a code
// satisfying `predicate_opcodes`, returning the first match or std::nullopt
// if none exists (should not normally happen for a well-formed prompt).
std::optional<uint32_t> find_declarable_card(goat::CardDatabase& database, const std::vector<uint64_t>& predicate_opcodes);

// Same search, returning up to `limit` matches — used to offer a human
// player a small menu of legal declarations instead of only the AI's single
// pick.
std::vector<uint32_t> find_declarable_cards(goat::CardDatabase& database, const std::vector<uint64_t>& predicate_opcodes, size_t limit);

} // namespace goat::ai
