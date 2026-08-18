#include "cards/CardDatabase.hpp"

#include <cassert>
#include <filesystem>

int main() {
    goat::CardDatabase database("external/BabelCDB/cards.cdb", "external/BabelCDB/goat-entries.cdb");

    const auto& dark_hole = database.resolve(53129443);
    assert(dark_hole.code == 53129443);
    assert(!dark_hole.goat_compatibility);
    assert(!dark_hole.name.empty());

    const auto& scapegoat = database.resolve(73915051);
    assert(scapegoat.code == 504700123);
    assert(scapegoat.goat_compatibility);
    assert(scapegoat.name == "Scapegoat"); // the " (GOAT)" suffix is stripped for display; see CardDatabase::read_card
}
