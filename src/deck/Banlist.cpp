#include "Banlist.hpp"

#include <fstream>
#include <stdexcept>
#include <string>

namespace goat {

Banlist Banlist::load(const std::filesystem::path& path) {
    std::ifstream input(path);
    if(!input) throw std::runtime_error("cannot open banlist: " + path.string());
    Banlist banlist;
    std::string line;
    while(std::getline(input, line)) {
        if(line == "$whitelist") { banlist.whitelist_ = true; continue; }
        if(line.empty() || line[0] == '#' || line[0] == '!') continue;
        const auto comment = line.find(" --");
        const auto values = line.substr(0, comment);
        const auto separator = values.find_first_of(" \t");
        if(separator == std::string::npos) continue;
        const auto code = static_cast<uint32_t>(std::stoul(values.substr(0, separator)));
        const auto limit = static_cast<uint8_t>(std::stoul(values.substr(values.find_first_not_of(" \t", separator))));
        banlist.limits_[code] = limit;
    }
    return banlist;
}

void Banlist::validate_main_deck(const std::vector<uint32_t>& codes, bool allow_illegal) const {
    if(allow_illegal) return;
    std::unordered_map<uint32_t, uint8_t> copies;
    for(const auto code : codes) {
        const auto count = ++copies[code];
        const auto entry = limits_.find(code);
        const auto limit = entry == limits_.end() ? (whitelist_ ? 0 : 3) : entry->second;
        if(count > limit) throw std::runtime_error("GOAT banlist violation for card " + std::to_string(code));
    }
}

} // namespace goat
