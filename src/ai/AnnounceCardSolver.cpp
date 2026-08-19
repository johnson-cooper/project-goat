#include "AnnounceCardSolver.hpp"

#include <vector>

extern "C" {
#include "ocgapi_constants.h"
}

namespace goat::ai {
namespace {

// Special-cased directly in ygopro-core's own is_declarable
// (external/ygopro-core/playerop.cpp) — Marine Dolphin and Twinkle Moss are
// always declarable regardless of the predicate, a ruling quirk from their
// real card text. Hardcoded here (rather than pulling in the internal
// card.h these constants live in) since they're the only two values needed.
constexpr uint32_t kCardMarineDolphin = 78734254;
constexpr uint32_t kCardTwinkleMoss = 13857930;

bool matches_declarable_predicate(const goat::CardDefinition& card, const std::vector<uint64_t>& opcodes) {
    std::vector<int64_t> stack;
    bool allow_aliases = false, allow_tokens = false;
    auto pop = [&]() -> int64_t {
        if (stack.empty()) return 0;
        const auto value = stack.back();
        stack.pop_back();
        return value;
    };

    for (const uint64_t opcode : opcodes) {
        switch (opcode) {
            case OPCODE_ADD: { const auto rhs = pop(); const auto lhs = pop(); stack.push_back(lhs + rhs); break; }
            case OPCODE_SUB: { const auto rhs = pop(); const auto lhs = pop(); stack.push_back(lhs - rhs); break; }
            case OPCODE_MUL: { const auto rhs = pop(); const auto lhs = pop(); stack.push_back(lhs * rhs); break; }
            case OPCODE_DIV: { const auto rhs = pop(); const auto lhs = pop(); stack.push_back(rhs != 0 ? lhs / rhs : 0); break; }
            case OPCODE_AND: { const auto rhs = pop(); const auto lhs = pop(); stack.push_back((lhs && rhs) ? 1 : 0); break; }
            case OPCODE_OR: { const auto rhs = pop(); const auto lhs = pop(); stack.push_back((lhs || rhs) ? 1 : 0); break; }
            case OPCODE_NEG: { const auto value = pop(); stack.push_back(-value); break; }
            case OPCODE_NOT: { const auto value = pop(); stack.push_back(!value ? 1 : 0); break; }
            case OPCODE_BAND: { const auto rhs = pop(); const auto lhs = pop(); stack.push_back(lhs & rhs); break; }
            case OPCODE_BOR: { const auto rhs = pop(); const auto lhs = pop(); stack.push_back(lhs | rhs); break; }
            case OPCODE_BXOR: { const auto rhs = pop(); const auto lhs = pop(); stack.push_back(lhs ^ rhs); break; }
            case OPCODE_BNOT: { const auto value = pop(); stack.push_back(~value); break; }
            case OPCODE_LSHIFT: { const auto rhs = pop(); const auto lhs = pop(); stack.push_back(lhs << rhs); break; }
            case OPCODE_RSHIFT: { const auto rhs = pop(); const auto lhs = pop(); stack.push_back(lhs >> rhs); break; }
            // ISCODE compares by equality; ISTYPE/ISRACE/ISATTRIBUTE push the
            // raw bitwise-AND result (not forced to 0/1), matching
            // is_declarable's UNARY_OP_OP macro exactly.
            case OPCODE_ISCODE: { const auto value = pop(); stack.push_back(card.code == static_cast<uint32_t>(value) ? 1 : 0); break; }
            case OPCODE_ISTYPE: { const auto value = pop(); stack.push_back(static_cast<int64_t>(card.type & static_cast<uint32_t>(value))); break; }
            case OPCODE_ISRACE: { const auto value = pop(); stack.push_back(static_cast<int64_t>(card.race & static_cast<uint64_t>(value))); break; }
            case OPCODE_ISATTRIBUTE: { const auto value = pop(); stack.push_back(static_cast<int64_t>(card.attribute & static_cast<uint32_t>(value))); break; }
            case OPCODE_GETCODE: stack.push_back(card.code); break;
            case OPCODE_GETTYPE: stack.push_back(card.type); break;
            case OPCODE_GETRACE: stack.push_back(static_cast<int64_t>(card.race)); break;
            case OPCODE_GETATTRIBUTE: stack.push_back(card.attribute); break;
            case OPCODE_ISSETCARD: {
                const auto set_code = pop();
                bool found = false;
                const uint16_t settype = static_cast<uint16_t>(set_code & 0xfff);
                const uint16_t setsubtype = static_cast<uint16_t>(set_code & 0xf000);
                for (const auto setcode : card.setcodes) {
                    if ((setcode & 0xfff) == settype && (setcode & 0xf000 & setsubtype) == setsubtype) { found = true; break; }
                }
                stack.push_back(found ? 1 : 0);
                break;
            }
            case OPCODE_ALLOW_ALIASES: allow_aliases = true; break;
            case OPCODE_ALLOW_TOKENS: allow_tokens = true; break;
            default: stack.push_back(static_cast<int64_t>(opcode)); break;
        }
    }

    if (stack.size() != 1 || stack.back() == 0) return false;
    if (card.code == kCardMarineDolphin || card.code == kCardTwinkleMoss) return true;
    const bool alias_ok = allow_aliases || card.alias == 0;
    const bool not_a_bare_token = allow_tokens || ((card.type & (TYPE_MONSTER | TYPE_TOKEN)) != (TYPE_MONSTER | TYPE_TOKEN));
    return alias_ok && not_a_bare_token;
}

} // namespace

std::optional<uint32_t> find_declarable_card(goat::CardDatabase& database, const std::vector<uint64_t>& predicate_opcodes) {
    for (const uint32_t code : database.all_codes()) {
        if (matches_declarable_predicate(database.resolve(code), predicate_opcodes)) return code;
    }
    return std::nullopt;
}

std::vector<uint32_t> find_declarable_cards(goat::CardDatabase& database, const std::vector<uint64_t>& predicate_opcodes, size_t limit) {
    std::vector<uint32_t> matches;
    for (const uint32_t code : database.all_codes()) {
        if (matches.size() >= limit) break;
        if (matches_declarable_predicate(database.resolve(code), predicate_opcodes)) matches.push_back(code);
    }
    return matches;
}

} // namespace goat::ai
