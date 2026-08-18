#include <filesystem>
#include <random>

#include "game/Progression.hpp"

int main() {
    goat::game::Progression game;
    const goat::game::Pack pack{"goat-starter", "GOAT Starter", 100, 5, "goat-starter.jpg", {10071456, 10202894, 11091375}};
    if(!game.buy_pack(pack) || game.profile().credits != 200) return 1;
    std::mt19937 random(12345);
    const auto cards = game.open_pack(pack, random);
    if(cards.size() != 5 || !game.owns(cards.front())) return 2;
    const goat::game::Npc npc{"rookie", "Rookie", 1, "decks/starter/flc3-3rd-aggro.ydk", {50, "goat-starter"}};
    game.award_npc_victory(npc);
    const auto file = (std::filesystem::temp_directory_path() / "goat-progression-test.sav").string();
    goat::game::ProfileStore::save(game.profile(), file);
    const auto loaded = goat::game::ProfileStore::load(file);
    std::filesystem::remove(file);
    return loaded.credits == 250 && loaded.npc_wins.at("rookie") == 1 && loaded.sealed_packs.at("goat-starter") == 1 ? 0 : 3;
}
