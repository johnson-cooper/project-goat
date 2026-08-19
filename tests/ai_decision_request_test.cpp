// Pins goat::ai::parse_decision_request's byte-level parsing (extracted from
// RandomAgent::choose in src/main.cpp) against hand-built engine message
// buffers, so a future change to the wire-format understanding can't
// silently drift without a test failing.
#include <cassert>
#include <cstring>
#include <vector>

#include "ai/DecisionRequest.hpp"

extern "C" {
#include "ocgapi_constants.h"
}

namespace {

struct ByteWriter {
    std::vector<uint8_t> data;
    template <class T> void write(T value) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(T));
    }
};

void write_field_card32(ByteWriter& writer, uint32_t code, uint8_t controller, uint8_t location, uint32_t sequence) {
    writer.write<uint32_t>(code);
    writer.write<uint8_t>(controller);
    writer.write<uint8_t>(location);
    writer.write<uint32_t>(sequence);
}

void write_field_card_full(ByteWriter& writer, uint32_t code, uint8_t controller, uint8_t location, uint32_t sequence, uint32_t position) {
    write_field_card32(writer, code, controller, location, sequence);
    writer.write<uint32_t>(position);
}

} // namespace

int main() {
    // --- MSG_SELECT_IDLECMD: one summonable candidate, one activatable, both phases open.
    {
        ByteWriter writer;
        writer.write<uint8_t>(0); // player
        writer.write<uint32_t>(1); write_field_card32(writer, 55144522, 0, LOCATION_HAND, 3); // Summon group: 1 candidate
        writer.write<uint32_t>(0); // SpecialSummon group
        writer.write<uint32_t>(0); // Reposition group
        writer.write<uint32_t>(0); // SetMonster group
        writer.write<uint32_t>(0); // SetSpellTrap group
        writer.write<uint32_t>(1); // Activatable count
        write_field_card32(writer, 79571449, 0, LOCATION_HAND, 0);
        writer.write<uint64_t>(123456ull);
        writer.write<uint8_t>(0); // client mode
        writer.write<uint8_t>(1); // can_battle_phase
        writer.write<uint8_t>(1); // can_end_phase
        writer.write<uint8_t>(0); // trailing "can shuffle" byte

        auto request = goat::ai::parse_decision_request(MSG_SELECT_IDLECMD, writer.data.data(), writer.data.data() + writer.data.size());
        const auto* idle = std::get_if<goat::ai::IdleCommandRequest>(&request);
        assert(idle != nullptr);
        assert(idle->player == 0);
        assert(idle->summonable.size() == 1 && idle->summonable[0].code == 55144522u);
        assert(idle->special_summonable.empty() && idle->repositionable.empty());
        assert(idle->monster_setable.empty() && idle->spell_setable.empty());
        assert(idle->activatable.size() == 1 && idle->activatable[0].card.code == 79571449u);
        assert(idle->activatable[0].description == 123456ull);
        assert(idle->can_battle_phase && idle->can_end_phase);
    }

    // --- MSG_SELECT_YESNO: just a player + opaque description.
    {
        ByteWriter writer;
        writer.write<uint8_t>(1);
        writer.write<uint64_t>(999ull);
        auto request = goat::ai::parse_decision_request(MSG_SELECT_YESNO, writer.data.data(), writer.data.data() + writer.data.size());
        const auto* yn = std::get_if<goat::ai::SelectYesNoRequest>(&request);
        assert(yn != nullptr && yn->player == 1);
    }

    // --- MSG_SELECT_CARD: full loc_info candidates (code, controller, location, sequence, position).
    {
        ByteWriter writer;
        writer.write<uint8_t>(0); // player
        writer.write<uint8_t>(0); // cancelable (unused)
        writer.write<uint32_t>(1); // min
        writer.write<uint32_t>(2); // max
        writer.write<uint32_t>(2); // choices
        write_field_card_full(writer, 44095762, 1, LOCATION_SZONE, 0, POS_FACEUP_ATTACK);
        write_field_card_full(writer, 53582587, 1, LOCATION_SZONE, 1, POS_FACEUP_ATTACK);
        auto request = goat::ai::parse_decision_request(MSG_SELECT_CARD, writer.data.data(), writer.data.data() + writer.data.size());
        const auto* select = std::get_if<goat::ai::SelectCardRequest>(&request);
        assert(select != nullptr);
        assert(select->min == 1 && select->max == 2);
        assert(select->candidates.size() == 2);
        assert(select->candidates[0].code == 44095762u && select->candidates[0].position == POS_FACEUP_ATTACK);
        assert(select->candidates[1].code == 53582587u);
    }

    // --- MSG_SELECT_TRIBUTE: abbreviated loc_info + trailing release_param byte.
    {
        ByteWriter writer;
        writer.write<uint8_t>(0); // player
        writer.write<uint8_t>(0); // cancelable
        writer.write<uint32_t>(1); // min
        writer.write<uint32_t>(1); // max
        writer.write<uint32_t>(2); // choices
        write_field_card32(writer, 5318639, 0, LOCATION_MZONE, 0); writer.write<uint8_t>(0);
        write_field_card32(writer, 89631139, 0, LOCATION_MZONE, 1); writer.write<uint8_t>(0);
        auto request = goat::ai::parse_decision_request(MSG_SELECT_TRIBUTE, writer.data.data(), writer.data.data() + writer.data.size());
        const auto* tribute = std::get_if<goat::ai::SelectTributeRequest>(&request);
        assert(tribute != nullptr);
        assert(tribute->min == 1 && tribute->max == 1);
        assert(tribute->candidates.size() == 2);
    }

    // --- Unsupported prompt kind: must fail fast, never silently answer.
    {
        ByteWriter writer;
        writer.write<uint8_t>(0);
        bool threw = false;
        try {
            goat::ai::parse_decision_request(MSG_ANNOUNCE_CARD, writer.data.data(), writer.data.data() + writer.data.size());
        } catch (const std::exception&) {
            threw = true;
        }
        assert(threw);
    }

    return 0;
}
