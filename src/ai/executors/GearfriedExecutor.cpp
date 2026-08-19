#include "GearfriedExecutor.hpp"

#include "../Heuristics.hpp"
#include "GenericGoatExecutors.hpp"

namespace goat::ai {
namespace {

bool exiled_force_worth_it(const ExecutorContext& context) {
    // Exiled Force pays its own body as the cost to destroy one monster —
    // only worth the card disadvantage when the opponent actually has
    // something worth removing.
    return is_one_enemy_better(context.database, context.observation);
}

} // namespace

ExecutorList build_gearfried_executors() {
    ExecutorList list;

    // The deck's card-advantage engine: search whatever Level 4-or-lower
    // Warrior is most useful right now (Gearfried the Iron Knight, D.D.
    // Warrior Lady, Don Zaloog, ...) ahead of the generic advantage staples.
    list.add(ExecutorType::Activate, goat_card::ReinforcementOfTheArmy);

    // A dedicated removal monster: only spend it when the board actually
    // calls for removal, same reactive-timing spirit as the generic traps.
    list.add(ExecutorType::Activate, goat_card::ExiledForce, exiled_force_worth_it);

    add_generic_goat_rules(list);
    return list;
}

} // namespace goat::ai
