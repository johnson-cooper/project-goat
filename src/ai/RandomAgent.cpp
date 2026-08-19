#include "RandomAgent.hpp"

#include <algorithm>
#include <stdexcept>

#include "AnnounceCardSolver.hpp"

extern "C" {
#include "ocgapi_constants.h"
}

namespace goat::ai {
namespace {

template <class... Ts> struct overload : Ts... { using Ts::operator()...; };
template <class... Ts> overload(Ts...) -> overload<Ts...>;

// Greedily takes the first `count` legal bits out of `available`, for
// MSG_ANNOUNCE_RACE/MSG_ANNOUNCE_ATTRIB — always a legal declaration since
// it only ever selects bits the engine itself offered.
uint64_t take_first_n_bits(uint64_t available, uint8_t count) {
    uint64_t taken = 0;
    for (int bit = 0; bit < 64 && count > 0; ++bit) {
        const uint64_t mask = uint64_t(1) << bit;
        if (available & mask) { taken |= mask; --count; }
    }
    return taken;
}

} // namespace

DecisionResponse RandomAgent::choose(const DuelObservation&, const DecisionRequest& request) {
    return std::visit(overload{
        [&](const IdleCommandRequest& r) -> DecisionResponse {
            for (size_t i = 0; i < r.summonable.size(); ++i)
                if ((database_.resolve(r.summonable[i].code).level & 0xffu) <= 4)
                    return encode_type_index(idle_action::Summon, static_cast<uint16_t>(i));
            if (r.can_battle_phase) return encode_type_index(idle_action::EnterBattlePhase, 0);
            if (r.can_end_phase) return encode_type_index(idle_action::EnterEndPhase, 0);
            return encode_type_index(idle_action::Pass, 0);
        },
        [&](const BattleCommandRequest& r) -> DecisionResponse {
            return encode_type_index(r.attackable.empty() ? battle_action::EnterEndPhase : battle_action::Attack, 0);
        },
        [&](const SelectChainRequest& r) -> DecisionResponse {
            if (r.forced && !r.candidates.empty()) return encode_raw(0);
            return encode_raw(-1);
        },
        [&](const SelectYesNoRequest&) -> DecisionResponse { return encode_raw(0); },
        [&](const SelectEffectYesNoRequest&) -> DecisionResponse { return encode_raw(0); },
        [&](const SortRequest&) -> DecisionResponse { return encode_raw(-1); },
        [&](const SelectOptionRequest&) -> DecisionResponse { return encode_raw(0); },
        [&](const SelectPlaceRequest& r) -> DecisionResponse {
            if (r.available_sequences.empty()) throw std::runtime_error("no legal zone in place prompt");
            return encode_place(r.target_player, r.location, r.available_sequences.front());
        },
        [&](const SelectTributeRequest& r) -> DecisionResponse { return encode_first_n(r.min); },
        [&](const SelectCardRequest& r) -> DecisionResponse { return encode_first_n(r.min); },
        [&](const SelectPositionRequest& r) -> DecisionResponse {
            for (uint8_t pos : {uint8_t(POS_FACEUP_ATTACK), uint8_t(POS_FACEUP_DEFENSE), uint8_t(POS_FACEDOWN_DEFENSE), uint8_t(POS_FACEDOWN_ATTACK)})
                if (r.positions & pos) return encode_raw(pos);
            throw std::runtime_error("no legal position in position prompt");
        },
        [&](const SelectCounterRequest& r) -> DecisionResponse {
            std::vector<uint16_t> take(r.available.size(), 0);
            uint16_t remaining = r.count;
            for (size_t i = 0; i < r.available.size() && remaining > 0; ++i) {
                const uint16_t use = std::min(r.available[i], remaining);
                take[i] = use;
                remaining -= use;
            }
            return encode_counter_amounts(take);
        },
        [&](const SelectSumRequest& r) -> DecisionResponse {
            std::vector<uint32_t> indices;
            uint32_t running = 0;
            for (const auto sum : r.must_select_params) running += (sum & 0xffffu);
            for (size_t i = 0; i < r.must_select.size(); ++i) indices.push_back(static_cast<uint32_t>(i));
            for (size_t i = 0; i < r.optional.size(); ++i) {
                if (running < r.max) {
                    indices.push_back(static_cast<uint32_t>(r.must_select.size() + i));
                    running += (r.optional_params[i] & 0xffffu);
                }
            }
            return encode_indices(indices);
        },
        [&](const SelectUnselectCardRequest& r) -> DecisionResponse {
            if (r.finishable || r.cancelable) return encode_select_unselect(-1, 0);
            if (!r.selectable.empty()) return encode_select_unselect(1, 0);
            return encode_select_unselect(-1, 0);
        },
        [&](const AnnounceRaceRequest& r) -> DecisionResponse { return encode_raw64(take_first_n_bits(r.available, r.count)); },
        [&](const AnnounceAttributeRequest& r) -> DecisionResponse { return encode_raw(static_cast<int32_t>(take_first_n_bits(r.available, r.count))); },
        [&](const AnnounceCardRequest& r) -> DecisionResponse {
            const auto code = find_declarable_card(database_, r.predicate_opcodes);
            if (!code) throw std::runtime_error("no card in the pinned pool satisfies this declare-a-card-name prompt");
            return encode_raw(static_cast<int32_t>(*code));
        },
        [&](const AnnounceNumberRequest& r) -> DecisionResponse {
            if (r.options.empty()) throw std::runtime_error("empty announce-number prompt");
            return encode_raw(0);
        },
    }, request);
}

} // namespace goat::ai
