#include "deck/Banlist.hpp"

#include <cassert>
#include <stdexcept>
#include <vector>

int main() {
    const auto banlist = goat::Banlist::load("external/LFLists/GOAT.lflist.conf");
    banlist.validate_main_deck(std::vector<uint32_t>{504700123, 504700123, 504700123}, false);
    bool rejected = false;
    try { banlist.validate_main_deck(std::vector<uint32_t>{53129443}, false); } catch(const std::runtime_error&) { rejected = true; }
    assert(rejected);
}
