#include "cards/CardDatabase.hpp"
#include "deck/Banlist.hpp"
#include "game/Catalog.hpp"
#include "game/DeckBuilder.hpp"

// Data-driven from data/npcs.json rather than a hardcoded file list, so this
// automatically covers whatever NPC roster is currently shipped instead of
// needing an edit every time the roster changes.
int main() {
    goat::CardDatabase database("external/BabelCDB/cards.cdb", "external/BabelCDB/goat-entries.cdb");
    const auto banlist = goat::Banlist::load("external/LFLists/GOAT.lflist.conf");
    const auto catalog = goat::game::load_catalog("data/npcs.json", "data/packs.json");
    if(catalog.npcs.empty()) return 1;
    for(const auto& npc : catalog.npcs) {
        const auto deck = goat::game::read_deck(npc.deck_path);
        goat::game::validate_npc_deck(deck, database, banlist);
    }
    return 0;
}
