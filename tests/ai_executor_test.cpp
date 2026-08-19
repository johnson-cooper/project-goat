// Unit-tests the WindBot-inspired executor pattern: registration order is
// priority (first match wins), a specific-card rule registered before a
// wildcard fallback for the same ExecutorType takes priority over it, and
// GoatAgent's staple executors only fire when their generic gating condition
// actually holds — never outside the engine-offered candidate set.
#include <cassert>

#include "ai/Executor.hpp"
#include "ai/executors/GenericGoatExecutors.hpp"
#include "cards/CardDatabase.hpp"

extern "C" {
#include "ocgapi_constants.h"
}

using namespace goat::ai;

namespace {

IdleCommandRequest make_idle_request_with_activatable(uint32_t code, uint64_t description = 0) {
    IdleCommandRequest request;
    request.player = 0;
    request.activatable.push_back(ActivatableCard{FieldCard{code, 0, LOCATION_HAND, 0}, description, 0});
    request.can_battle_phase = true;
    request.can_end_phase = true;
    return request;
}

} // namespace

int main() {
    goat::CardDatabase database("external/BabelCDB/cards.cdb", "external/BabelCDB/goat-entries.cdb");
    DuelObservation empty_observation{};
    empty_observation.self_player = 0;

    // --- Registration order is priority: a later, more specific rule for
    // the same card never overrides an earlier match.
    {
        ExecutorList list;
        list.add(ExecutorType::Activate, goat_card::PotOfGreed, [](const ExecutorContext&) { return true; });
        list.add(ExecutorType::Activate, goat_card::PotOfGreed, [](const ExecutorContext&) { return false; }); // never reached
        auto request = make_idle_request_with_activatable(goat_card::PotOfGreed);
        auto choice = evaluate_idle_executors(list, request, database, empty_observation);
        assert(choice.has_value() && choice->type == ExecutorType::Activate && choice->index == 0);
    }

    // --- A wildcard wins only when no specific-card rule for that type
    // matched first (registered after, so a specific rule earlier still wins).
    {
        ExecutorList list;
        list.add(ExecutorType::Activate, goat_card::PotOfGreed, [](const ExecutorContext&) { return false; });
        list.add(ExecutorType::Activate, [](const ExecutorContext&) { return true; }); // wildcard fallback
        auto request = make_idle_request_with_activatable(goat_card::GracefulCharity);
        auto choice = evaluate_idle_executors(list, request, database, empty_observation);
        assert(choice.has_value() && choice->type == ExecutorType::Activate);
        assert(request.activatable[choice->index].card.code == goat_card::GracefulCharity);
    }

    // --- A rule never matches a candidate it wasn't offered: Pot of Greed's
    // registered rule must not fire when only an unrelated card is legal.
    {
        auto executors = build_generic_goat_executors();
        auto request = make_idle_request_with_activatable(45986603 /* Snatch Steal — not in the generic table */);
        auto choice = evaluate_idle_executors(executors, request, database, empty_observation);
        // No registered rule claims this card, and there's no summon/set/
        // reposition candidate either, so the fallback is Battle/End Phase.
        assert(choice.has_value());
        assert(choice->type == ExecutorType::EnterBattlePhase || choice->type == ExecutorType::EndPhase);
    }

    // --- Pot of Greed's generic rule is unconditional once legal.
    {
        auto executors = build_generic_goat_executors();
        auto request = make_idle_request_with_activatable(goat_card::PotOfGreed);
        auto choice = evaluate_idle_executors(executors, request, database, empty_observation);
        assert(choice.has_value() && choice->type == ExecutorType::Activate && choice->index == 0);
    }

    // --- Heavy Storm's generic gate: never fires when our own backrow is
    // larger than (or equal to) the opponent's — it would only trade away
    // our own better position.
    {
        auto executors = build_generic_goat_executors();
        auto request = make_idle_request_with_activatable(goat_card::HeavyStorm);
        DuelObservation observation{};
        observation.self_player = 0;
        observation.own_spells[0] = {true, 79571449, POS_FACEUP_ATTACK, 0};
        observation.own_spells[1] = {true, 55144522, POS_FACEUP_ATTACK, 1};
        // Opponent has strictly fewer backrow cards than we do.
        observation.opponent_spells[0] = {true, 19613556, POS_FACEDOWN_DEFENSE, 0};
        auto choice = evaluate_idle_executors(executors, request, database, observation);
        assert(choice.has_value());
        assert(choice->type != ExecutorType::Activate || request.activatable[choice->index].card.code != goat_card::HeavyStorm);
    }

    // --- ...but does fire when the opponent's backrow outnumbers ours.
    {
        auto executors = build_generic_goat_executors();
        auto request = make_idle_request_with_activatable(goat_card::HeavyStorm);
        DuelObservation observation{};
        observation.self_player = 0;
        observation.opponent_spells[0] = {true, 19613556, POS_FACEDOWN_DEFENSE, 0};
        observation.opponent_spells[1] = {true, 14087893, POS_FACEDOWN_DEFENSE, 1};
        auto choice = evaluate_idle_executors(executors, request, database, observation);
        assert(choice.has_value() && choice->type == ExecutorType::Activate);
        assert(request.activatable[choice->index].card.code == goat_card::HeavyStorm);
    }

    return 0;
}
