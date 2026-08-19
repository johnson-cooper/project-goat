#include "ObservationBuilder.hpp"

extern "C" {
#include "ocgapi_constants.h"
}

namespace goat::ai {

DuelObservation build_observation(const ObservationInputs& inputs) {
    const uint8_t self = inputs.self_player;
    const uint8_t opponent = static_cast<uint8_t>(1 - self);

    DuelObservation observation;
    observation.self_player = self;
    observation.own_life = inputs.life[self];
    observation.opponent_life = inputs.life[opponent];
    observation.turn_number = inputs.turn_number;
    observation.turn_player = inputs.turn_player;
    observation.phase = inputs.phase;
    observation.own_hand = inputs.hand_cards[self]; // Never inputs.hand_cards[opponent] — that would leak hidden information.
    observation.opponent_hand_count = inputs.hand_count[opponent];

    for (uint8_t i = 0; i < 5; ++i) {
        const auto& own = inputs.monsters[self][i];
        observation.own_monsters[i] = {own.code != 0, own.code, own.position, i};
        const auto& opp = inputs.monsters[opponent][i];
        const bool hidden = opp.code != 0 && (opp.position & POS_FACEDOWN);
        observation.opponent_monsters[i] = {opp.code != 0, hidden ? 0u : opp.code, opp.position, i};
    }
    for (uint8_t i = 0; i < 6; ++i) {
        const auto& own = inputs.spells[self][i];
        observation.own_spells[i] = {own.code != 0, own.code, own.position, i};
        const auto& opp = inputs.spells[opponent][i];
        const bool hidden = opp.code != 0 && (opp.position & POS_FACEDOWN);
        observation.opponent_spells[i] = {opp.code != 0, hidden ? 0u : opp.code, opp.position, i};
    }

    observation.own_deck_count = inputs.deck_count[self];
    observation.own_grave_count = inputs.grave_count[self];
    observation.own_extra_count = inputs.extra_count[self];
    observation.own_banished_count = inputs.banished_count[self];
    observation.opponent_deck_count = inputs.deck_count[opponent];
    observation.opponent_grave_count = inputs.grave_count[opponent];
    observation.opponent_extra_count = inputs.extra_count[opponent];
    observation.opponent_banished_count = inputs.banished_count[opponent];

    observation.chain_stack = inputs.chain_stack;
    observation.last_chain_player = inputs.last_chain_player;
    observation.last_summon_player = inputs.last_summon_player;
    return observation;
}

} // namespace goat::ai
