#include "Catalog.hpp"

#include <cctype>
#include <fstream>
#include <regex>
#include <stdexcept>

namespace goat::game {
namespace {
std::string read_all(const std::string& filename) {
    std::ifstream input(filename);
    if(!input) throw std::runtime_error("cannot open catalog: " + filename);
    return {std::istreambuf_iterator<char>(input), {}};
}
std::vector<std::string> objects(const std::string& document) {
    std::vector<std::string> result; int depth = 0; size_t start = 0;
    for(size_t i = 0; i < document.size(); ++i) {
        if(document[i] == '{') { if(depth++ == 0) start = i; }
        else if(document[i] == '}' && --depth == 0) result.push_back(document.substr(start, i - start + 1));
    }
    return result;
}
std::string string_field(const std::string& object, const char* key) {
    const std::regex pattern(std::string("\\\"") + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch match; if(!std::regex_search(object, match, pattern)) throw std::runtime_error(std::string("catalog field missing: ") + key);
    return match[1].str();
}
int int_field(const std::string& object, const char* key) {
    const std::regex pattern(std::string("\\\"") + key + "\\\"\\s*:\\s*([0-9]+)");
    std::smatch match; if(!std::regex_search(object, match, pattern)) throw std::runtime_error(std::string("catalog field missing: ") + key);
    return std::stoi(match[1].str());
}
int int_field_or(const std::string& object, const char* key, int fallback) {
    const std::regex pattern(std::string("\\\"") + key + "\\\"\\s*:\\s*([0-9]+)");
    std::smatch match; if(!std::regex_search(object, match, pattern)) return fallback;
    return std::stoi(match[1].str());
}
std::string string_field_or(const std::string& object, const char* key, const std::string& fallback) {
    const std::regex pattern(std::string("\\\"") + key + "\\\"\\s*:\\s*\\\"([^\\\"]*)\\\"");
    std::smatch match; if(!std::regex_search(object, match, pattern)) return fallback;
    return match[1].str();
}
std::vector<uint32_t> card_list(const std::string& object) {
    const auto key = object.find("\"cards\""); if(key == std::string::npos) throw std::runtime_error("pack cards missing");
    const auto begin = object.find('[', key), end = object.find(']', begin);
    if(begin == std::string::npos || end == std::string::npos) throw std::runtime_error("invalid pack cards");
    std::vector<uint32_t> cards; size_t cursor = begin + 1;
    while(cursor < end) {
        while(cursor < end && !std::isdigit(static_cast<unsigned char>(object[cursor]))) ++cursor;
        if(cursor == end) break;
        size_t finish = cursor; while(finish < end && std::isdigit(static_cast<unsigned char>(object[finish]))) ++finish;
        cards.push_back(static_cast<uint32_t>(std::stoul(object.substr(cursor, finish - cursor)))); cursor = finish;
    }
    if(cards.empty()) throw std::runtime_error("pack needs at least one card");
    return cards;
}
}

const Npc& Catalog::npc(const std::string& id) const {
    for(const auto& value : npcs) if(value.id == id) return value;
    throw std::runtime_error("unknown NPC: " + id);
}
const Pack& Catalog::pack(const std::string& id) const {
    for(const auto& value : packs) if(value.id == id) return value;
    throw std::runtime_error("unknown pack: " + id);
}
Catalog load_catalog(const std::string& npc_filename, const std::string& pack_filename) {
    Catalog catalog;
    for(const auto& object : objects(read_all(npc_filename))) {
        Npc npc; npc.id = string_field(object, "id"); npc.name = string_field(object, "name");
        npc.difficulty = int_field(object, "difficulty"); npc.deck_path = string_field(object, "deck");
        npc.reward.credits = int_field(object, "credits"); npc.reward.pack_id = string_field(object, "pack");
        npc.tier = int_field_or(object, "tier", 1);
        npc.agent = string_field_or(object, "agent", "random");
        catalog.npcs.push_back(std::move(npc));
    }
    for(const auto& object : objects(read_all(pack_filename))) {
        Pack pack; pack.id = string_field(object, "id"); pack.name = string_field(object, "name");
        pack.price = int_field(object, "price"); pack.cards_per_pack = int_field(object, "cardsPerPack"); pack.art = string_field(object, "art"); pack.pool = card_list(object);
        pack.required_tier = int_field_or(object, "requiredTier", 1);
        catalog.packs.push_back(std::move(pack));
    }
    if(catalog.npcs.empty() || catalog.packs.empty()) throw std::runtime_error("catalog cannot be empty");
    return catalog;
}
} // namespace goat::game
