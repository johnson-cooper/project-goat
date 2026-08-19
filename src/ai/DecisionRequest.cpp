#include "DecisionRequest.hpp"

#include <cstring>
#include <stdexcept>

extern "C" {
#include "ocgapi_constants.h"
}

namespace goat::ai {
namespace {

template <class T>
T read(const uint8_t*& p, const uint8_t* end) {
    if (static_cast<size_t>(end - p) < sizeof(T)) throw std::runtime_error("truncated engine message");
    T value;
    std::memcpy(&value, p, sizeof(T));
    p += sizeof(T);
    return value;
}

// Same (controller, location, sequence) triple used by every idle-command
// candidate group and the activation-candidate lists — see
// RandomAgent::choose_human_idle in src/main.cpp for the reference parse this
// mirrors.
FieldCard read_field_card32(const uint8_t*& p, const uint8_t* end) {
    FieldCard card;
    card.code = read<uint32_t>(p, end);
    card.controller = read<uint8_t>(p, end);
    card.location = read<uint8_t>(p, end);
    card.sequence = read<uint32_t>(p, end);
    return card;
}

// The Reposition candidate group is the one exception: its third field is a
// single byte, not a uint32_t (matches choose_human_idle's `kind==2` branch).
FieldCard read_field_card8(const uint8_t*& p, const uint8_t* end) {
    FieldCard card;
    card.code = read<uint32_t>(p, end);
    card.controller = read<uint8_t>(p, end);
    card.location = read<uint8_t>(p, end);
    card.sequence = read<uint8_t>(p, end);
    return card;
}

// Several prompts (select chain, select effect-yes/no, select card, select
// sum, select-unselect) carry the *full* engine loc_info shape — controller,
// location, sequence, and a trailing position field — one field wider than
// the abbreviated triple idle-command candidates use. Verified against each
// kind's exact read sequence in RandomAgent::choose (src/main.cpp).
FieldCard read_field_card_full(const uint8_t*& p, const uint8_t* end) {
    FieldCard card = read_field_card32(p, end);
    card.position = read<uint32_t>(p, end);
    return card;
}

ActivatableCard read_activatable(const uint8_t*& p, const uint8_t* end) {
    ActivatableCard activatable;
    activatable.card = read_field_card32(p, end);
    activatable.description = read<uint64_t>(p, end);
    activatable.client_mode = read<uint8_t>(p, end);
    return activatable;
}

IdleCommandRequest parse_idle_command(const uint8_t* p, const uint8_t* end) {
    IdleCommandRequest request;
    request.player = read<uint8_t>(p, end);
    for (uint16_t group = 0; group < 5; ++group) {
        const auto count = read<uint32_t>(p, end);
        for (uint32_t i = 0; i < count; ++i) {
            switch (group) {
                case 0: request.summonable.push_back(read_field_card32(p, end)); break;
                case 1: request.special_summonable.push_back(read_field_card32(p, end)); break;
                case 2: request.repositionable.push_back(read_field_card8(p, end)); break;
                case 3: request.monster_setable.push_back(read_field_card32(p, end)); break;
                case 4: request.spell_setable.push_back(read_field_card32(p, end)); break;
            }
        }
    }
    const auto activations = read<uint32_t>(p, end);
    request.activatable.reserve(activations);
    for (uint32_t i = 0; i < activations; ++i) request.activatable.push_back(read_activatable(p, end));
    request.can_battle_phase = read<uint8_t>(p, end) != 0;
    request.can_end_phase = read<uint8_t>(p, end) != 0;
    return request;
}

BattleCommandRequest parse_battle_command(const uint8_t* p, const uint8_t* end) {
    BattleCommandRequest request;
    request.player = read<uint8_t>(p, end);
    const auto effects = read<uint32_t>(p, end);
    request.activatable.reserve(effects);
    for (uint32_t i = 0; i < effects; ++i) request.activatable.push_back(read_activatable(p, end));
    const auto attackers = read<uint32_t>(p, end);
    request.attackable.reserve(attackers);
    for (uint32_t i = 0; i < attackers; ++i) {
        // choose_human_battle: code(u32) + 4 single-byte fields (controller,
        // location, sequence, direct-attackable flag) — only the first three
        // are useful for identifying which of our monsters this candidate is.
        FieldCard card;
        card.code = read<uint32_t>(p, end);
        card.controller = read<uint8_t>(p, end);
        card.location = read<uint8_t>(p, end);
        card.sequence = read<uint8_t>(p, end);
        read<uint8_t>(p, end);
        request.attackable.push_back(card);
    }
    request.can_main_phase_2 = read<uint8_t>(p, end) != 0;
    request.can_end_phase = read<uint8_t>(p, end) != 0;
    return request;
}

SelectChainRequest parse_select_chain(const uint8_t* p, const uint8_t* end) {
    SelectChainRequest request;
    request.player = read<uint8_t>(p, end);
    read<uint8_t>(p, end); // spe_count: engine-side chain-limit metadata, not needed here.
    request.forced = read<uint8_t>(p, end) != 0;
    read<uint32_t>(p, end);
    read<uint32_t>(p, end); // hint_timing for each player: UI hint metadata only.
    const auto count = read<uint32_t>(p, end);
    request.candidates.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        SelectChainRequest::ChainLink link;
        link.card = read_field_card_full(p, end);
        link.description = read<uint64_t>(p, end);
        read<uint8_t>(p, end); // client mode
        request.candidates.push_back(link);
    }
    return request;
}

SelectYesNoRequest parse_select_yesno(const uint8_t* p, const uint8_t* end) {
    SelectYesNoRequest request;
    request.player = read<uint8_t>(p, end);
    read<uint64_t>(p, end); // description: no client-side lookup table available.
    return request;
}

SelectEffectYesNoRequest parse_select_effectyn(const uint8_t* p, const uint8_t* end) {
    SelectEffectYesNoRequest request;
    request.player = read<uint8_t>(p, end);
    request.card = read_field_card_full(p, end);
    request.description = read<uint64_t>(p, end);
    return request;
}

SortRequest parse_sort(bool is_chain_order) {
    // No verified byte layout exists for this prompt (see the comment on
    // SortRequest) — consume nothing, matching the always-safe "-1" decline.
    return SortRequest{is_chain_order};
}

SelectOptionRequest parse_select_option(const uint8_t* p, const uint8_t* end) {
    SelectOptionRequest request;
    request.player = read<uint8_t>(p, end);
    const auto count = read<uint8_t>(p, end);
    request.option_descriptions.reserve(count);
    for (uint8_t i = 0; i < count; ++i) request.option_descriptions.push_back(read<uint64_t>(p, end));
    if (request.option_descriptions.empty()) throw std::runtime_error("empty option-selection prompt");
    return request;
}

SelectPlaceRequest parse_select_place(const uint8_t* p, const uint8_t* end) {
    const auto player = read<uint8_t>(p, end);
    const auto count = read<uint8_t>(p, end);
    const auto flag = read<uint32_t>(p, end);
    if (count != 1) throw std::runtime_error("multi-place selection is not supported by Phase 1");
    // See RandomAgent's MSG_SELECT_PLACE handling in src/main.cpp for the
    // inverted-flag range-detection rationale (mirrors
    // field::process(Processors::SelectPlace&) in playerop.cpp).
    const uint32_t detect = ~flag;
    SelectPlaceRequest request;
    request.player = player;
    uint32_t blocked;
    if (detect & 0x7fu) { request.target_player = player; request.location = LOCATION_MZONE; blocked = flag & 0x7fu; }
    else if (detect & 0x1f00u) { request.target_player = player; request.location = LOCATION_SZONE; blocked = (flag >> 8) & 0x1fu; }
    else if (detect & 0x7f0000u) { request.target_player = static_cast<uint8_t>(1 - player); request.location = LOCATION_MZONE; blocked = (flag >> 16) & 0x7fu; }
    else if (detect & 0x1f000000u) { request.target_player = static_cast<uint8_t>(1 - player); request.location = LOCATION_SZONE; blocked = (flag >> 24) & 0x1fu; }
    else throw std::runtime_error("unsupported zone-placement flag (pendulum zone?)");
    for (uint8_t seq = 0; seq < 5; ++seq) if ((blocked & (1u << seq)) == 0) request.available_sequences.push_back(seq);
    return request;
}

SelectTributeRequest parse_select_tribute(const uint8_t* p, const uint8_t* end) {
    SelectTributeRequest request;
    request.player = read<uint8_t>(p, end);
    read<uint8_t>(p, end);
    request.min = read<uint32_t>(p, end);
    request.max = read<uint32_t>(p, end);
    const auto choices = read<uint32_t>(p, end);
    if (request.min > choices) throw std::runtime_error("invalid tribute-selection prompt");
    request.candidates.reserve(choices);
    for (uint32_t i = 0; i < choices; ++i) {
        FieldCard card = read_field_card32(p, end);
        read<uint8_t>(p, end); // release_param: cost-payment metadata, not needed for candidate identity.
        request.candidates.push_back(card);
    }
    return request;
}

SelectCardRequest parse_select_card(const uint8_t* p, const uint8_t* end) {
    SelectCardRequest request;
    request.player = read<uint8_t>(p, end);
    read<uint8_t>(p, end);
    request.min = read<uint32_t>(p, end);
    request.max = read<uint32_t>(p, end);
    const auto choices = read<uint32_t>(p, end);
    if (request.min > choices) throw std::runtime_error("invalid card-selection prompt");
    request.candidates.reserve(choices);
    for (uint32_t i = 0; i < choices; ++i) request.candidates.push_back(read_field_card_full(p, end));
    return request;
}

SelectPositionRequest parse_select_position(const uint8_t* p, const uint8_t* end) {
    SelectPositionRequest request;
    request.player = read<uint8_t>(p, end);
    request.code = read<uint32_t>(p, end);
    request.positions = read<uint8_t>(p, end);
    return request;
}

SelectCounterRequest parse_select_counter(const uint8_t* p, const uint8_t* end) {
    SelectCounterRequest request;
    read<uint8_t>(p, end); // player: this prompt's amounts apply regardless of who answers it.
    request.counter_type = read<uint16_t>(p, end);
    request.count = read<uint16_t>(p, end);
    const auto card_count = read<uint32_t>(p, end);
    request.candidates.reserve(card_count);
    request.available.reserve(card_count);
    for (uint32_t i = 0; i < card_count; ++i) {
        FieldCard card;
        card.code = read<uint32_t>(p, end);
        card.controller = read<uint8_t>(p, end);
        card.location = read<uint8_t>(p, end);
        read<uint8_t>(p, end);
        request.candidates.push_back(card);
        request.available.push_back(read<uint16_t>(p, end));
    }
    return request;
}

SelectSumRequest parse_select_sum(const uint8_t* p, const uint8_t* end) {
    SelectSumRequest request;
    request.player = read<uint8_t>(p, end);
    request.exact_mode = read<uint8_t>(p, end) != 0;
    read<uint32_t>(p, end); // target sum: informational, the [min,max] window below is what legality actually requires.
    request.min = read<uint32_t>(p, end);
    request.max = read<uint32_t>(p, end);
    const auto must_count = read<uint32_t>(p, end);
    request.must_select.reserve(must_count);
    request.must_select_params.reserve(must_count);
    for (uint32_t i = 0; i < must_count; ++i) {
        request.must_select.push_back(read_field_card_full(p, end));
        request.must_select_params.push_back(read<uint32_t>(p, end));
    }
    const auto optional_count = read<uint32_t>(p, end);
    request.optional.reserve(optional_count);
    request.optional_params.reserve(optional_count);
    for (uint32_t i = 0; i < optional_count; ++i) {
        request.optional.push_back(read_field_card_full(p, end));
        request.optional_params.push_back(read<uint32_t>(p, end));
    }
    return request;
}

AnnounceRaceRequest parse_announce_race(const uint8_t* p, const uint8_t* end) {
    // field::process(Processors::AnnounceRace&), external/ygopro-core/playerop.cpp.
    AnnounceRaceRequest request;
    request.player = read<uint8_t>(p, end);
    request.count = read<uint8_t>(p, end);
    request.available = read<uint64_t>(p, end);
    return request;
}

AnnounceAttributeRequest parse_announce_attribute(const uint8_t* p, const uint8_t* end) {
    // field::process(Processors::AnnounceAttribute&), same file.
    AnnounceAttributeRequest request;
    request.player = read<uint8_t>(p, end);
    request.count = read<uint8_t>(p, end);
    request.available = read<uint32_t>(p, end);
    return request;
}

AnnounceCardRequest parse_announce_card(const uint8_t* p, const uint8_t* end) {
    // field::process(Processors::AnnounceCard&): player, option count (u8),
    // then that many u64 predicate opcodes.
    AnnounceCardRequest request;
    request.player = read<uint8_t>(p, end);
    const auto count = read<uint8_t>(p, end);
    request.predicate_opcodes.reserve(count);
    for (uint8_t i = 0; i < count; ++i) request.predicate_opcodes.push_back(read<uint64_t>(p, end));
    return request;
}

AnnounceNumberRequest parse_announce_number(const uint8_t* p, const uint8_t* end) {
    // field::process(Processors::AnnounceNumber&): same shape as AnnounceCard,
    // but the response is an index into this list, not a raw value.
    AnnounceNumberRequest request;
    request.player = read<uint8_t>(p, end);
    const auto count = read<uint8_t>(p, end);
    request.options.reserve(count);
    for (uint8_t i = 0; i < count; ++i) request.options.push_back(read<uint64_t>(p, end));
    return request;
}

SelectUnselectCardRequest parse_select_unselect(const uint8_t* p, const uint8_t* end) {
    SelectUnselectCardRequest request;
    request.player = read<uint8_t>(p, end);
    request.finishable = read<uint8_t>(p, end) != 0;
    request.cancelable = read<uint8_t>(p, end) != 0;
    read<uint32_t>(p, end);
    read<uint32_t>(p, end); // min, max: informational only, mirrors RandomAgent's own handling.
    const auto selectable_count = read<uint32_t>(p, end);
    request.selectable.reserve(selectable_count);
    for (uint32_t i = 0; i < selectable_count; ++i) request.selectable.push_back(read_field_card_full(p, end));
    const auto selected_count = read<uint32_t>(p, end);
    request.already_selected.reserve(selected_count);
    for (uint32_t i = 0; i < selected_count; ++i) request.already_selected.push_back(read_field_card_full(p, end));
    return request;
}

} // namespace

