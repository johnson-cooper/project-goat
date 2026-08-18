#include "CardDatabase.hpp"

#include <sqlite3.h>

#include <stdexcept>
#include <string_view>

namespace {

void check(int status, sqlite3* db, const char* action) {
    if(status != SQLITE_OK) throw std::runtime_error(std::string(action) + ": " + sqlite3_errmsg(db));
}

} // namespace

namespace goat {

CardDatabase::CardDatabase(const std::filesystem::path& cards_cdb, const std::filesystem::path& goat_entries_cdb) {
    check(sqlite3_open_v2(cards_cdb.string().c_str(), &cards_, SQLITE_OPEN_READONLY, nullptr), cards_, "open cards.cdb");
    check(sqlite3_open_v2(goat_entries_cdb.string().c_str(), &goat_, SQLITE_OPEN_READONLY, nullptr), goat_, "open goat-entries.cdb");
}

CardDatabase::~CardDatabase() {
    if(cards_) sqlite3_close(cards_);
    if(goat_) sqlite3_close(goat_);
}

uint32_t CardDatabase::goat_code_for(uint32_t requested_code) const {
    sqlite3_stmt* stmt{};
    check(sqlite3_prepare_v2(goat_, "SELECT id FROM datas WHERE alias = ?1 LIMIT 1", -1, &stmt, nullptr), goat_, "prepare GOAT lookup");
    sqlite3_bind_int64(stmt, 1, requested_code);
    const int status = sqlite3_step(stmt);
    const uint32_t resolved = status == SQLITE_ROW ? static_cast<uint32_t>(sqlite3_column_int64(stmt, 0)) : requested_code;
    sqlite3_finalize(stmt);
    return resolved;
}

CardDefinition CardDatabase::read_card(sqlite3* database, uint32_t requested_code, uint32_t stored_code, bool goat_compatibility) const {
    sqlite3_stmt* stmt{};
    check(sqlite3_prepare_v2(database, "SELECT alias,setcode,type,atk,def,level,race,attribute FROM datas WHERE id = ?1", -1, &stmt, nullptr), database, "prepare card lookup");
    sqlite3_bind_int64(stmt, 1, stored_code);
    if(sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); throw std::runtime_error("missing card data for " + std::to_string(stored_code)); }

    CardDefinition card{};
    card.requested_code = requested_code;
    card.code = stored_code;
    card.alias = static_cast<uint32_t>(sqlite3_column_int64(stmt, 0));
    uint64_t setcode = static_cast<uint64_t>(sqlite3_column_int64(stmt, 1));
    for(int shift=0; shift<64; shift+=16) { const auto value=static_cast<uint16_t>((setcode >> shift) & 0xffffu); if(value) card.setcodes.push_back(value); }
    card.setcodes.push_back(0); // OCG_CardData uses a zero-terminated setcode array.
    card.type = static_cast<uint32_t>(sqlite3_column_int64(stmt, 2));
    card.attack = sqlite3_column_int(stmt, 3);
    card.defense = sqlite3_column_int(stmt, 4);
    card.level = static_cast<uint32_t>(sqlite3_column_int64(stmt, 5));
    card.race = static_cast<uint64_t>(sqlite3_column_int64(stmt, 6));
    card.attribute = static_cast<uint32_t>(sqlite3_column_int64(stmt, 7));
    card.goat_compatibility = goat_compatibility;
    sqlite3_finalize(stmt);

    check(sqlite3_prepare_v2(database, "SELECT name,desc FROM texts WHERE id = ?1", -1, &stmt, nullptr), database, "prepare card-text lookup");
    sqlite3_bind_int64(stmt, 1, stored_code);
    if(sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if(name) card.name = name;
        const auto* description = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        if(description) card.text = description;
    }
    sqlite3_finalize(stmt);
    // Every row in goat-entries.cdb suffixes its name with literal " (GOAT)"
    // to distinguish it from the modern printing in the tooling that curates
    // that database; it's not meant for display (nothing else in this project
    // uses that convention), so strip it here rather than showing "Card Name
    // (GOAT)" everywhere the resolved name is used.
    constexpr std::string_view goat_suffix = " (GOAT)";
    if(card.name.size() > goat_suffix.size() && card.name.compare(card.name.size() - goat_suffix.size(), goat_suffix.size(), goat_suffix) == 0)
        card.name.resize(card.name.size() - goat_suffix.size());
    if(card.name.empty()) card.name = std::to_string(stored_code);
    return card;
}

const CardDefinition& CardDatabase::resolve(uint32_t requested_code) {
    if(auto cached=cache_.find(requested_code); cached != cache_.end()) return cached->second;
    const uint32_t goat_code = goat_code_for(requested_code);
    try {
        return cache_.emplace(requested_code, read_card(goat_, requested_code, goat_code, goat_code != requested_code)).first->second;
    } catch(const std::runtime_error&) {
        return cache_.emplace(requested_code, read_card(cards_, requested_code, requested_code, false)).first->second;
    }
}

bool CardDatabase::contains(uint32_t requested_code) {
    try { resolve(requested_code); return true; } catch(const std::runtime_error&) { return false; }
}

} // namespace goat
