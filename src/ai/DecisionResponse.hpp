#pragma once

#include <array>
#include <cstdint>
#include <vector>

// The exact response-encoding helpers RandomAgent used to own privately in
// src/main.cpp, extracted verbatim (same byte layouts) so both the human
// menu/file-IPC path and every DuelAgent share one implementation.

namespace goat::ai {

using Response = std::array<uint8_t, 64>;

// MSG_SELECT_IDLECMD action-kind codes, matching the engine's own encoding
// (see RandomAgent::choose_human_idle in src/main.cpp for the reference).
namespace idle_action {
constexpr uint16_t Summon = 0;
constexpr uint16_t SpecialSummon = 1;
constexpr uint16_t Reposition = 2;
constexpr uint16_t SetMonster = 3;
constexpr uint16_t SetSpellTrap = 4;
constexpr uint16_t Activate = 5;
constexpr uint16_t EnterBattlePhase = 6;
constexpr uint16_t EnterEndPhase = 7;
constexpr uint16_t Pass = 8;
} // namespace idle_action

// MSG_SELECT_BATTLECMD action-kind codes.
namespace battle_action {
constexpr uint16_t Activate = 0;
constexpr uint16_t Attack = 1;
constexpr uint16_t EnterMainPhase2 = 2;
constexpr uint16_t EnterEndPhase = 3;
} // namespace battle_action

// (type << 16) | index — used by MSG_SELECT_IDLECMD/MSG_SELECT_BATTLECMD.
Response encode_type_index(uint16_t type, uint16_t index);
// A bare little-endian int32_t in the first 4 bytes — used by MSG_SELECT_CHAIN
// (-1 to decline / index to activate), MSG_SELECT_YESNO, MSG_SELECT_EFFECTYN,
// MSG_SORT_CARD/MSG_SORT_CHAIN (always -1), MSG_SELECT_OPTION,
// MSG_ANNOUNCE_ATTRIB (an attribute bitmask), MSG_ANNOUNCE_CARD (a card
// code), and MSG_ANNOUNCE_NUMBER (an index into the offered options).
Response encode_raw(int32_t value);
// The 8-byte little-endian uint64_t equivalent — used by MSG_ANNOUNCE_RACE
// (a race bitmask, wider than fits in encode_raw's 4 bytes).
Response encode_raw64(uint64_t value);
// player/location/sequence bytes — MSG_SELECT_PLACE/MSG_SELECT_DISFIELD.
Response encode_place(uint8_t player, uint8_t location, uint8_t sequence);
// count (u32 at offset 4) followed by that many u32 indices — MSG_SELECT_CARD,
// MSG_SELECT_TRIBUTE, MSG_SELECT_SUM.
Response encode_indices(const std::vector<uint32_t>& indices);
// Convenience: select the first `count` candidates by index.
Response encode_first_n(uint32_t count);
// One u16 amount per candidate, front-to-back — MSG_SELECT_COUNTER.
Response encode_counter_amounts(const std::vector<uint16_t>& amounts);
// (action, index) pair — MSG_SELECT_UNSELECT_CARD (action: 1=select,
// -1=finish/cancel).
Response encode_select_unselect(int32_t action, int32_t index);

} // namespace goat::ai
