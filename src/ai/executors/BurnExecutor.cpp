#include "BurnExecutor.hpp"

#include "../Heuristics.hpp"
#include "GenericGoatExecutors.hpp"

namespace goat::ai {
namespace {

bool has_open_opponent_monster_zones(const ExecutorContext& context) {
    // Ojama Trio floods three of the opponent's monster zones with tokens —
    // only useful while at least a couple are actually open to flood.
    size_t occupied = 0;
    for (const auto& monster : context.observation.opponent_monsters) if (monster.occupied) ++occupied;
    return occupied <= 3; // GOAT has 5 monster zones; leave room for the 3 tokens to matter.
}

bool skill_drain_worth_it(const ExecutorContext& context) {
    // A blunt approximation: only worth neutering every monster's effects
    // (including our own, of which this shell runs almost none) when the
    // opponent currently has the better board.
    return is_one_enemy_better(context.database, context.observation);
}

bool nightmare_wheel_worth_it(const ExecutorContext& context) {
    return should_fire_reactive_trap(context.observation) && is_one_enemy_better(context.database, context.observation);
}

bool magic_cylinder_worth_it(const ExecutorContext& context) {
    return should_fire_reactive_trap(context.observation);
}

} // namespace

ExecutorList build_burn_executors() {
    ExecutorList list;

    // Direct-damage staples: always worth activating once legal — this
    // shell wins by adding these up, not by attacking.
    list.add(ExecutorType::Activate, goat_card::JustDesserts);
    list.add(ExecutorType::Activate, goat_card::SecretBarrel);
    list.add(ExecutorType::Activate, goat_card::Ceasefire);

    // Reactive/targeted burn and lockdown pieces.
    list.add(ExecutorType::Activate, goat_card::MagicCylinder, magic_cylinder_worth_it);
    list.add(ExecutorType::Activate, goat_card::NightmareWheel, nightmare_wheel_worth_it);
    list.add(ExecutorType::Activate, goat_card::OjamaTrio, has_open_opponent_monster_zones);
    list.add(ExecutorType::Activate, goat_card::SkillDrain, skill_drain_worth_it);

    // Stall: this shell runs almost no real attackers, so slowing the
    // opponent down is close to always correct once legal.
    list.add(ExecutorType::Activate, goat_card::SwordsOfRevealingLight);
    list.add(ExecutorType::Activate, goat_card::WallOfRevealingLight);

    add_generic_goat_rules(list);
    return list;
}

} // namespace goat::ai
