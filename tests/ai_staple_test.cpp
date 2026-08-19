// End-to-end unit tests for GoatAgent against synthetic (but
// engine-shaped) DecisionRequest/DuelObservation fixtures — no live duel
// needed, since both types are deliberately plain data. Covers the
// project's "definition of success" examples: Pot of Greed is activated
// when legal, Heavy Storm doesn't fire into an empty opponent backrow while
// destroying our own better one, the CPU prefers an available lethal attack,
// and it doesn't attack into a visibly stronger monster without reason —
// and, throughout, every response stays inside the offered candidate set.
#include <cassert>
#include <cstring>

#include "ai/GoatAgent.hpp"
#include "ai/executors/GenericGoatExecutors.hpp"
#include "cards/CardDatabase.hpp"

extern "C" {
#include "ocgapi_constants.h"
}

using namespace goat::ai;

namespace {

int32_t decode_type(const DecisionResponse& response) {
    uint32_t raw; std::memcpy(&raw, response.data(), 4); return static_cast<int32_t>(raw & 0xffffu);
}
int32_t decode_index(const DecisionResponse& response) {
    uint32_t raw; std::memcpy(&raw, response.data(), 4); return static_cast<int32_t>(raw >> 16);
}

GoatAgent make_agent(goat::CardDatabase& database) {
    return GoatAgent(database, build_generic_goat_executors(), Difficulty::Normal, 1);
}

} // namespace

int main() {
    goat::CardDatabase database("external/BabelCDB/cards.cdb", "external/BabelCDB/goat-entries.cdb");
    auto agent = make_agent(database);

    // --- Pot of Greed is activated when it's among the legal idle actions.
    {
        IdleCommandRequest request;
        request.player = 0;
        request.activatable.push_back(ActivatableCard{FieldCard{goat_card::PotOfGreed, 0, LOCATION_HAND, 0}, 0, 0});
        request.activatable.push_back(ActivatableCard{FieldCard{45986603 /* Snatch Steal */, 0, LOCATION_HAND, 1}, 0, 0});
        request.can_battle_phase = true;
        request.can_end_phase = true;
        DuelObservation observation{}; observation.self_player = 0;
        auto response = agent.choose(observation, request);
        assert(decode_type(response) == idle_action::Activate);
        assert(request.activatable[decode_index(response)].card.code == goat_card::PotOfGreed);
    }

    // --- Heavy Storm never destroys our own larger backrow to hit an
    // opponent with nothing set.
    {
        IdleCommandRequest request;
        request.player = 0;
        request.activatable.push_back(ActivatableCard{FieldCard{goat_card::HeavyStorm, 0, LOCATION_HAND, 0}, 0, 0});
        request.can_end_phase = true;
        DuelObservation observation{}; observation.self_player = 0;
        observation.own_spells[0] = {true, goat_card::MirrorForce, POS_FACEDOWN_DEFENSE, 0};
        observation.own_spells[1] = {true, goat_card::TorrentialTribute, POS_FACEDOWN_DEFENSE, 1};
        auto response = agent.choose(observation, request);
        // Nothing else is legal (no summon/set/battle), so the only question
        // is whether Heavy Storm's own gate fired — it must not have.
        assert(!(decode_type(response) == idle_action::Activate));
    }

    // --- Every SelectCardRequest response index stays inside the offered
    // candidate set, even when candidates belong to a mix of both players.
    {
        SelectCardRequest request;
        request.player = 0; request.min = 1; request.max = 1;
        request.candidates.push_back(FieldCard{5318639, 0, LOCATION_MZONE, 0, POS_FACEUP_ATTACK});
        request.candidates.push_back(FieldCard{89631139, 1, LOCATION_MZONE, 1, POS_FACEUP_ATTACK});
        DuelObservation observation{}; observation.self_player = 0;
        DecisionRequest variant = request;
        auto response = agent.choose(observation, variant);
        uint32_t count; std::memcpy(&count, response.data() + 4, 4);
        assert(count == 1);
        uint32_t index; std::memcpy(&index, response.data() + 8, 4);
        assert(index < request.candidates.size());
    }

    // --- Battle: prefers a lethal attack when the opponent has no monsters
    // and our combined ATK meets/exceeds their life points. 11091375 =
    // Luster Dragon (ATK 1900, verified against cards.cdb).
    {
        BattleCommandRequest request;
        request.player = 0;
        request.attackable.push_back(FieldCard{11091375, 0, LOCATION_MZONE, 0});
        request.can_end_phase = true;
        DuelObservation observation{}; observation.self_player = 0;
        observation.opponent_life = 100; // Trivially low: Luster Dragon's 1900 ATK clears it undefended.
        auto response = agent.choose(observation, request);
        assert(decode_type(response) == battle_action::Attack);
    }

    // --- Battle: doesn't attack into a known monster stronger than
    // anything we have, when there's no lethal to chase. 10071456 =
    // Protector of the Throne (ATK 800); 89631139 = Blue-Eyes White Dragon
    // (ATK 3000) — both verified against cards.cdb.
    {
        BattleCommandRequest request;
        request.player = 0;
        request.attackable.push_back(FieldCard{10071456, 0, LOCATION_MZONE, 0});
        request.can_end_phase = true;
        DuelObservation observation{}; observation.self_player = 0;
        observation.opponent_life = 8000;
        observation.opponent_monsters[0] = {true, 89631139, POS_FACEUP_ATTACK, 0};
        auto response = agent.choose(observation, request);
        assert(decode_type(response) == battle_action::EnterEndPhase || decode_type(response) == battle_action::EnterMainPhase2);
    }

    return 0;
}
