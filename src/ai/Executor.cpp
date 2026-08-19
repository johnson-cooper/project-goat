#include "Executor.hpp"

namespace goat::ai {
namespace {

bool matches(const Executor& executor, uint32_t code) {
    return !executor.card_code.has_value() || *executor.card_code == code;
}

bool accepts(const Executor& executor, const ExecutorContext& context) {
    return !executor.condition || executor.condition(context);
}

} // namespace

void ExecutorList::add(ExecutorType type, uint32_t card_code, std::function<bool(const ExecutorContext&)> condition) {
    executors_.push_back(Executor{type, card_code, std::move(condition)});
}

void ExecutorList::add(ExecutorType type, std::function<bool(const ExecutorContext&)> condition) {
    executors_.push_back(Executor{type, std::nullopt, std::move(condition)});
}

std::optional<IdleActionChoice> evaluate_idle_executors(
    const ExecutorList& executors,
    const IdleCommandRequest& request,
    goat::CardDatabase& database,
    const DuelObservation& observation) {
    for (const auto& executor : executors.entries()) {
        switch (executor.type) {
            case ExecutorType::EnterBattlePhase:
                if (request.can_battle_phase && accepts(executor, ExecutorContext{database, observation, FieldCard{}, 0}))
                    return IdleActionChoice{ExecutorType::EnterBattlePhase, 0};
                break;
            case ExecutorType::EndPhase:
                if (request.can_end_phase && accepts(executor, ExecutorContext{database, observation, FieldCard{}, 0}))
                    return IdleActionChoice{ExecutorType::EndPhase, 0};
                break;
            case ExecutorType::EnterMainPhase2:
                break; // Not applicable to the idle-command prompt.
            case ExecutorType::Activate: {
                for (size_t i = 0; i < request.activatable.size(); ++i) {
                    const auto& candidate = request.activatable[i];
                    if (!matches(executor, candidate.card.code)) continue;
                    if (accepts(executor, ExecutorContext{database, observation, candidate.card, candidate.description}))
                        return IdleActionChoice{ExecutorType::Activate, i};
                }
                break;
            }
            case ExecutorType::Summon:
            case ExecutorType::SpecialSummon:
            case ExecutorType::Reposition:
            case ExecutorType::SetMonster:
            case ExecutorType::SetSpellTrap: {
                const std::vector<FieldCard>* candidates = nullptr;
                switch (executor.type) {
                    case ExecutorType::Summon: candidates = &request.summonable; break;
                    case ExecutorType::SpecialSummon: candidates = &request.special_summonable; break;
                    case ExecutorType::Reposition: candidates = &request.repositionable; break;
                    case ExecutorType::SetMonster: candidates = &request.monster_setable; break;
                    case ExecutorType::SetSpellTrap: candidates = &request.spell_setable; break;
                    default: break;
                }
                for (size_t i = 0; i < candidates->size(); ++i) {
                    const auto& candidate = (*candidates)[i];
                    if (!matches(executor, candidate.code)) continue;
                    if (accepts(executor, ExecutorContext{database, observation, candidate, 0}))
                        return IdleActionChoice{executor.type, i};
                }
                break;
            }
        }
    }
    return std::nullopt;
}

std::optional<size_t> evaluate_activate_executors(
    const ExecutorList& executors,
    const std::vector<ActivatableCard>& activatable,
    goat::CardDatabase& database,
    const DuelObservation& observation) {
    for (const auto& executor : executors.entries()) {
        if (executor.type != ExecutorType::Activate) continue;
        for (size_t i = 0; i < activatable.size(); ++i) {
            const auto& candidate = activatable[i];
            if (!matches(executor, candidate.card.code)) continue;
            if (accepts(executor, ExecutorContext{database, observation, candidate.card, candidate.description}))
                return i;
        }
    }
    return std::nullopt;
}

} // namespace goat::ai
