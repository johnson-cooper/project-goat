#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// A player-relative, engine-independent snapshot of duel state. This is
// intentionally a plain data struct with no dependency on ygopro-core or
// main.cpp's BoardState: the engine-facing layer (main.cpp) is responsible
// for filling one of these in per seat from whatever it already tracks, and
// every DuelAgent only ever sees the finished, already-masked result. This
// also makes DuelObservation trivial to construct by hand in unit tests.
//
// CRITICAL INVARIANT: a DuelObservation built `for_player` must never carry
// information that player couldn't legitimately know — no opponent hand
// identities, no un-revealed face-down card codes. See
// tests/ai_observation_test.cpp.

namespace goat::ai {

struct ObservedCard {
    bool occupied{};  // false means this zone slot is genuinely empty.
    uint32_t code{};  // 0 while occupied means "present but identity unknown" (an opponent face-down card).
    uint8_t position{};
    uint8_t sequence{};
};

struct DuelObservation {
    uint8_t self_player{};

    int32_t own_life{};
    int32_t opponent_life{};

    uint32_t turn_number{};
    uint8_t turn_player{};
    uint16_t phase{};

    std::vector<uint32_t> own_hand; // own hand identities: always legitimately known.
    uint32_t opponent_hand_count{}; // count only — never identities.

    std::array<ObservedCard, 5> own_monsters{};
    std::array<ObservedCard, 6> own_spells{};
    std::array<ObservedCard, 5> opponent_monsters{}; // face-down slots carry code == 0.
    std::array<ObservedCard, 6> opponent_spells{};   // face-down slots carry code == 0.

    uint32_t own_deck_count{};
    uint32_t own_grave_count{};
    uint32_t own_extra_count{};
    uint32_t own_banished_count{};
    uint32_t opponent_deck_count{};
    uint32_t opponent_grave_count{};
    uint32_t opponent_extra_count{};
    uint32_t opponent_banished_count{};

    // Chain-link identities are always public once a card is chained (that's
    // how activating an effect works), so this carries real card codes
    // regardless of `self_player`.
    std::vector<uint32_t> chain_stack;
    std::optional<uint8_t> last_chain_player;  // who controls the top-of-chain card, if any.
    std::optional<uint8_t> last_summon_player; // who most recently Normal Summoned, if known.
};

} // namespace goat::ai
