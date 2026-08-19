#include "CardEvaluator.hpp"

#include <algorithm>

extern "C" {
#include "ocgapi_constants.h"
}

namespace goat::ai {

int32_t effective_power(const goat::CardDefinition& card, uint8_t position) {
    const bool defense = (position & (POS_FACEUP_DEFENSE | POS_FACEDOWN_DEFENSE)) != 0;
    return defense ? card.defense : card.attack;
}

int32_t effective_power(goat::CardDatabase& database, uint32_t code, uint8_t position) {
    return effective_power(database.resolve(code), position);
}

bool is_monster(const goat::CardDefinition& card) { return (card.type & TYPE_MONSTER) != 0; }
bool is_spell(const goat::CardDefinition& card) { return (card.type & TYPE_SPELL) != 0; }
bool is_trap(const goat::CardDefinition& card) { return (card.type & TYPE_TRAP) != 0; }

bool is_persistent_threat(const goat::CardDefinition& card) {
    if (is_monster(card)) return (card.type & TYPE_EFFECT) != 0;
    return (card.type & (TYPE_CONTINUOUS | TYPE_EQUIP | TYPE_FIELD)) != 0;
}

int32_t generic_value(goat::CardDatabase& database, uint32_t code) {
    auto& card = database.resolve(code);
    if (is_monster(card)) {
        int32_t score = std::max(card.attack, card.defense) + static_cast<int32_t>(card.level & 0xffu) * 100;
        if (card.type & TYPE_EFFECT) score += 300;
        return score;
    }
    int32_t score = 500; // A baseline Normal Spell/Trap already represents card advantage.
    if (is_persistent_threat(card)) score += 400;
    return score;
}

} // namespace goat::ai
