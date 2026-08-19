#include "DeckArchetype.hpp"

#include <algorithm>

#include "BurnExecutor.hpp"
#include "ChaosControlExecutor.hpp"
#include "GearfriedExecutor.hpp"
#include "GenericGoatExecutors.hpp"

namespace goat::ai {
namespace {

bool contains(const std::vector<uint32_t>& codes, uint32_t code) {
    return std::find(codes.begin(), codes.end(), code) != codes.end();
}

} // namespace

DeckArchetype detect_deck_archetype(const std::vector<uint32_t>& main_deck_codes) {
    // Chaos Control and Chaos Turbo share the same core engine (BLS-Envoy /
    // Chaos Sorcerer), so either signature card routes to the one Chaos
    // executor table.
    if (contains(main_deck_codes, goat_card::BlackLusterSoldierEnvoy) || contains(main_deck_codes, goat_card::ChaosSorcerer))
        return DeckArchetype::ChaosControl;
    if (contains(main_deck_codes, goat_card::GearfriedTheIronKnight))
        return DeckArchetype::Gearfried;
    // Any two of these direct-damage staples is a reliable enough signal —
    // a deck that just happens to run one of them for another reason
    // (unlikely in this pool, but not impossible) shouldn't get diverted
    // into full stall mode.
    int burn_signals = 0;
    for (uint32_t code : {goat_card::JustDesserts, goat_card::SecretBarrel, goat_card::Ceasefire, goat_card::OjamaTrio})
        if (contains(main_deck_codes, code)) ++burn_signals;
    if (burn_signals >= 2) return DeckArchetype::Burn;
    return DeckArchetype::Generic;
}

ExecutorList build_executors_for_deck(const std::vector<uint32_t>& main_deck_codes) {
    switch (detect_deck_archetype(main_deck_codes)) {
        case DeckArchetype::ChaosControl: return build_chaos_control_executors();
        case DeckArchetype::Gearfried: return build_gearfried_executors();
        case DeckArchetype::Burn: return build_burn_executors();
        case DeckArchetype::Generic: break;
    }
    return build_generic_goat_executors();
}

} // namespace goat::ai