DecisionRequest parse_decision_request(uint8_t kind, const uint8_t* data, const uint8_t* end) {
    if (kind == MSG_SELECT_IDLECMD) return parse_idle_command(data, end);
    if (kind == MSG_SELECT_BATTLECMD) return parse_battle_command(data, end);
    if (kind == MSG_SELECT_CHAIN) return parse_select_chain(data, end);
    if (kind == MSG_SELECT_YESNO) return parse_select_yesno(data, end);
    if (kind == MSG_SELECT_EFFECTYN) return parse_select_effectyn(data, end);
    if (kind == MSG_SORT_CARD) return parse_sort(false);
    if (kind == MSG_SORT_CHAIN) return parse_sort(true);
    if (kind == MSG_SELECT_OPTION) return parse_select_option(data, end);
    if (kind == MSG_SELECT_PLACE || kind == MSG_SELECT_DISFIELD) return parse_select_place(data, end);
    if (kind == MSG_SELECT_TRIBUTE) return parse_select_tribute(data, end);
    if (kind == MSG_SELECT_CARD) return parse_select_card(data, end);
    if (kind == MSG_SELECT_POSITION) return parse_select_position(data, end);
    if (kind == MSG_SELECT_COUNTER) return parse_select_counter(data, end);
    if (kind == MSG_SELECT_SUM) return parse_select_sum(data, end);
    if (kind == MSG_SELECT_UNSELECT_CARD) return parse_select_unselect(data, end);
    if (kind == MSG_ANNOUNCE_RACE) return parse_announce_race(data, end);
    if (kind == MSG_ANNOUNCE_ATTRIB) return parse_announce_attribute(data, end);
    if (kind == MSG_ANNOUNCE_CARD) return parse_announce_card(data, end);
    if (kind == MSG_ANNOUNCE_NUMBER) return parse_announce_number(data, end);
    throw std::runtime_error("unsupported decision message " + std::to_string(kind));
}

} // namespace goat::ai
