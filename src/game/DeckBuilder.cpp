#include "DeckBuilder.hpp"

#include <fstream>
#include <map>
#include <stdexcept>

namespace {
// Trims trailing CR/whitespace so both Unix-LF and Windows-CRLF .ydk files
// parse identically, and a stray trailing space on a "#main"/"#extra" marker
// line (common from deck editors) doesn't silently break the exact match.
std::string rtrim(std::string text) {
    while(!text.empty() && (text.back()=='\r' || text.back()==' ' || text.back()=='\t')) text.pop_back();
    return text;
}
} // namespace

namespace goat::game {
DeckContents read_deck(const std::filesystem::path& filename) {
    std::ifstream input(filename); if(!input) throw std::runtime_error("cannot open deck: " + filename.string());
    DeckContents deck; enum class Section { None, Main, Extra } section = Section::None; std::string line;
    while(std::getline(input, line)) {
        line = rtrim(line);
        if(line == "#main") { section = Section::Main; continue; }
        if(line == "#extra") { section = Section::Extra; continue; }
        if(!line.empty() && (line[0] == '#' || line[0] == '!')) { section = Section::None; continue; }
        if(line.empty()) continue;
        if(section == Section::Main) deck.main.push_back(static_cast<uint32_t>(std::stoul(line)));
        else if(section == Section::Extra) deck.extra.push_back(static_cast<uint32_t>(std::stoul(line)));
    }
    return deck;
}
namespace {
// Every requested copy (main+extra combined) must be owned by the profile.
void check_ownership(const DeckContents& deck, const Profile& profile) {
    std::map<uint32_t, int> requested;
    for(const auto code : deck.main) ++requested[code];
    for(const auto code : deck.extra) ++requested[code];
    for(const auto& [code, copies] : requested) {
        const auto owned = profile.collection.find(code);
        if(owned == profile.collection.end() || owned->second < copies) throw std::runtime_error("deck contains cards not owned by profile");
    }
}
// Resolves every code to its canonical (GOAT-alias-translated) form before
// handing the combined main+extra list to the banlist, matching how the duel
// engine itself resolves cards (see CardDatabase::resolve / goat_code_for).
void check_banlist(const DeckContents& deck, CardDatabase& database, const Banlist& banlist) {
    std::vector<uint32_t> codes; codes.reserve(deck.main.size() + deck.extra.size());
    for(const auto code : deck.main) codes.push_back(database.resolve(code).code);
    for(const auto code : deck.extra) codes.push_back(database.resolve(code).code);
    banlist.validate_main_deck(codes, false);
}
void check_sizes(const DeckContents& deck) {
    if(deck.main.size() < 40 || deck.main.size() > 60) throw std::runtime_error("GOAT main deck must contain 40-60 cards");
    if(deck.extra.size() > 15) throw std::runtime_error("extra deck cannot exceed 15 cards");
}
} // namespace
void validate_player_deck(const DeckContents& deck, const Profile& profile, CardDatabase& database, const Banlist& banlist) {
    check_sizes(deck);
    check_ownership(deck, profile);
    check_banlist(deck, database, banlist);
}
void validate_npc_deck(const DeckContents& deck, CardDatabase& database, const Banlist& banlist) {
    check_sizes(deck);
    check_banlist(deck, database, banlist);
}
void write_deck(const std::filesystem::path& filename, const DeckContents& deck) {
    if(filename.has_parent_path()) std::filesystem::create_directories(filename.parent_path());
    std::ofstream output(filename, std::ios::trunc); if(!output) throw std::runtime_error("cannot write deck: " + filename.string());
    output << "#main\n"; for(const auto code : deck.main) output << code << '\n';
    output << "#extra\n"; for(const auto code : deck.extra) output << code << '\n';
    output << "!side\n";
}
} // namespace goat::game
