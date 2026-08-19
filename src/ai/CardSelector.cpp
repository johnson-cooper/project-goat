#include "CardSelector.hpp"

#include <algorithm>
#include <limits>
#include <numeric>

namespace goat::ai {
namespace {

int32_t value_of(goat::CardDatabase& database, const FieldCard& card) {
    if (card.code == 0) return std::numeric_limits<int32_t>::min(); // Unknown identity: never preferred over a known card.
    return generic_value(database, card.code);
}

std::vector<size_t> rank_by(const std::vector<int32_t>& values, size_t count, bool ascending) {
    std::vector<size_t> order(values.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return ascending ? values[a] < values[b] : values[a] > values[b];
    });
    if (order.size() > count) order.resize(count);
    return order;
}

} // namespace

size_t best_removal_target(goat::CardDatabase& database, const std::vector<FieldCard>& candidates, uint8_t self_player) {
    bool any_opponent = false;
    for (const auto& card : candidates) if (card.controller != self_player) any_opponent = true;
    size_t best = 0;
    int32_t best_value = std::numeric_limits<int32_t>::min();
    bool found = false;
    for (size_t i = 0; i < candidates.size(); ++i) {
        const bool is_opponent = candidates[i].controller != self_player;
        if (any_opponent && !is_opponent) continue; // Prefer their cards when any are offered.
        const int32_t value = any_opponent ? value_of(database, candidates[i]) : -value_of(database, candidates[i]);
        if (!found || value > best_value) { best = i; best_value = value; found = true; }
    }
    return best;
}

size_t worst_card(goat::CardDatabase& database, const std::vector<FieldCard>& candidates) {
    size_t worst = 0;
    int32_t worst_value = std::numeric_limits<int32_t>::max();
    for (size_t i = 0; i < candidates.size(); ++i) {
        const int32_t value = value_of(database, candidates[i]);
        if (value < worst_value) { worst = i; worst_value = value; }
    }
    return worst;
}

size_t best_card(goat::CardDatabase& database, const std::vector<FieldCard>& candidates) {
    size_t best = 0;
    int32_t best_value = std::numeric_limits<int32_t>::min();
    for (size_t i = 0; i < candidates.size(); ++i) {
        const int32_t value = value_of(database, candidates[i]);
        if (value > best_value) { best = i; best_value = value; }
    }
    return best;
}

std::vector<size_t> rank_worst(goat::CardDatabase& database, const std::vector<FieldCard>& candidates, size_t count) {
    std::vector<int32_t> values;
    values.reserve(candidates.size());
    for (const auto& card : candidates) values.push_back(value_of(database, card));
    return rank_by(values, count, true);
}

std::vector<size_t> rank_best(goat::CardDatabase& database, const std::vector<FieldCard>& candidates, size_t count) {
    std::vector<int32_t> values;
    values.reserve(candidates.size());
    for (const auto& card : candidates) values.push_back(value_of(database, card));
    return rank_by(values, count, false);
}

std::vector<size_t> rank_removal_targets(goat::CardDatabase& database, const std::vector<FieldCard>& candidates, uint8_t self_player, size_t count) {
    bool any_opponent = false;
    for (const auto& card : candidates) if (card.controller != self_player) any_opponent = true;
    std::vector<int32_t> values;
    values.reserve(candidates.size());
    for (const auto& card : candidates) {
        const bool is_opponent = card.controller != self_player;
        if (any_opponent && !is_opponent) { values.push_back(std::numeric_limits<int32_t>::min()); continue; }
        values.push_back(any_opponent ? value_of(database, card) : -value_of(database, card));
    }
    return rank_by(values, count, false);
}

} // namespace goat::ai
