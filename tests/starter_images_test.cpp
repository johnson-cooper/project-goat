#include <filesystem>
#include <string>

#include "game/Catalog.hpp"
#include "game/DeckBuilder.hpp"

// Data-driven from data/npcs.json — see starter_decks_test.cpp.
int main() {
    const auto catalog = goat::game::load_catalog("data/npcs.json", "data/packs.json");
    for(const auto& npc : catalog.npcs) {
        const auto deck = goat::game::read_deck(npc.deck_path);
        for(const auto code : deck.main)
            if(!std::filesystem::exists(std::filesystem::path("external/card_images") / (std::to_string(code) + ".jpg"))) return 1;
    }
    return 0;
}
