#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct sqlite3;

namespace goat {

struct CardDefinition {
    uint32_t requested_code{};
    uint32_t code{};
    uint32_t alias{};
    uint32_t type{};
    uint32_t level{};
    uint32_t attribute{};
    uint64_t race{};
    int32_t attack{};
    int32_t defense{};
    std::vector<uint16_t> setcodes;
    std::string name;
    std::string text;
    bool goat_compatibility{};
};

class CardDatabase {
public:
    CardDatabase(const std::filesystem::path& cards_cdb, const std::filesystem::path& goat_entries_cdb);
    ~CardDatabase();
    CardDatabase(const CardDatabase&) = delete;
    CardDatabase& operator=(const CardDatabase&) = delete;

    const CardDefinition& resolve(uint32_t requested_code);
    bool contains(uint32_t requested_code);

    // Every card id in the pinned cards.cdb's main `datas` table — the
    // search space for "declare a card name satisfying condition X" prompts
    // (MSG_ANNOUNCE_CARD; see src/ai/AnnounceCardSolver.cpp). Queried once
    // and cached, since it's a full-table scan.
    const std::vector<uint32_t>& all_codes();

private:
    sqlite3* cards_{};
    sqlite3* goat_{};
    std::unordered_map<uint32_t, CardDefinition> cache_;
    std::vector<uint32_t> all_codes_cache_;

    uint32_t goat_code_for(uint32_t requested_code) const;
    CardDefinition read_card(sqlite3* database, uint32_t requested_code, uint32_t stored_code, bool goat_compatibility) const;
};

} // namespace goat
