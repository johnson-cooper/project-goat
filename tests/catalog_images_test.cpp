#include <filesystem>

#include "game/Catalog.hpp"

int main() {
    const auto catalog = goat::game::load_catalog("data/npcs.json", "data/packs.json");
    for(const auto& pack : catalog.packs) {
        if(!std::filesystem::exists(std::filesystem::path("external/packart") / pack.art)) return 1;
        for(const auto code : pack.pool) {
        if(!std::filesystem::exists(std::filesystem::path("external/card_images") / (std::to_string(code) + ".jpg"))) return 1;
        }
    }
    return 0;
}
