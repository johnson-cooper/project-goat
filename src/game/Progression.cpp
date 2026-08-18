#include "Progression.hpp"

#include <filesystem>
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

Progression::Progression(Profile profile) : profile_(std::move(profile)) { grant_starter_deck(profile_.selected_deck); }
const Profile& Progression::profile() const noexcept { return profile_; }
void Progression::select_starter_deck(const std::string& path) { grant_starter_deck(path); profile_.selected_deck = path; }
void Progression::grant_starter_deck(const std::string& path) {
    std::ifstream input(path); if(!input) throw std::runtime_error("cannot open starter deck: " + path);
    // Grants #extra alongside #main: DeckBuilder::validate_player_deck checks
    // ownership across main+extra combined, so a starter deck's own fusion
    // monsters must be granted too or selecting/replaying it would fail its
    // own ownership check.
    //
    // Counts occurrences per code first rather than granting a flat 1 copy
    // per line: a deck that plays 3 copies of a card must grant 3 copies of
    // ownership, or the player could never rebuild their own starter deck
    // (or add a 2nd/3rd copy of anything) in the Deck Editor — every card
    // would already show as "maxed out" at 1/1 owned.
    bool granting = false; std::string line; std::map<uint32_t, int> counts;
    while(std::getline(input, line)) {
        line = rtrim(line);
        if(line == "#main" || line == "#extra") { granting = true; continue; }
        if(!line.empty() && (line[0] == '#' || line[0] == '!')) { granting = false; continue; }
        if(granting && !line.empty()) ++counts[static_cast<uint32_t>(std::stoul(line))];
    }
    for(const auto& [code, count] : counts) {
        auto& copies = profile_.collection[code];
        if(copies < count) copies = count;
    }
}
void Progression::award_npc_victory(const Npc& npc) {
    profile_.credits += npc.reward.credits;
    ++profile_.npc_wins[npc.id];
    if(!npc.reward.pack_id.empty()) ++profile_.sealed_packs[npc.reward.pack_id];
}
bool Progression::buy_pack(const Pack& pack) {
    if(pack.price < 0 || pack.cards_per_pack <= 0 || pack.pool.empty() || profile_.credits < pack.price) return false;
    profile_.credits -= pack.price;
    ++profile_.sealed_packs[pack.id];
    return true;
}
std::vector<uint32_t> Progression::open_pack(const Pack& pack, std::mt19937& random) {
    auto found = profile_.sealed_packs.find(pack.id);
    if(found == profile_.sealed_packs.end() || found->second <= 0 || pack.cards_per_pack <= 0 || pack.pool.empty())
        throw std::runtime_error("no sealed pack available to open");
    --found->second;
    std::uniform_int_distribution<size_t> choose(0, pack.pool.size() - 1);
    std::vector<uint32_t> cards;
    cards.reserve(static_cast<size_t>(pack.cards_per_pack));
    for(int i = 0; i < pack.cards_per_pack; ++i) {
        const auto code = pack.pool[choose(random)];
        ++profile_.collection[code]; cards.push_back(code);
    }
    return cards;
}
bool Progression::owns(uint32_t card_code, int copies) const {
    const auto found = profile_.collection.find(card_code);
    return found != profile_.collection.end() && found->second >= copies;
}

void ProfileStore::save(const Profile& profile, const std::string& filename) {
    const std::filesystem::path path(filename);
    if(path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::trunc);
    if(!output) throw std::runtime_error("cannot save profile: " + filename);
    output << "GOAT_PROFILE_V1\nname=" << profile.player_name << "\ncredits=" << profile.credits
           << "\ndeck=" << profile.selected_deck << '\n';
    for(const auto& [code, copies] : profile.collection) output << "card=" << code << ',' << copies << '\n';
    for(const auto& [id, count] : profile.sealed_packs) output << "pack=" << id << ',' << count << '\n';
    for(const auto& [id, wins] : profile.npc_wins) output << "npc=" << id << ',' << wins << '\n';
}

Profile ProfileStore::load(const std::string& filename) {
    std::ifstream input(filename);
    if(!input) return {};
    std::string line;
    if(!std::getline(input, line) || line != "GOAT_PROFILE_V1") throw std::runtime_error("unsupported profile file");
    Profile profile;
    while(std::getline(input, line)) {
        const auto split = line.find('='); if(split == std::string::npos) continue;
        const auto key = line.substr(0, split), value = line.substr(split + 1);
        if(key == "name") profile.player_name = value;
        else if(key == "credits") profile.credits = std::stoi(value);
        else if(key == "deck") profile.selected_deck = value;
        else if(key == "card" || key == "pack" || key == "npc") {
            const auto comma = value.rfind(','); if(comma == std::string::npos) throw std::runtime_error("malformed profile entry");
            const int count = std::stoi(value.substr(comma + 1));
            if(key == "card") profile.collection[static_cast<uint32_t>(std::stoul(value.substr(0, comma)))] = count;
            else if(key == "pack") profile.sealed_packs[value.substr(0, comma)] = count;
            else profile.npc_wins[value.substr(0, comma)] = count;
        }
    }
    return profile;
}

} // namespace goat::game
