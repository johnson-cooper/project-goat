#pragma once

#include <random>

#include "DuelAgent.hpp"
#include "Executor.hpp"
#include "Heuristics.hpp"
#include "RandomAgent.hpp"

namespace goat::ai {

// `Hard` currently behaves identically to `Normal` — the extension point
// (deck-aware executors, resource-preservation weighting) is architected
// (see ExecutorList/GoatAgent's constructor) but deliberately not filled in
// this pass. `Easy` fully delegates to RandomAgent plus a seeded chance of
// picking a different-but-still-legal option, never a real strategic
// weakening beyond that (Easy still never plays an illegal action).
enum class Difficulty { Easy, Normal, Hard };

// The concrete "smart" CPU: an ExecutorList (see executors/) layered over
// the generic Heuristics/CardEvaluator/CardSelector/BattleEvaluator
// utilities, falling back to the same legal-first default RandomAgent uses
// for anything not covered by a specific rule. Deterministic given the same
// seed — the seed is currently only consulted by Easy difficulty, since
// Normal/Hard have no randomized choices yet.
class GoatAgent : public DuelAgent {
public:
    GoatAgent(goat::CardDatabase& database, ExecutorList executors, Difficulty difficulty = Difficulty::Normal, uint64_t seed = 0);
    DecisionResponse choose(const DuelObservation& observation, const DecisionRequest& request) override;

private:
    goat::CardDatabase& database_;
    ExecutorList executors_;
    Difficulty difficulty_;
    std::mt19937_64 random_;
    ActivationGuard guard_;
    RandomAgent fallback_;
};

} // namespace goat::ai
