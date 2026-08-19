#include "ChaosControlExecutor.hpp"

#include "GenericGoatExecutors.hpp"

namespace goat::ai {

ExecutorList build_chaos_control_executors() {
    ExecutorList list;

    // The deck's whole win condition: the moment the engine offers either
    // Chaos monster as a legal Special Summon (i.e. the banish-from-GY cost
    // is already payable), take it — there's essentially never a reason to
    // hold a hard-once-per-turn power play like this back. Registered as
    // both SpecialSummon and Activate since this project's wire format
    // doesn't guarantee which idle-command category a given card's
    // Special-Summon-from-hand effect surfaces under; whichever one the
    // engine doesn't use for this card simply never matches, harmlessly.
    list.add(ExecutorType::SpecialSummon, goat_card::BlackLusterSoldierEnvoy);
    list.add(ExecutorType::Activate, goat_card::BlackLusterSoldierEnvoy);
    list.add(ExecutorType::SpecialSummon, goat_card::ChaosSorcerer);
    list.add(ExecutorType::Activate, goat_card::ChaosSorcerer);

    // Card Destruction refills the hand and — just as importantly for this
    // archetype — restocks the graveyard with fresh LIGHT/DARK fodder for
    // the Chaos monsters above, ahead of the generic Graceful Charity/Pot of
    // Greed advantage plays.
    list.add(ExecutorType::Activate, goat_card::CardDestruction);

    // A full graveyard-to-field return is a huge swing; the engine only
    // offers it once there's something in the banished pile worth bringing
    // back, so firing it unconditionally once legal is safe.
    list.add(ExecutorType::Activate, goat_card::ReturnFromTheDifferentDimension);

    add_generic_goat_rules(list);
    return list;
}

} // namespace goat::ai
