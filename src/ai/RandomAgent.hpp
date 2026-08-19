#pragma once

#include "DuelAgent.hpp"
#include "../cards/CardDatabase.hpp"

namespace goat::ai {

// The project's original baseline CPU strategy (previously the only
// behavior `RandomAgent` in src/main.cpp had), reimplemented against the
// DuelAgent interface with byte-identical decisions: prefer a legal Level<=4
// Normal Summon, otherwise attack with anything available, decline every
// optional prompt, and take the first legal option everywhere else. This is
// the regression baseline, the `"random"` NPC agent, and the Easy-difficulty
// fallback.
class RandomAgent : public DuelAgent {
public:
    explicit RandomAgent(goat::CardDatabase& database) : database_(database) {}
    DecisionResponse choose(const DuelObservation& observation, const DecisionRequest& request) override;

private:
    goat::CardDatabase& database_;
};

} // namespace goat::ai
