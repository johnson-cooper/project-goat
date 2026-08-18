#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "cards/CardDatabase.hpp"
#include "deck/Banlist.hpp"
#include "Progression.hpp"

namespace goat::game {

struct DeckContents { std::vector<uint32_t> main; std::vector<uint32_t> extra; };

DeckContents read_deck(const std::filesystem::path& filename);
// Throws on: main deck outside 40-60 cards, extra deck over 15 cards, or any
// banlist violation in either. Ownership (every requested copy actually owned
// by `profile`) is checked across main+extra combined.
void validate_player_deck(const DeckContents& deck, const Profile& profile, CardDatabase& database, const Banlist& banlist);
// NPCs don't "own" cards the way a player profile does, so this only checks
// deck size and banlist legality (no collection-ownership check).
void validate_npc_deck(const DeckContents& deck, CardDatabase& database, const Banlist& banlist);
void write_deck(const std::filesystem::path& filename, const DeckContents& deck);

} // namespace goat::game
