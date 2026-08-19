#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "../cards/CardDatabase.hpp"
#include "DecisionRequest.hpp"
#include "DuelObservation.hpp"

// The core WindBot-inspired rule-table pattern: a flat, ordered list of
// (ActionKind, cardCode | wildcard, predicate) entries, evaluated first-match
// wins. Registration order is priority — put removal/board-wipe and
// hard-counter rules first, generic fallbacks last (see
// executors/GenericGoatExecutors.cpp).

namespace goat::ai {

// GOAT-safe subset of WindBot's ExecutorType: no SummonOrSet (GOAT's
// idle-command wire format already lists Summon and Set-monster as separate
// candidate lists per card; ordering the rule table handles the
// preference), no Surrender (no engine mechanism for it).
enum class ExecutorType {
    Summon,
    SpecialSummon,
    Reposition,
    SetMonster,
    SetSpellTrap,
    Activate,
    EnterBattlePhase,
    EnterMainPhase2,
    EndPhase,
};

struct ExecutorContext {
    goat::CardDatabase& database;
    const DuelObservation& observation;
    FieldCard card;
    uint64_t description{};
};

struct Executor {
    ExecutorType type;
    std::optional<uint32_t> card_code; // std::nullopt matches any code (a wildcard/generic rule).
    std::function<bool(const ExecutorContext&)> condition; // empty means "always true once type/code match".
};

class ExecutorList {
public:
    void add(ExecutorType type, uint32_t card_code, std::function<bool(const ExecutorContext&)> condition = {});
    void add(ExecutorType type, std::function<bool(const ExecutorContext&)> condition = {});
    const std::vector<Executor>& entries() const { return executors_; }

private:
    std::vector<Executor> executors_;
};

struct IdleActionChoice {
    ExecutorType type;
    size_t index{}; // Meaningless (0) for EnterBattlePhase/EndPhase.
};

// Faithful port of WindBot's GameAI.OnSelectIdleCmd control flow: a single
// ordered pass over `executors.entries()`; each entry only ever matches
// against the one candidate list its own declared ExecutorType corresponds
// to. The first entry (in registration order) that both matches a candidate
// and whose predicate accepts wins immediately — a later-registered entry
// for a "more important" category never jumps ahead of an earlier one.
std::optional<IdleActionChoice> evaluate_idle_executors(
    const ExecutorList& executors,
    const IdleCommandRequest& request,
    goat::CardDatabase& database,
    const DuelObservation& observation);

// The same Activate-only scan, reused for battle-phase activations,
// select-chain, and select-effect-yes/no prompts.
std::optional<size_t> evaluate_activate_executors(
    const ExecutorList& executors,
    const std::vector<ActivatableCard>& activatable,
    goat::CardDatabase& database,
    const DuelObservation& observation);

} // namespace goat::ai
