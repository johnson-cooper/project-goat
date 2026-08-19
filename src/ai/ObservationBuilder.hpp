#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "DuelObservation.hpp"

// The one bridge between whatever a duel-driving process already tracks
// (main.cpp's BoardState today) and the player-relative, no-cheating
// DuelObservation a DuelAgent actually sees. Kept as plain data in/plain
// data out specifically so the face-down-masking logic can be unit tested
// (see tests/ai_observation_test.cpp) without needing a live duel or
// main.cpp's own types.

namespace goat::ai {

struct RawFieldCard {
    uint32_t code{}; // 0 means the zone slot is empty.
    uint8_t position{};
};

struct ObservationInputs {
    uint8_t self_player{};
    std::array<int32_t, 2> life{};
    std::array<uint32_t, 2> hand_count{};
    std::array<std::vector<uint32_t>, 2> hand_cards{};
    std::array<std::array<RawFieldCard, 5>, 2> monsters{};
    std::array<std::array<RawFieldCard, 6>, 2> spells{};
    std::array<uint32_t, 2> deck_count{};
    std::array<uint32_t, 2> grave_count{};
    std::array<uint32_t, 2> extra_count{};
    std::array<uint32_t, 2> banished_count{};
    uint8_t turn_player{};
    uint32_t turn_number{};
    uint16_t phase{};
    std::vector<uint32_t> chain_stack;
    std::optional<uint8_t> last_chain_player;
    std::optional<uint8_t> last_summon_player;
};

// CRITICAL INVARIANT: the result never carries `inputs.hand_cards[opponent]`
// identities, and any opponent monster/spell slot whose position has a
// face-down bit set has its code replaced with 0 (identity unknown) — see
// tests/ai_observation_test.cpp.
DuelObservation build_observation(const ObservationInputs& inputs);

} // namespace goat::ai
