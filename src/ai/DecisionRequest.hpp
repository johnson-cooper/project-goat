#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

// Structured, engine-legality-preserving decomposition of the raw
// OCG_DuelGetMessage payloads for every decision prompt goat-sim currently
// answers (see RandomAgent::choose in src/main.cpp, which this is extracted
// from byte-for-byte — the wire formats here are not re-derived, they mirror
// already-verified parsing). A DuelAgent never sees raw engine bytes; it only
// ever sees one of these variants, and can only answer with an index/target
// that appears somewhere inside the matching request.

namespace goat::ai {

// One legal candidate card offered by the engine for a given action. `code`
// is always safe to read here because ygopro-core only ever offers a card as
// a *candidate for a legal action* to the player who is actually being asked
// to choose it — this is never opponent hidden information.
struct FieldCard {
    uint32_t code{};
    uint8_t controller{};
    uint8_t location{};
    uint32_t sequence{};
    // Only populated by prompt kinds whose wire format actually carries a
    // per-candidate position (see DecisionRequest.cpp); 0 elsewhere. Prefer
    // DuelObservation's own board data for anything position-sensitive.
    uint32_t position{};
};

struct ActivatableCard {
    FieldCard card;
    uint64_t description{};
    uint8_t client_mode{};
};

struct IdleCommandRequest {
    uint8_t player{};
    std::vector<FieldCard> summonable;
    std::vector<FieldCard> special_summonable;
    std::vector<FieldCard> repositionable;
    std::vector<FieldCard> monster_setable;
    std::vector<FieldCard> spell_setable;
    std::vector<ActivatableCard> activatable;
    bool can_battle_phase{};
    bool can_end_phase{};
};

struct BattleCommandRequest {
    uint8_t player{};
    std::vector<ActivatableCard> activatable;
    std::vector<FieldCard> attackable;
    bool can_main_phase_2{};
    bool can_end_phase{};
};

struct SelectChainRequest {
    struct ChainLink { FieldCard card; uint64_t description{}; };
    uint8_t player{};
    bool forced{};
    std::vector<ChainLink> candidates;
};

struct SelectYesNoRequest { uint8_t player{}; };

struct SelectEffectYesNoRequest {
    uint8_t player{};
    FieldCard card;
    uint64_t description{};
};

// The existing implementation this is extracted from never established a
// verified byte layout for MSG_SORT_CARD/MSG_SORT_CHAIN — it always declines
// (keeps the engine's default order) without reading any fields, which is
// always a legal response. This mirrors that exactly rather than guessing at
// an unverified per-candidate format.
struct SortRequest {
    bool is_chain_order{};
};

struct SelectOptionRequest {
    uint8_t player{};
    std::vector<uint64_t> option_descriptions;
};

struct SelectPlaceRequest {
    uint8_t player{};
    uint8_t target_player{};
    uint8_t location{}; // LOCATION_MZONE or LOCATION_SZONE
    std::vector<uint8_t> available_sequences;
};

struct SelectTributeRequest {
    uint8_t player{};
    uint32_t min{};
    uint32_t max{};
    std::vector<FieldCard> candidates;
};

struct SelectCardRequest {
    uint8_t player{};
    uint32_t min{};
    uint32_t max{};
    std::vector<FieldCard> candidates;
};

struct SelectPositionRequest {
    uint8_t player{};
    uint32_t code{};
    uint8_t positions{}; // bitmask of POS_*
};

struct SelectCounterRequest {
    uint16_t counter_type{};
    uint16_t count{};
    std::vector<FieldCard> candidates;
    std::vector<uint16_t> available;
};

struct SelectSumRequest {
    uint8_t player{};
    bool exact_mode{};
    uint32_t min{};
    uint32_t max{};
    std::vector<FieldCard> must_select;
    std::vector<uint32_t> must_select_params;
    std::vector<FieldCard> optional;
    std::vector<uint32_t> optional_params;
};

struct SelectUnselectCardRequest {
    uint8_t player{};
    bool finishable{};
    bool cancelable{};
    std::vector<FieldCard> selectable;
    std::vector<FieldCard> already_selected;
};

// "Declare a Race" (e.g. Tribe-Infecting Virus) — `available` is a bitmask
// of RACE_* bits; the response must set exactly `count` of them, all within
// `available`.
struct AnnounceRaceRequest {
    uint8_t player{};
    uint8_t count{};
    uint64_t available{};
};

// "Declare an Attribute" — same shape as AnnounceRace, over ATTRIBUTE_* bits.
struct AnnounceAttributeRequest {
    uint8_t player{};
    uint8_t count{};
    uint32_t available{};
};

// "Declare a card name satisfying condition X" (e.g. Dark Designator,
// Prohibition, Mind Crush) — `predicate_opcodes` is a small stack-machine
// bytecode program (ygopro-core's OPCODE_* encoding) the declared card must
// satisfy; see AnnounceCardSolver.cpp for the interpreter.
struct AnnounceCardRequest {
    uint8_t player{};
    std::vector<uint64_t> predicate_opcodes;
};

// "Declare a number" (e.g. Wall of Revealing Light) — the response is an
// index into `options`, not the number itself.
struct AnnounceNumberRequest {
    uint8_t player{};
    std::vector<uint64_t> options;
};

using DecisionRequest = std::variant<
    IdleCommandRequest,
    BattleCommandRequest,
    SelectChainRequest,
    SelectYesNoRequest,
    SelectEffectYesNoRequest,
    SortRequest,
    SelectOptionRequest,
    SelectPlaceRequest,
    SelectTributeRequest,
    SelectCardRequest,
    SelectPositionRequest,
    SelectCounterRequest,
    SelectSumRequest,
    SelectUnselectCardRequest,
    AnnounceRaceRequest,
    AnnounceAttributeRequest,
    AnnounceCardRequest,
    AnnounceNumberRequest>;

// Parses one OCG_DuelGetMessage frame's payload (everything after the leading
// kind byte, i.e. what `main.cpp` currently reads by hand inline). Throws
// std::runtime_error("unsupported decision message ...") for any kind not
// listed above — an explicit, fail-fast gap, never a silently invented
// response.
DecisionRequest parse_decision_request(uint8_t kind, const uint8_t* data, const uint8_t* end);

} // namespace goat::ai
