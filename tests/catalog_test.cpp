#include "game/Catalog.hpp"

#include <random>

int main() {
    const auto catalog = goat::game::load_catalog("data/npcs.json", "data/packs.json");
    const auto& rookie = catalog.npc("flc3_3rd_aggro");
    const auto& pack = catalog.pack("goat-starter");
    const auto& classics = catalog.pack("goat-classics");
    if(catalog.npcs.size() != 12 || catalog.packs.size() != 2 || rookie.reward.credits != 60 ||
       rookie.reward.pack_id != "goat-starter" || pack.cards_per_pack != 5 || pack.pool.empty()) return 1;
    goat::game::Progression profile;
    std::mt19937 random(2);
    if(!profile.buy_pack(pack) || profile.open_pack(pack, random).size() != 5) return 2;
    goat::game::Progression classics_profile;
    if(!classics_profile.buy_pack(classics) || classics_profile.open_pack(classics, random).size() != 9) return 3;
    return profile.profile().credits == 200 && !profile.profile().collection.empty() && classics.art == "goat-classics.jpg" && classics_profile.profile().credits == 50 ? 0 : 4;
}
