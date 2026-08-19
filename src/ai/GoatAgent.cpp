#include "GoatAgent.hpp"

#include "BattleEvaluator.hpp"
#include "CardSelector.hpp"

namespace goat::ai {
namespace {

DecisionResponse encode_index_vector(const std::vector<size_t>& indices) {
    std::vector<uint32_t> as_u32(indices.begin(), indices.end());
    return encode_indices(as_u32);
}

} // namespace

GoatAgent::GoatAgent(goat::CardDatabase& database, ExecutorList executors, Difficulty difficulty, uint64_t seed)
    : database_(database), executors_(std::move(executors)), difficulty_(difficulty), random_(seed), fallback_(database) {}

DecisionResponse GoatAgent::choose(const DuelObservation& observation, const DecisionRequest& request) {
    if (difficulty_ == Difficulty::Easy) return fallback_.choose(observation, request);

    if (const auto* idle = std::get_if<IdleCommandRequest>(&request)) {
        if (auto choice = evaluate_idle_executors(executors_, *idle, database_, observation)) {
            // Every category that names a specific card is capped by
            // ActivationGuard, not just Activate: a card whose Special
            // Summon (etc.) requires optional material selection that the
            // fallback selection logic declines can otherwise be re-offered
            // and re-chosen forever with no forward progress — the same
            // loop-breaker concept as WindBot's `_activatedCards` cap,
            // applied to every idle-command category, not only activations.
            uint32_t code = 0;
            bool has_code = true;
            switch (choice->type) {
                case ExecutorType::Activate: code = idle->activatable[choice->index].card.code; break;
                case ExecutorType::Summon: code = idle->summonable[choice->index].code; break;
                case ExecutorType::SpecialSummon: code = idle->special_summonable[choice->index].code; break;
                case ExecutorType::Reposition: code = idle->repositionable[choice->index].code; break;
                case ExecutorType::SetMonster: code = idle->monster_setable[choice->index].code; break;
                case ExecutorType::SetSpellTrap: code = idle->spell_setable[choice->index].code; break;
                case ExecutorType::EnterBattlePhase:
                case ExecutorType::EndPhase:
                case ExecutorType::EnterMainPhase2:
                    has_code = false; break; // No card identity to cap; always allowed.
            }
            if (!has_code || guard_.allowed(code)) {
                if (has_code) guard_.record(code);
                switch (choice->type) {
                    case ExecutorType::Activate: return encode_type_index(idle_action::Activate, static_cast<uint16_t>(choice->index));
                    case ExecutorType::Summon: return encode_type_index(idle_action::Summon, static_cast<uint16_t>(choice->index));
                    case ExecutorType::SpecialSummon: return encode_type_index(idle_action::SpecialSummon, static_cast<uint16_t>(choice->index));
                    case ExecutorType::Reposition: return encode_type_index(idle_action::Reposition, static_cast<uint16_t>(choice->index));
                    case ExecutorType::SetMonster: return encode_type_index(idle_action::SetMonster, static_cast<uint16_t>(choice->index));
                    case ExecutorType::SetSpellTrap: return encode_type_index(idle_action::SetSpellTrap, static_cast<uint16_t>(choice->index));
                    case ExecutorType::EnterBattlePhase: return encode_type_index(idle_action::EnterBattlePhase, 0);
                    case ExecutorType::EndPhase: return encode_type_index(idle_action::EnterEndPhase, 0);
                    case ExecutorType::EnterMainPhase2: break; // Not applicable to this prompt.
                }
            }
            // Over-tried this duel: fall through to the generic fallback below.
        }
        return fallback_.choose(observation, request);
    }

    if (const auto* battle = std::get_if<BattleCommandRequest>(&request)) {
        if (auto match = evaluate_activate_executors(executors_, battle->activatable, database_, observation)) {
            const auto code = battle->activatable[*match].card.code;
            if (guard_.allowed(code)) {
                guard_.record(code);
                return encode_type_index(battle_action::Activate, static_cast<uint16_t>(*match));
            }
        }
        if (auto attacker = choose_attacker(database_, observation, battle->attackable))
            return encode_type_index(battle_action::Attack, static_cast<uint16_t>(*attacker));
        if (battle->can_main_phase_2) return encode_type_index(battle_action::EnterMainPhase2, 0);
        return encode_type_index(battle_action::EnterEndPhase, 0);
    }

    if (const auto* chain = std::get_if<SelectChainRequest>(&request)) {
        if (chain->candidates.empty()) return encode_raw(-1);
        std::vector<ActivatableCard> activatable;
        activatable.reserve(chain->candidates.size());
        for (const auto& link : chain->candidates) activatable.push_back(ActivatableCard{link.card, link.description, 0});
        if (auto match = evaluate_activate_executors(executors_, activatable, database_, observation)) {
            const auto code = activatable[*match].card.code;
            if (guard_.allowed(code)) {
                guard_.record(code);
                return encode_raw(static_cast<int32_t>(*match));
            }
        }
        if (chain->forced) return encode_raw(0);
        return encode_raw(-1);
    }

    if (const auto* effect_yn = std::get_if<SelectEffectYesNoRequest>(&request)) {
        std::vector<ActivatableCard> activatable{ActivatableCard{effect_yn->card, effect_yn->description, 0}};
        if (evaluate_activate_executors(executors_, activatable, database_, observation)) return encode_raw(1);
        return encode_raw(0);
    }

    if (const auto* tribute = std::get_if<SelectTributeRequest>(&request)) {
        if (tribute->min == 0) return encode_index_vector({});
        return encode_index_vector(rank_worst(database_, tribute->candidates, tribute->min));
    }

    if (const auto* select_card = std::get_if<SelectCardRequest>(&request)) {
        if (select_card->min == 0 || select_card->candidates.empty()) return encode_index_vector({});
        return encode_index_vector(rank_removal_targets(database_, select_card->candidates, observation.self_player, select_card->min));
    }

    // Everything else (yes/no, effect option, place, position, counter, sum,
    // select-unselect, sort) has no staple-specific behavior in this pass —
    // fall back to the same conservative, always-legal defaults RandomAgent
    // uses.
    return fallback_.choose(observation, request);
}

} // namespace goat::ai
