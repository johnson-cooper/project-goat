// Verifies the MSG_ANNOUNCE_* wire parsing and the AnnounceCard predicate
// solver (a port of ygopro-core's own is_declarable in playerop.cpp).
#include <cassert>

#include "ai/AnnounceCardSolver.hpp"
#include "ai/DecisionRequest.hpp"
#include "cards/CardDatabase.hpp"

extern "C" {
#include "ocgapi_constants.h"
}

using namespace goat::ai;

namespace {
struct ByteWriter {
    std::vector<uint8_t> data;
    template <class T> void write(T value) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        data.insert(data.end(), bytes, bytes + sizeof(T));
    }
};
} // namespace

int main() {
    goat::CardDatabase database("external/BabelCDB/cards.cdb", "external/BabelCDB/goat-entries.cdb");

    // --- Parsing: MSG_ANNOUNCE_RACE.
    {
        ByteWriter writer;
        writer.write<uint8_t>(0);       // player
        writer.write<uint8_t>(1);       // count
        writer.write<uint64_t>(0x1full); // available
        auto request = parse_decision_request(MSG_ANNOUNCE_RACE, writer.data.data(), writer.data.data() + writer.data.size());
        const auto* race = std::get_if<AnnounceRaceRequest>(&request);
        assert(race != nullptr && race->count == 1 && race->available == 0x1full);
    }

    // --- Parsing: MSG_ANNOUNCE_NUMBER.
    {
        ByteWriter writer;
        writer.write<uint8_t>(1); // player
        writer.write<uint8_t>(3); // option count
        writer.write<uint64_t>(1); writer.write<uint64_t>(3); writer.write<uint64_t>(5);
        auto request = parse_decision_request(MSG_ANNOUNCE_NUMBER, writer.data.data(), writer.data.data() + writer.data.size());
        const auto* number = std::get_if<AnnounceNumberRequest>(&request);
        assert(number != nullptr && number->options.size() == 3 && number->options[1] == 3);
    }

    // --- Solver: find a card matching "code == Pot of Greed" via ISCODE.
    {
        std::vector<uint64_t> opcodes{55144522ull, OPCODE_ISCODE};
        auto found = find_declarable_card(database, opcodes);
        assert(found.has_value() && *found == 55144522u);
    }

    // --- Solver: an impossible predicate (declare a card whose code equals
    // an id that doesn't exist) yields no match.
    {
        std::vector<uint64_t> opcodes{0xffffffffull, OPCODE_ISCODE};
        auto found = find_declarable_card(database, opcodes);
        assert(!found.has_value());
    }

    // --- Solver: find_declarable_cards returns multiple matches, all
    // satisfying the predicate (LIGHT-attribute monsters), capped at the
    // requested limit.
    {
        std::vector<uint64_t> opcodes{static_cast<uint64_t>(ATTRIBUTE_LIGHT), OPCODE_ISATTRIBUTE};
        auto matches = find_declarable_cards(database, opcodes, 5);
        assert(matches.size() == 5);
        for (const auto code : matches) {
            auto& card = database.resolve(code);
            assert((card.attribute & ATTRIBUTE_LIGHT) != 0);
        }
    }

    return 0;
}
