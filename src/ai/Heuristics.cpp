#include "Heuristics.hpp"

#include <algorithm>

namespace goat::ai {

bool is_one_enemy_better(goat::CardDatabase& database, const DuelObservation& observation) {
    int32_t best_own = 0;
    for (const auto& card : observation.own_monsters)
        if (card.code) best_own = std::max(best_own, effective_power(database, card.code, card.position));
    for (const auto& card : observation.opponent_monsters) {
        if (!card.code) continue;
        if (effective_power(database, card.code, card.position) > best_own) return true;
    }
    return false;
}

bool is_all_enemy_better(goat::CardDatabase& database, const DuelObservation& observation) {
    int32_t best_own = 0;
    for (const auto& card : observation.own_monsters)
        if (card.code) best_own = std::max(best_own, effective_power(database, card.code, card.position));
    bool any_known = false;
    for (const auto& card : observation.opponent_monsters) {
        if (!card.code) continue;
        any_known = true;
        if (effective_power(database, card.code, card.position) < best_own) return false;
    }
    return any_known;
}

bool should_fire_reactive_trap(const DuelObservation& observation) {
    if (observation.chain_stack.empty()) {
        // No chain window open yet: only justified if we didn't just Normal
        // Summon uncontested ourselves.
        return !(observation.last_summon_player.has_value() && *observation.last_summon_player == observation.self_player);
    }
    return observation.last_chain_player.has_value() && *observation.last_chain_player != observation.self_player;
}

bool ActivationGuard::allowed(uint32_t code) const {
    const auto it = counts_.find(code);
    return it == counts_.end() || it->second < cap_;
}

void ActivationGuard::record(uint32_t code, bool disabled) {
    counts_[code] += disabled ? 3 : 1;
}

} // namespace goat::ai
