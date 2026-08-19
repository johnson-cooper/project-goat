// Verifies deck-archetype detection picks the right executor table by
// signature card, and spot-checks one behavior difference each for Chaos
// Control, Gearfried, and Burn versus the plain generic table — without a
// live duel, since DecisionRequest/DuelObservation are plain data.
#include <cassert>

#include "ai/executors/BurnExecutor.hpp"
#include "ai/executors/ChaosControlExecutor.hpp"
#include "ai/executors/DeckArchetype.hpp"
#include "ai/executors/GearfriedExecutor.hpp"
#include "ai/executors/GenericGoatExecutors.hpp"
#include "cards/CardDatabase.hpp"

extern "C" {
#include "ocgapi_constants.h"
}

using namespace goat::ai;

int main() {
    // --- Detection: signature card present -> matching archetype.
    assert(detect_deck_archetype({goat_card::BlackLusterSoldierEnvoy, 12345}) == DeckArchetype::ChaosControl);
    assert(detect_deck_archetype({goat_card::ChaosSorcerer}) == DeckArchetype::ChaosControl);
    assert(detect_deck_archetype({goat_card::GearfriedTheIronKnight, 12345}) == DeckArchetype::Gearfried);
    assert(detect_deck_archetype({goat_card::JustDesserts, goat_card::SecretBarrel}) == DeckArchetype::Burn);
    assert(detect_deck_archetype({goat_card::JustDesserts}) == DeckArchetype::Generic); // A single signal isn't enough on its own.
    assert(detect_deck_archetype({55144522, 79571449}) == DeckArchetype::Generic);

    goat::CardDatabase database("external/BabelCDB/cards.cdb", "external/BabelCDB/goat-entries.cdb");
    DuelObservation observation{};
    observation.self_player = 0;

    // --- Chaos Control: Special Summoning Black Luster Soldier is offered
    // ahead of a generic advantage spell also in the activatable list.
    {
        auto executors = build_chaos_control_executors();
        IdleCommandRequest request;
        request.player = 0;
        request.special_summonable.push_back(FieldCard{goat_card::BlackLusterSoldierEnvoy, 0, LOCATION_HAND, 0});
        request.activatable.push_back(ActivatableCard{FieldCard{goat_card::PotOfGreed, 0, LOCATION_HAND, 0}, 0, 0});
        request.can_end_phase = true;
        auto choice = evaluate_idle_executors(executors, request, database, observation);
        assert(choice.has_value() && choice->type == ExecutorType::SpecialSummon);
    }

    // --- Gearfried: Reinforcement of the Army is prioritized over a
    // generic wildcard Summon when both are legal at once.
    {
        auto executors = build_gearfried_executors();
        IdleCommandRequest request;
        request.player = 0;
        request.activatable.push_back(ActivatableCard{FieldCard{goat_card::ReinforcementOfTheArmy, 0, LOCATION_HAND, 0}, 0, 0});
        request.summonable.push_back(FieldCard{goat_card::GearfriedTheIronKnight, 0, LOCATION_HAND, 1});
        request.can_end_phase = true;
        auto choice = evaluate_idle_executors(executors, request, database, observation);
        assert(choice.has_value() && choice->type == ExecutorType::Activate);
        assert(request.activatable[choice->index].card.code == goat_card::ReinforcementOfTheArmy);
    }

    // --- Burn: Just Desserts fires unconditionally once legal.
    {
        auto executors = build_burn_executors();
        IdleCommandRequest request;
        request.player = 0;
        request.activatable.push_back(ActivatableCard{FieldCard{goat_card::JustDesserts, 0, LOCATION_HAND, 0}, 0, 0});
        request.can_end_phase = true;
        auto choice = evaluate_idle_executors(executors, request, database, observation);
        assert(choice.has_value() && choice->type == ExecutorType::Activate);
        assert(request.activatable[choice->index].card.code == goat_card::JustDesserts);
    }

    // --- Burn: Ojama Trio withheld when the opponent's board is already
    // nearly full (little left to flood).
    {
        auto executors = build_burn_executors();
        IdleCommandRequest request;
        request.player = 0;
        request.activatable.push_back(ActivatableCard{FieldCard{goat_card::OjamaTrio, 0, LOCATION_HAND, 0}, 0, 0});
        request.can_end_phase = true;
        DuelObservation full_board{};
        full_board.self_player = 0;
        for (int i = 0; i < 5; ++i) full_board.opponent_monsters[i] = {true, 11091375, POS_FACEUP_ATTACK, static_cast<uint8_t>(i)};
        auto choice = evaluate_idle_executors(executors, request, database, full_board);
        assert(!(choice.has_value() && choice->type == ExecutorType::Activate && request.activatable[choice->index].card.code == goat_card::OjamaTrio));
    }

    return 0;
}
