#pragma once

#include <cstdint>
#include <unordered_map>

#include "CardEvaluator.hpp"
#include "DuelObservation.hpp"

// Generic board-power comparison utilities, ported as concepts from
// WindBot's `AIUtil`/`DefaultExecutor` (IsOneEnemyBetter/IsAllEnemyBetter,
// the reactive-trap timing gate, the activation-count loop breaker) and
// rewritten for GOAT's classic zone layout and no-cheating observation model.

namespace goat::ai {

// True if any *known* opponent monster's effective power beats our single
// best own monster. Face-down opponent monsters (code == 0) are never
// treated as a known threat — we don't get to peek at them.
bool is_one_enemy_better(goat::CardDatabase& database, const DuelObservation& observation);

// True if every known opponent monster is at least as strong as our best own
// monster, and the opponent has at least one known monster. Used to gate
// board wipes (e.g. Torrential Tribute) to situations where we're actually
// outclassed rather than wiping a board we're winning.
bool is_all_enemy_better(goat::CardDatabase& database, const DuelObservation& observation);

// The reactive-trap timing gate: fire in response to the opponent's move,
// not on our own uncontested turn, and don't fire "into nothing" just
// because we happen to have the option.
bool should_fire_reactive_trap(const DuelObservation& observation);

// Loop-breaker matching WindBot's `_activatedCards` cap: once the same card
// has been offered/taken this many times in a single duel, stop matching it,
// so a buggy or edge-case predicate can never spin forever re-activating the
// same card. Disabled/negated cards count against the cap 3x as fast, since
// repeatedly trying to activate something that can't resolve is pure waste.
class ActivationGuard {
public:
    explicit ActivationGuard(int cap = 9) : cap_(cap) {}
    // Non-mutating: safe to call while scanning candidates without
    // penalizing cards that were merely considered, not chosen.
    bool allowed(uint32_t code) const;
    // Call once, only for the action actually committed to.
    void record(uint32_t code, bool disabled = false);

private:
    int cap_;
    std::unordered_map<uint32_t, int> counts_;
};

} // namespace goat::ai
