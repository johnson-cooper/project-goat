#include "GenericGoatExecutors.hpp"

#include <algorithm>

#include "../CardEvaluator.hpp"
#include "../Heuristics.hpp"

extern "C" {
#include "ocgapi_constants.h"
}

namespace goat::ai {
namespace {

bool default_heavy_storm(const ExecutorContext& context) {
    // Port of WindBot's DefaultHeavyStorm: only blow up the field when the
    // opponent's backrow outnumbers ours — never trade our own better
    // backrow away just because Heavy Storm happens to be legal.
    size_t own_backrow = 0, opponent_backrow = 0;
    for (const auto& spell : context.observation.own_spells) if (spell.occupied) ++own_backrow;
    for (const auto& spell : context.observation.opponent_spells) if (spell.occupied) ++opponent_backrow;
    return opponent_backrow > own_backrow;
}

bool default_mystical_space_typhoon(const ExecutorContext& context) {
    for (const auto& spell : context.observation.opponent_spells) if (spell.occupied) return true;
    return false;
}

bool default_reactive_trap(const ExecutorContext& context) {
    return should_fire_reactive_trap(context.observation) && is_all_enemy_better(context.database, context.observation);
}

bool default_single_removal_trap(const ExecutorContext& context) {
    return should_fire_reactive_trap(context.observation) && is_one_enemy_better(context.database, context.observation);
}

bool default_call_of_the_haunted(const ExecutorContext& context) {
    // Port of WindBot's DefaultCallOfTheHaunted: revive only when we're
    // currently outclassed on board (the actual grave target is chosen
    // later, by CardSelector, from whatever the engine's SelectCardRequest
    // actually offers).
    return is_all_enemy_better(context.database, context.observation);
}

bool default_book_of_moon(const ExecutorContext& context) {
    return is_all_enemy_better(context.database, context.observation);
}

bool default_monster_summon(const ExecutorContext& context) {
    auto& definition = context.database.resolve(context.card.code);
    if ((definition.level & 0xffu) <= 4) return true; // No tribute required.
    // Tribute summon: only worthwhile if we have a monster to spare whose
    // own effective power is lower than the incoming monster's ATK.
    for (const auto& monster : context.observation.own_monsters) {
        if (!monster.occupied || monster.code == 0) continue;
        if (effective_power(context.database, monster.code, monster.position) < definition.attack) return true;
    }
    return false;
}

bool default_monster_reposition(const ExecutorContext& context) {
    if (context.card.location != LOCATION_MZONE || context.card.sequence >= context.observation.own_monsters.size()) return false;
    const auto& on_field = context.observation.own_monsters[context.card.sequence];
    if (!on_field.occupied) return false;
    auto& definition = context.database.resolve(context.card.code);
    int32_t opponent_best = 0;
    for (const auto& monster : context.observation.opponent_monsters)
        if (monster.occupied && monster.code) opponent_best = std::max(opponent_best, effective_power(context.database, monster.code, monster.position));
    const bool currently_attack = (on_field.position & (POS_FACEUP_ATTACK | POS_FACEDOWN_ATTACK)) != 0;
    if (currently_attack) {
        // Flip to defense only when defense is actually the safer stat and
        // it's high enough to survive the opponent's best known threat.
        return definition.defense > definition.attack && definition.defense >= opponent_best;
    }
    // Currently defense: flip up to attack only when attack is the better
    // stat and nothing known on the opponent's side would punish it.
    return definition.attack >= definition.defense && definition.attack > opponent_best;
}

bool has_any_own_monster(const ExecutorContext& context) {
    for (const auto& monster : context.observation.own_monsters) if (monster.occupied) return true;
    return false;
}

} // namespace

void add_generic_goat_rules(ExecutorList& list) {
    // Reactive board-wipes and hard counters: checked first, matching
    // WindBot's convention of registering removal/hard-counters ahead of
    // everything else.
    list.add(ExecutorType::Activate, goat_card::TorrentialTribute, default_reactive_trap);
    list.add(ExecutorType::Activate, goat_card::MirrorForce, default_reactive_trap);
    list.add(ExecutorType::Activate, goat_card::SakuretsuArmor, default_single_removal_trap);
    list.add(ExecutorType::Activate, goat_card::CallOfTheHaunted, default_call_of_the_haunted);
    list.add(ExecutorType::Activate, goat_card::BookOfMoon, default_book_of_moon);

    // Backrow-aware board control.
    list.add(ExecutorType::Activate, goat_card::HeavyStorm, default_heavy_storm);
    list.add(ExecutorType::Activate, goat_card::MysticalSpaceTyphoon, default_mystical_space_typhoon);

    // Advantage staples: unconditional once the engine says they're legal.
    list.add(ExecutorType::Activate, goat_card::PotOfGreed);
    list.add(ExecutorType::Activate, goat_card::GracefulCharity);

    // Generic fallbacks (WindBot's DefaultExecutor-equivalent policy).
    list.add(ExecutorType::Summon, default_monster_summon);
    list.add(ExecutorType::SpecialSummon);
    list.add(ExecutorType::Reposition, default_monster_reposition);
    list.add(ExecutorType::SetMonster);
    list.add(ExecutorType::SetSpellTrap);
    list.add(ExecutorType::EnterBattlePhase, has_any_own_monster);
    list.add(ExecutorType::EndPhase);
}

ExecutorList build_generic_goat_executors() {
    ExecutorList list;
    add_generic_goat_rules(list);
    return list;
}

} // namespace goat::ai
