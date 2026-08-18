#include <filesystem>

#include "game/DeckBuilder.hpp"

int main() {
    // Explicit rather than relying on Profile's default selected_deck (which
    // is the player's one starter, water-fusion — unrelated to what this test
    // actually exercises: validate/select/write/read round-tripping against
    // an arbitrary legal deck).
    goat::game::Profile migrated; migrated.collection[10071456] = 3; migrated.selected_deck = "decks/starter/flc1-3rd-chaos-con.ydk";
    goat::game::Progression progression(migrated);
    const auto deck = goat::game::read_deck("decks/starter/flc1-3rd-chaos-con.ydk");
    goat::CardDatabase database("external/BabelCDB/cards.cdb", "external/BabelCDB/goat-entries.cdb");
    const auto banlist = goat::Banlist::load("external/LFLists/GOAT.lflist.conf");
    goat::game::validate_player_deck(deck, progression.profile(), database, banlist);
    progression.select_starter_deck("decks/starter/flc2-3rd-goat.ydk");
    goat::game::validate_player_deck(goat::game::read_deck(progression.profile().selected_deck), progression.profile(), database, banlist);
    const auto output = std::filesystem::temp_directory_path() / "goat-deck-builder-test.ydk";
    goat::game::write_deck(output, deck);
    const auto restored = goat::game::read_deck(output); std::filesystem::remove(output);
    return restored.main == deck.main ? 0 : 1;
}
