#pragma once

#include <cstdint>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace goat {

class Banlist {
public:
    static Banlist load(const std::filesystem::path& path);
    void validate_main_deck(const std::vector<uint32_t>& codes, bool allow_illegal) const;

private:
    bool whitelist_{};
    std::unordered_map<uint32_t, uint8_t> limits_;
};

} // namespace goat
