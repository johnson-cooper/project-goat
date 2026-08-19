// Verifies goat::ai::build_observation's no-cheating invariant: a
// DuelObservation built for one seat must never carry the opponent's hand
// identities or un-revealed face-down card codes, while everything
// legitimately visible (own cards, face-up opponent cards, public counts)
// comes through unchanged.
#include <cassert>

#include "ai/ObservationBuilder.hpp"

extern "C" {
#include "ocgapi_constants.h"
}

using goat::ai::ObservationInputs;
using goat::ai::RawFieldCard;

int main() {
    ObservationInputs inputs;
    inputs.self_player = 0;
    inputs.life = {8000, 6000};
    inputs.hand_count = {3, 4};
    inputs.hand_cards[0] = {55144522, 79571449, 44095762}; // our own hand: fully known to us.
    inputs.hand_cards[1] = {12345678, 87654321, 11111111, 22222222}; // opponent hand: must never surface.

    // Player 0 (self): a face-down monster is still fully known to its own controller.
    inputs.monsters[0][0] = RawFieldCard{5318639, POS_FACEDOWN_DEFENSE};
    // Player 1 (opponent): one face-up monster (known) and one face-down monster (must be masked).
    inputs.monsters[1][0] = RawFieldCard{44095762, POS_FACEUP_ATTACK};
    inputs.monsters[1][1] = RawFieldCard{53582587, POS_FACEDOWN_DEFENSE};

    inputs.spells[1][0] = RawFieldCard{19613556, POS_FACEDOWN_DEFENSE}; // opponent set spell/trap: masked.
    inputs.spells[1][1] = RawFieldCard{14087893, POS_FACEUP_ATTACK};    // opponent face-up continuous-style spell: known.

    inputs.deck_count = {30, 28};
    inputs.grave_count = {2, 5};
    inputs.turn_player = 1;
    inputs.turn_number = 4;
    inputs.chain_stack = {97077563};
    inputs.last_chain_player = 1;
    inputs.last_summon_player = 0;

    const auto observation = goat::ai::build_observation(inputs);

    // Own hand: fully visible.
    assert(observation.own_hand.size() == 3);
    // Opponent hand: identities never exposed, only the count.
    assert(observation.opponent_hand_count == 4);

    // Own face-down monster: code still visible to its own controller.
    assert(observation.own_monsters[0].occupied && observation.own_monsters[0].code == 5318639u);

    // Opponent face-up monster: code visible.
    assert(observation.opponent_monsters[0].occupied && observation.opponent_monsters[0].code == 44095762u);
    // Opponent face-down monster: occupied but identity masked to 0.
    assert(observation.opponent_monsters[1].occupied && observation.opponent_monsters[1].code == 0u);
    // A genuinely empty opponent monster zone slot: not occupied at all.
    assert(!observation.opponent_monsters[2].occupied);

    // Same face-down masking rule for the opponent's spell/trap zone.
    assert(observation.opponent_spells[0].occupied && observation.opponent_spells[0].code == 0u);
    assert(observation.opponent_spells[1].occupied && observation.opponent_spells[1].code == 14087893u);

    // Public counts pass straight through for both sides.
    assert(observation.own_deck_count == 30 && observation.opponent_deck_count == 28);
    assert(observation.own_grave_count == 2 && observation.opponent_grave_count == 5);

    // Chain identities are always public (activating an effect reveals it).
    assert(observation.chain_stack.size() == 1 && observation.chain_stack[0] == 97077563u);
    assert(observation.last_chain_player.has_value() && *observation.last_chain_player == 1);
    assert(observation.last_summon_player.has_value() && *observation.last_summon_player == 0);

    // Building for the *other* seat flips which hand/side is "own" vs
    // "opponent" — confirms the masking is genuinely self_player-relative,
    // not hardcoded to seat 0.
    inputs.self_player = 1;
    const auto flipped = goat::ai::build_observation(inputs);
    assert(flipped.own_hand.size() == 4); // now player 1's hand is "own" and fully visible.
    assert(flipped.opponent_hand_count == 3);
    assert(flipped.opponent_monsters[0].occupied && flipped.opponent_monsters[0].code == 0u); // our own player-0 facedown, now viewed as "opponent".

    return 0;
}
