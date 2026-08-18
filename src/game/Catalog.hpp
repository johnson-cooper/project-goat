#pragma once

#include <string>
#include <vector>

#include "Progression.hpp"

namespace goat::game {

struct Catalog {
    std::vector<Npc> npcs;
    std::vector<Pack> packs;
    const Npc& npc(const std::string& id) const;
    const Pack& pack(const std::string& id) const;
};

Catalog load_catalog(const std::string& npc_filename, const std::string& pack_filename);

} // namespace goat::game
