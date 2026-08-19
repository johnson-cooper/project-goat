#pragma once

#include "DecisionRequest.hpp"
#include "DecisionResponse.hpp"
#include "DuelObservation.hpp"

namespace goat::ai {

using DecisionResponse = Response;

// Anything capable of answering an engine decision prompt for one seat.
// ygopro-core remains the sole legality authority: a DuelAgent only ever
// chooses among candidates already present in the DecisionRequest it's
// given, never invents one.
class DuelAgent {
public:
    virtual ~DuelAgent() = default;
    virtual DecisionResponse choose(const DuelObservation& observation, const DecisionRequest& request) = 0;
};

} // namespace goat::ai
