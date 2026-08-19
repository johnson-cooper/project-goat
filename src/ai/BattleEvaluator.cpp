#include "BattleEvaluator.hpp"

#include <algorithm>
#include <limits>

namespace goat::ai {

bool has_potential_lethal(goat::CardDatabase& database, const DuelObservation& observation, const std::vector<FieldCard>& attackable) {
    for (const auto& monster : observation.opponent_monsters)
        if (monster.occupied) return false; // Even an unrevealed monster might block — never claim lethal past one.
    int64_t total_attack = 0;
    for (const auto& attacker : attackable) total_attack += database.resolve(attacker.code).attack;
    return total_attack >= observation.opponent_life;
}

std::optional<size_t> choose_attacker(goat::CardDatabase& database, const DuelObservation& observation, const std::vector<FieldCard>& attackable) {
    if (attackable.empty()) return std::nullopt;

    size_t best_index = 0;
    int32_t best_attack = std::numeric_limits<int32_t>::min();
    for (size_t i = 0; i < attackable.size(); ++i) {
        const int32_t attack = database.resolve(attackable[i].code).attack;
        if (attack > best_attack) { best_attack = attack; best_index = i; }
    }

    if (has_potential_lethal(database, observation, attackable)) return best_index;

    // Suicide avoidance: if the opponent has a known monster at least as
    // strong as our best attacker, sending it in would trade down (or die)
    // for nothing, so hold back rather than blindly swing like the baseline
    // RandomAgent does.
    int32_t opponent_best = 0;
    bool any_known_opponent = false;
    for (const auto& monster : observation.opponent_monsters) {
        if (!monster.occupied || monster.code == 0) continue;
        any_known_opponent = true;
        opponent_best = std::max(opponent_best, effective_power(database, monster.code, monster.position));
    }
    if (any_known_opponent && opponent_best >= best_attack) return std::nullopt;

    return best_index;
}

} // namespace goat::ai
