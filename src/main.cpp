#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cards/CardDatabase.hpp"
#include "deck/Banlist.hpp"

extern "C" {
#include "ocgapi.h"
#include "ocgapi_constants.h"
}

namespace fs = std::filesystem;

struct Deck { std::vector<uint32_t> main; std::vector<uint32_t> extra; };
// Trims trailing CR/whitespace so both Unix-LF and Windows-CRLF .ydk files
// parse identically, and so a stray trailing space on a "#main"/"#extra"
// marker line (common from deck editors) doesn't silently break the exact
// string match below.
static std::string rtrim(std::string text) {
 while(!text.empty() && (text.back()=='\r' || text.back()==' ' || text.back()=='\t')) text.pop_back();
 return text;
}
static Deck load_deck(const fs::path& path, goat::CardDatabase& database, const goat::Banlist& banlist, bool allow_illegal) {
 std::ifstream in(path); if(!in) throw std::runtime_error("cannot open deck: " + path.string());
 Deck deck; std::string line; enum class Section { None, Main, Extra } section = Section::None;
 while(std::getline(in, line)) {
  line = rtrim(line);
  if(line == "#main") { section = Section::Main; continue; }
  if(line == "#extra") { section = Section::Extra; continue; }
  if(!line.empty() && (line[0] == '#' || line[0] == '!')) { section = Section::None; continue; }
  if(line.empty()) continue;
  const uint32_t id = static_cast<uint32_t>(std::stoul(line));
  if(section == Section::Main) deck.main.push_back(database.resolve(id).code);
  else if(section == Section::Extra) deck.extra.push_back(database.resolve(id).code);
 }
 if(deck.main.size() < 40 || deck.main.size() > 60) throw std::runtime_error("GOAT main deck must contain 40-60 cards");
 if(deck.extra.size() > 15) throw std::runtime_error("GOAT extra deck cannot exceed 15 cards");
 banlist.validate_main_deck(deck.main, allow_illegal);
 banlist.validate_main_deck(deck.extra, allow_illegal);
 return deck;
}

struct Context {
 fs::path scripts;
 goat::CardDatabase database;
 Context(fs::path script_path, const fs::path& cards, const fs::path& goat_entries) : scripts(std::move(script_path)), database(cards, goat_entries) {}
};
static goat::CardDatabase* g_database = nullptr;
static bool g_quiet = false;
static std::optional<fs::path> g_result_file;
static void card_reader(void* payload, uint32_t code, OCG_CardData* data) {
 if(code == 0) return; // ygopro-core uses an internal temporary card without database metadata.
 auto& card = static_cast<Context*>(payload)->database.resolve(code);
 data->code=card.code; data->alias=card.alias; data->setcodes=card.setcodes.empty() ? nullptr : const_cast<uint16_t*>(card.setcodes.data()); data->type=card.type; data->level=card.level; data->attribute=card.attribute; data->race=card.race; data->attack=card.attack; data->defense=card.defense;
}
static int script_reader(void* payload, OCG_Duel duel, const char* name) {
 auto* ctx = static_cast<Context*>(payload); fs::path file = ctx->scripts / name;
 if(name[0] == 'c' && name[1] >= '0' && name[1] <= '9') {
  const auto code = static_cast<uint32_t>(std::strtoul(name + 1, nullptr, 10));
  if(code == 0) return 1; // Internal temporary card: it has no script or database row.
  if(ctx->database.resolve(code).type & TYPE_NORMAL) return 1;
 }
 if(!fs::exists(file)) file = ctx->scripts / "official" / name;
 // Some cards' only implementation (or their GOAT-accurate ruling) lives in
 // the goat/ subdirectory rather than the root or official/ directories.
 if(!fs::exists(file)) file = ctx->scripts / "goat" / name;
 if(!fs::exists(file)) return 1; // Normal monsters deliberately have no Lua script.
 std::ifstream in(file, std::ios::binary); std::string source((std::istreambuf_iterator<char>(in)), {});
 return OCG_LoadScript(duel, source.data(), static_cast<uint32_t>(source.size()), name);
}
static void logger(void*, const char* message, int) { std::cerr << "engine: " << message << '\n'; }

template<class T> static T read(const uint8_t*& p, const uint8_t* end) {
 if(static_cast<size_t>(end-p) < sizeof(T)) throw std::runtime_error("truncated engine message"); T value; std::memcpy(&value,p,sizeof(T)); p += sizeof(T); return value;
}
static std::string name(uint32_t code) { return g_database ? g_database->resolve(code).name : std::to_string(code); }
using Response = std::array<uint8_t,64>;

class RandomAgent {
public:
 explicit RandomAgent(int human_player = -1, std::optional<fs::path> decision_directory = std::nullopt) : human_player_(human_player), decision_directory_(std::move(decision_directory)) {}
 // `chain_context_code` is the card currently on top of the resolving chain
 // (0 if none), as tracked by the caller from MSG_CHAINING/MSG_CHAIN_SOLVED —
 // several decision prompts (generic Yes/No, "choose an effect option",
 // "confirm" with no chain-response choices) carry no card identity of their
 // own in the engine's wire format, so this is the best available context to
 // show the player what they're actually being asked about.
 Response choose(const uint8_t* data, size_t length, uint32_t chain_context_code = 0) {
  const uint8_t *p=data, *end=data+length; auto kind=read<uint8_t>(p,end);
  if(human_player_ >= 0 && length > 1 && data[1] == human_player_) {
   if(kind == MSG_SELECT_IDLECMD) return choose_human_idle(p,end);
   if(kind == MSG_SELECT_BATTLECMD) return choose_human_battle(p,end);
  }
  if(kind == MSG_SELECT_IDLECMD) {
   // Prefer a legal normal summon; otherwise attack, then end the phase.
   // The command format follows Project Ignis' OCG message protocol.
   read<uint8_t>(p,end); uint32_t summon=read<uint32_t>(p,end);
   for(uint32_t i=0;i<summon;i++) {
    const auto code=read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end);
    // Keep the baseline CPU strategy legal without yet choosing tribute fodder.
    if((g_database->resolve(code).level & 0xffu) <= 4) return response(0,i);
   }
   uint32_t special=read<uint32_t>(p,end); skip_cards(p,end,special);
   uint32_t reposition=read<uint32_t>(p,end);
   for(uint32_t i=0;i<reposition;i++) { read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); }
   uint32_t set=read<uint32_t>(p,end); skip_cards(p,end,set);
   uint32_t stset=read<uint32_t>(p,end); skip_cards(p,end,stset);
   uint32_t activate=read<uint32_t>(p,end); skip_effects(p,end,activate);
   bool battle=read<uint8_t>(p,end); bool endphase=read<uint8_t>(p,end);
   return response(battle ? 6 : (endphase ? 7 : 8),0);
  }
  if(kind == MSG_SELECT_BATTLECMD) {
   read<uint8_t>(p,end); auto effects=read<uint32_t>(p,end); skip_effects(p,end,effects);
   auto attackers=read<uint32_t>(p,end); return response(attackers ? 1 : 3,0);
  }
  if(kind == MSG_SELECT_CHAIN) {
   const auto player = read<uint8_t>(p,end); read<uint8_t>(p,end) /*spe_count*/; const auto forced = read<uint8_t>(p,end);
   read<uint32_t>(p,end); read<uint32_t>(p,end); // hint_timing for each player: engine/UI hint metadata, not needed here.
   const auto count = read<uint32_t>(p,end);
   std::vector<uint32_t> codes; codes.reserve(count);
   for(uint32_t i=0;i<count;++i) {
    const auto code=read<uint32_t>(p,end);
    read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end); read<uint32_t>(p,end); // loc_info: controler,location,sequence,position
    read<uint64_t>(p,end); read<uint8_t>(p,end); // effect description, client mode
    codes.push_back(code);
   }
   if(decision_directory_ && player == human_player_) {
    std::vector<std::pair<std::string,Response>> choices;
    for(uint32_t i=0;i<count;++i) choices.emplace_back("Activate "+name(codes[i])+" ["+image(codes[i])+"]", raw_response(static_cast<int32_t>(i)));
    if(!forced) choices.emplace_back(count>0 ? "Pass (don't respond)" : "Continue", raw_response(-1));
    if(choices.empty()) return raw_response(-1);
    return choose_menu((count>0 ? "You may respond with a chain link" : "Confirm") + context_suffix(chain_context_code), choices);
   }
   return raw_response(-1); // CPU declines optional chain responses (baseline strategy unchanged).
  }
  if(kind == MSG_SELECT_YESNO) {
   // The engine's own wire format for this message carries no card identity
   // at all (see field::process(Processors::SelectYesNo&) in playerop.cpp:
   // just player + a description id with no strings.conf text available
   // here) — "Confirm effect" alone left the player guessing what they were
   // actually being asked. `chain_context_code`, the card on top of the
   // currently-resolving chain, is the best identifying context available.
   const auto player = read<uint8_t>(p,end); read<uint64_t>(p,end); // description: engine/card-script metadata, no lookup table available client-side.
   if(decision_directory_ && player == human_player_) {
    return choose_menu("Confirm effect" + context_suffix(chain_context_code), {{"Yes", raw_response(1)}, {"No", raw_response(0)}});
   }
   return raw_response(0); // CPU declines optional confirmations unless its strategy is expanded.
  }
  if(kind == MSG_SELECT_EFFECTYN) {
   // "Do you want to use this specific card's effect?" — distinct from the
   // generic MSG_SELECT_YESNO above, but the same response shape (0/1). This
   // was previously unhandled entirely, so any card whose script asks this
   // (a very common pattern — flip effects, "you may" triggers, etc.) threw
   // immediately and ended the duel.
   const auto player = read<uint8_t>(p,end); const auto code = read<uint32_t>(p,end);
   read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end); read<uint32_t>(p,end); // loc_info
   read<uint64_t>(p,end); // effect description
   if(decision_directory_ && player == human_player_) {
    return choose_menu("Use " + name(code) + "'s effect?", {{"Yes", raw_response(1)}, {"No", raw_response(0)}});
   }
   return raw_response(0);
  }
  if(kind == MSG_SORT_CARD || kind == MSG_SORT_CHAIN) {
   // Purely cosmetic ordering (e.g. which order simultaneous triggers resolve
   // in when it doesn't affect legality); -1 declines and keeps the engine's
   // own default order, which is always a valid response for anyone.
   return raw_response(-1);
  }
  if(kind == MSG_SELECT_OPTION) {
   // field::process(Processors::SelectOption&) in playerop.cpp writes count as
   // a single uint8_t and each option's description as a uint64_t. This used
   // to read them as uint32_t/uint32_t, which desynced the parse: it consumed
   // 3 extra bytes into `count` (turning it into a huge garbage value) and
   // then read half of each 8-byte description, so the loop ran past the end
   // of the message and `read<T>` threw "truncated engine message" — for
   // *either* player's decision, since this runs before the human-player
   // check below. Any card effect that presents an option choice (not
   // uncommon — e.g. "choose which effect to apply") crashed the whole duel
   // process outright, which is what a duel ending abruptly for no visible
   // reason looked like from the client.
   const auto player = read<uint8_t>(p,end); const auto count = read<uint8_t>(p,end);
   std::vector<std::pair<std::string,Response>> choices;
   choices.reserve(count);
   for(uint8_t i=0;i<count;++i) { const auto description = read<uint64_t>(p,end); choices.emplace_back("Effect option " + std::to_string(i + 1) + " (" + std::to_string(description) + ")", raw_response(static_cast<int32_t>(i))); }
   if(choices.empty()) throw std::runtime_error("empty option-selection prompt");
   if(decision_directory_ && player == human_player_) return choose_menu("Choose an effect option" + context_suffix(chain_context_code), choices);
   return raw_response(0);
  }
  if(kind == MSG_SELECT_PLACE || kind == MSG_SELECT_DISFIELD) {
   // Both messages come from the exact same engine processor (SelectPlace)
   // with an identical payload and response format — MSG_SELECT_DISFIELD is
   // just the "this zone is being disabled" flavor of the same prompt.
   const auto player = read<uint8_t>(p,end); const auto count = read<uint8_t>(p,end); const auto flag = read<uint32_t>(p,end);
   if(count != 1) throw std::runtime_error("multi-place selection is not supported by Phase 1");
   // `flag` packs both which zone type this is (monster vs spell/trap) and
   // whose side (the deciding player's own field vs their opponent's, e.g.
   // for effects like Ojama Trio that place on the opponent's zones) into
   // different bit ranges, plus which slots in that range are blocked. The
   // first matching range determines what this prompt is actually asking for
   // — see field::process(Processors::SelectPlace&) in playerop.cpp. Treating
   // every MSG_SELECT_PLACE as "monster zone, own side" (the old behavior)
   // silently broke every non-Summon placement, e.g. Set Spell/Trap.
   // A range that isn't relevant to this particular prompt is packed with all
   // 1-bits (fully "blocked"), so a plain non-zero test on the raw flag can't
   // tell an irrelevant range from a relevant-but-mostly-blocked one; only the
   // *inverted* flag reliably has zero bits throughout an irrelevant range.
   // (Matches field::process(Processors::SelectPlace&)'s own `flag = ~flag`
   // range-detection in playerop.cpp — the per-zone blocked/available bits
   // themselves are read from the original, non-inverted flag.)
   const uint32_t detect = ~flag;
   uint8_t targetPlayer; uint8_t location; uint32_t blocked; const char* zoneNoun;
   if(detect & 0x7fu) { targetPlayer = player; location = LOCATION_MZONE; blocked = flag & 0x7fu; zoneNoun = "monster"; }
   else if(detect & 0x1f00u) { targetPlayer = player; location = LOCATION_SZONE; blocked = (flag >> 8) & 0x1fu; zoneNoun = "spell/trap"; }
   else if(detect & 0x7f0000u) { targetPlayer = static_cast<uint8_t>(1 - player); location = LOCATION_MZONE; blocked = (flag >> 16) & 0x7fu; zoneNoun = "monster"; }
   else if(detect & 0x1f000000u) { targetPlayer = static_cast<uint8_t>(1 - player); location = LOCATION_SZONE; blocked = (flag >> 24) & 0x1fu; zoneNoun = "spell/trap"; }
   else throw std::runtime_error("unsupported zone-placement flag (pendulum zone?)");
   if(decision_directory_ && player == human_player_) {
    std::vector<std::pair<std::string,Response>> choices;
    for(uint8_t seq = 0; seq < 5; ++seq) if((blocked & (1u << seq)) == 0) choices.emplace_back(std::string("Place in ") + zoneNoun + " zone " + std::to_string(seq + 1), place_response(targetPlayer, location, seq));
    return choose_menu(std::string("Choose a ") + zoneNoun + " zone", choices);
   }
   for(uint8_t seq = 0; seq < 5; ++seq) if((blocked & (1u << seq)) == 0) return place_response(targetPlayer, location, seq);
   throw std::runtime_error("no legal zone in place prompt");
  }
  if(kind == MSG_SELECT_TRIBUTE) {
   const auto player = read<uint8_t>(p,end); read<uint8_t>(p,end); const auto min = read<uint32_t>(p,end); read<uint32_t>(p,end);
   const auto choices = read<uint32_t>(p,end);
   if(min > choices) throw std::runtime_error("invalid tribute-selection prompt");
   std::vector<std::string> labels;
   labels.reserve(choices);
   for(uint32_t i=0;i<choices;++i) { const auto code=read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end); read<uint8_t>(p,end); labels.push_back(name(code)); }
   if(decision_directory_ && player == human_player_ && min > 0 && min <= 14 && choices <= 12) {
    return choose_menu(min == 1 ? "Choose a tribute" : "Choose tributes", selection_menu(labels, min, "Tribute "));
   }
   return select_first(min);
  }
  if(kind == MSG_SELECT_CARD) {
   const auto player = read<uint8_t>(p,end); read<uint8_t>(p,end); const auto min = read<uint32_t>(p,end);
   read<uint32_t>(p,end); const auto choices = read<uint32_t>(p,end);
   if(min > choices) throw std::runtime_error("invalid card-selection prompt");
   std::vector<std::string> labels;
   labels.reserve(choices);
   for(uint32_t i=0;i<choices;++i) { const auto code=read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end); read<uint32_t>(p,end); labels.push_back(name(code)); }
   if(decision_directory_ && player == human_player_ && min > 0 && min <= 14 && choices <= 12) {
    return choose_menu(min == 1 ? "Choose a card" : "Choose cards", selection_menu(labels, min, "Select "));
   }
   return select_first(min);
  }
  if(kind == MSG_SELECT_POSITION) {
   const auto player = read<uint8_t>(p,end); const auto code = read<uint32_t>(p,end); const auto positions = read<uint8_t>(p,end);
   if(decision_directory_ && player == human_player_) {
    std::vector<std::pair<std::string,Response>> choices;
    for(const auto& [position, label] : std::array<std::pair<uint8_t,const char*>,4>{{{uint8_t(POS_FACEUP_ATTACK), "Face-up attack"}, {uint8_t(POS_FACEUP_DEFENSE), "Face-up defense"}, {uint8_t(POS_FACEDOWN_DEFENSE), "Set face-down defense"}, {uint8_t(POS_FACEDOWN_ATTACK), "Set face-down attack"}}}) if(positions & position) choices.emplace_back(label, raw_response(position));
    return choose_menu("Choose battle position for " + name(code), choices);
   }
   for(uint8_t pos : {uint8_t(POS_FACEUP_ATTACK), uint8_t(POS_FACEUP_DEFENSE), uint8_t(POS_FACEDOWN_DEFENSE), uint8_t(POS_FACEDOWN_ATTACK)}) if(positions & pos) return raw_response(pos);
  }
  if(kind == MSG_SELECT_COUNTER) {
   // Distributing N counters (e.g. Spell Counters) to remove across however
   // many cards currently hold them — rare in GOAT (mainly Breaker-style
   // cards), and the engine already auto-resolves the common single-card
   // case internally without sending this message at all. For the rarer
   // multi-card case, greedily take from the front of the (sorted) list;
   // always a legal distribution, just not an interactive choice of *which*
   // card to draw from when more than one holds enough.
   read<uint8_t>(p,end); read<uint16_t>(p,end); const auto count = read<uint16_t>(p,end);
   const auto cardCount = read<uint32_t>(p,end);
   std::vector<uint16_t> available; available.reserve(cardCount);
   for(uint32_t i=0;i<cardCount;++i) { read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); available.push_back(read<uint16_t>(p,end)); }
   std::vector<uint16_t> take(cardCount,0); uint16_t remaining=count;
   for(uint32_t i=0;i<cardCount && remaining>0;++i) { const uint16_t use=std::min(available[i],remaining); take[i]=use; remaining-=use; }
   return select_counter_response(take);
  }
  if(kind == MSG_SELECT_SUM) {
   // "Select cards whose values sum to a target" (e.g. Level/ATK-sum costs).
   // Rare in GOAT-era scripts. Every must-select card is mandatory; greedily
   // add optional cards after that until the running total first reaches the
   // legal [min,max] sum window — a safe, always-attempted default rather
   // than solving the general subset-sum problem for an interactive menu.
   read<uint8_t>(p,end); read<uint8_t>(p,end) /*mode*/; read<uint32_t>(p,end) /*target*/;
   const auto min = read<uint32_t>(p,end); const auto max = read<uint32_t>(p,end);
   const auto mustCount = read<uint32_t>(p,end);
   std::vector<uint32_t> indices; std::vector<uint32_t> sumParams;
   for(uint32_t i=0;i<mustCount;++i) { read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end); read<uint32_t>(p,end); const auto sum=read<uint32_t>(p,end); indices.push_back(i); sumParams.push_back(sum); }
   const auto optionalCount = read<uint32_t>(p,end);
   uint32_t running=0; for(const auto s : sumParams) running += (s & 0xffffu);
   for(uint32_t i=0;i<optionalCount;++i) {
    read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end); read<uint32_t>(p,end); const auto sum=read<uint32_t>(p,end);
    if(running < max) { indices.push_back(mustCount+i); running += (sum & 0xffffu); }
   }
   (void)min;
   return select_indices(indices);
  }
  if(kind == MSG_SELECT_UNSELECT_CARD) {
   const auto player = read<uint8_t>(p,end); const auto finishable = read<uint8_t>(p,end); const auto cancelable = read<uint8_t>(p,end);
   read<uint32_t>(p,end); read<uint32_t>(p,end); // min, max (informational)
   const auto selectableCount = read<uint32_t>(p,end);
   std::vector<uint32_t> selectable; selectable.reserve(selectableCount);
   for(uint32_t i=0;i<selectableCount;++i) { const auto code=read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end); read<uint32_t>(p,end); selectable.push_back(code); }
   const auto selectedCount = read<uint32_t>(p,end);
   std::vector<uint32_t> already; already.reserve(selectedCount);
   for(uint32_t i=0;i<selectedCount;++i) { const auto code=read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end); read<uint32_t>(p,end); already.push_back(code); }
   if(decision_directory_ && player == human_player_) {
    std::vector<std::pair<std::string,Response>> choices;
    for(uint32_t i=0;i<selectableCount;++i) choices.emplace_back("Select "+name(selectable[i])+" ["+image(selectable[i])+"]", select_unselect_response(1,static_cast<int32_t>(i)));
    for(uint32_t i=0;i<selectedCount;++i) choices.emplace_back("Unselect "+name(already[i])+" ["+image(already[i])+"]", select_unselect_response(1,static_cast<int32_t>(selectableCount+i)));
    if(finishable) choices.emplace_back("Finish selection", select_unselect_response(-1,0));
    else if(cancelable) choices.emplace_back("Cancel", select_unselect_response(-1,0));
    if(choices.empty()) return select_unselect_response(-1,0);
    return choose_menu("Choose cards", choices);
   }
   if(finishable || cancelable) return select_unselect_response(-1,0);
   if(selectableCount>0) return select_unselect_response(1,0);
   return select_unselect_response(-1,0);
  }
  // A nonzero default is intentionally rejected by the engine, exposing
  // unsupported protocol instead of silently cheating.
  throw std::runtime_error("unsupported decision message " + std::to_string(kind));
 }
private:
 int human_player_;
 std::optional<fs::path> decision_directory_;
 // Always embeds a path whose filename stem is exactly `code` (the board's
 // real identity), regardless of whether that file exists — the bracketed
 // text is never shown to the user (action_caption strips it before display),
 // it only exists so the client can parse the code back out. Returning "no
 // local image" for a missing file used to make that parse fail and silently
 // fall back to a plain action-list entry for every such card, instead of the
 // usual click-the-hand-card/click-the-zone/click-the-attacker handling. Any
 // missing-art fallback (e.g. via a GOAT-entries alias) belongs client-side.
 static std::string image(uint32_t code) { return (fs::path("external/card_images")/(std::to_string(code)+".jpg")).string(); }
 static std::string context_suffix(uint32_t code) { return code ? " — " + name(code) : std::string(); }
 Response choose_menu(const std::string& title, const std::vector<std::pair<std::string,Response>>& choices) const {
  if(decision_directory_) {
   fs::create_directories(*decision_directory_);
   const auto request = *decision_directory_ / "request.txt";
   const auto temporary = *decision_directory_ / "request.tmp";
   const auto response_file = *decision_directory_ / "response.txt";
   std::error_code ignored; fs::remove(response_file, ignored);
   { std::ofstream out(temporary, std::ios::trunc); out << title << '\n'; for(size_t i=0;i<choices.size();++i) out << i << '|' << choices[i].first << '\n'; }
   fs::rename(temporary, request, ignored); if(ignored) throw std::runtime_error("cannot publish player decision request");
   for(int wait=0;wait<6000;++wait) {
    if(fs::exists(response_file)) { std::ifstream in(response_file); size_t choice=choices.size(); in >> choice; fs::remove(response_file, ignored); fs::remove(request, ignored); if(choice < choices.size()) return choices[choice].second; throw std::runtime_error("invalid graphical action index"); }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
   }
   throw std::runtime_error("player decision timed out");
  }
  for(size_t i=0;i<choices.size();++i) std::cout << "  [" << i << "] " << choices[i].first << '\n';
  std::cout << "Choose action: "; size_t choice=choices.size(); std::cin >> choice;
  if(choice >= choices.size()) throw std::runtime_error("invalid human action index");
  return choices[choice].second;
 }
 Response choose_human_idle(const uint8_t*& p,const uint8_t* end) const {
  const auto player=read<uint8_t>(p,end); std::vector<std::pair<std::string,Response>> choices;
  const char* verbs[] = {"Summon", "Special summon", "Change position", "Set monster", "Set spell/trap"};
  for(uint16_t kind=0;kind<5;++kind) {
   const auto count=read<uint32_t>(p,end);
   for(uint32_t index=0;index<count;++index) { auto code=read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); if(kind==2) read<uint8_t>(p,end); else read<uint32_t>(p,end); choices.emplace_back(std::string(verbs[kind])+" "+name(code)+" ["+image(code)+"]",response(kind,index)); }
  }
  const auto activations=read<uint32_t>(p,end);
  for(uint32_t index=0;index<activations;++index) { auto code=read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end); read<uint64_t>(p,end); read<uint8_t>(p,end); choices.emplace_back("Activate "+name(code)+" ["+image(code)+"]",response(5,index)); }
  const auto battle=read<uint8_t>(p,end); const auto end_phase=read<uint8_t>(p,end); read<uint8_t>(p,end);
  if(battle) choices.emplace_back("Enter Battle Phase",response(6,0));
  if(end_phase) choices.emplace_back("End Phase",response(7,0));
  std::cout << "\nPlayer " << int(player+1) << " Main Phase — legal actions:\n";
  return choose_menu("Main Phase", choices);
 }
 Response choose_human_battle(const uint8_t*& p,const uint8_t* end) const {
  const auto player=read<uint8_t>(p,end); std::vector<std::pair<std::string,Response>> choices;
  // Effects activatable during Battle Phase (ignition/quick effects, set traps,
  // etc.) use the same 6-field layout as the Main Phase idle-command's
  // activation list; this used to be discarded via skip_effects, which meant
  // there was never any way to activate anything during Battle Phase.
  const auto effects=read<uint32_t>(p,end);
  for(uint32_t index=0;index<effects;++index) { auto code=read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end); read<uint64_t>(p,end); read<uint8_t>(p,end); choices.emplace_back("Activate "+name(code)+" ["+image(code)+"]",response(0,index)); }
  auto attackers=read<uint32_t>(p,end);
  for(uint32_t index=0;index<attackers;++index) { auto code=read<uint32_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint8_t>(p,end); choices.emplace_back("Attack with "+name(code)+" ["+image(code)+"]",response(1,index)); }
  const auto main2=read<uint8_t>(p,end); const auto end_phase=read<uint8_t>(p,end); if(main2) choices.emplace_back("Main Phase 2",response(2,0)); if(end_phase) choices.emplace_back("End Phase",response(3,0));
  std::cout << "\nPlayer " << int(player+1) << " Battle Phase — legal actions:\n"; return choose_menu("Battle Phase", choices);
 }
 static void skip_cards(const uint8_t*& p,const uint8_t* e,uint32_t n){ for(uint32_t i=0;i<n;i++){read<uint32_t>(p,e);read<uint8_t>(p,e);read<uint8_t>(p,e);read<uint32_t>(p,e);} }
 static void skip_effects(const uint8_t*& p,const uint8_t* e,uint32_t n){ for(uint32_t i=0;i<n;i++){read<uint32_t>(p,e);read<uint8_t>(p,e);read<uint8_t>(p,e);read<uint32_t>(p,e);read<uint64_t>(p,e);read<uint8_t>(p,e);} }
 static Response response(uint16_t type,uint16_t index){ Response out{}; uint32_t r=uint32_t(type)|(uint32_t(index)<<16); std::memcpy(out.data(),&r,4); return out; }
 static Response raw_response(int32_t r){ Response out{}; std::memcpy(out.data(),&r,4); return out; }
 static Response place_response(uint8_t player,uint8_t location,uint8_t sequence){ Response out{}; out[0]=player; out[1]=location; out[2]=sequence; return out; }
 static Response select_indices(const std::vector<uint32_t>& indices){ Response out{}; const auto count=static_cast<uint32_t>(indices.size()); std::memcpy(out.data()+4,&count,4); for(uint32_t i=0;i<count && 8+i*4+4<=out.size();++i) std::memcpy(out.data()+8+i*4,&indices[i],4); return out; }
 static Response select_first(uint32_t count){ std::vector<uint32_t> indices; for(uint32_t i=0;i<count;++i) indices.push_back(i); return select_indices(indices); }
 static Response select_counter_response(const std::vector<uint16_t>& amounts){ Response out{}; for(size_t i=0;i<amounts.size() && i*2+2<=out.size();++i) std::memcpy(out.data()+i*2,&amounts[i],2); return out; }
 static Response select_unselect_response(int32_t action,int32_t index){ Response out{}; std::memcpy(out.data(),&action,4); std::memcpy(out.data()+4,&index,4); return out; }
 static std::vector<std::pair<std::string,Response>> selection_menu(const std::vector<std::string>& labels, uint32_t required, const std::string& verb) {
  std::vector<std::pair<std::string,Response>> menu;
  std::vector<uint32_t> selected;
  auto visit = [&](auto&& self, size_t first) -> void {
   if(menu.size() >= 96) return;
   if(selected.size() == required) {
    std::string label = verb;
    for(size_t i=0;i<selected.size();++i) { if(i) label += " + "; label += labels[selected[i]]; }
    menu.emplace_back(std::move(label), select_indices(selected));
    return;
   }
   for(size_t i=first; i<labels.size() && labels.size()-i >= required-selected.size(); ++i) {
    selected.push_back(static_cast<uint32_t>(i));
    self(self, i + 1);
    selected.pop_back();
   }
  };
  visit(visit, 0);
  return menu;
 }
};

static void log_message(const uint8_t* data,size_t length) {
 const uint8_t *p=data,*end=data+length; auto kind=read<uint8_t>(p,end);
 if(g_quiet && kind != MSG_WIN) return;
 if(kind==MSG_NEW_TURN) std::cout << "\nTurn: Player " << int(read<uint8_t>(p,end)+1) << '\n';
 else if(kind==MSG_NEW_PHASE) std::cout << "Phase: 0x" << std::hex << read<uint16_t>(p,end) << std::dec << '\n';
 else if(kind==MSG_DRAW) { auto pl=read<uint8_t>(p,end); auto count=read<uint32_t>(p,end); std::cout << "Player " << int(pl+1) << " draws " << count << " card(s)\n"; }
 else if(kind==MSG_SUMMONED) std::cout << "A monster is summoned.\n";
 else if(kind==MSG_ATTACK) std::cout << "An attack is declared.\n";
 else if(kind==MSG_DAMAGE) { auto pl=read<uint8_t>(p,end); std::cout << "Player " << int(pl+1) << " takes " << read<int32_t>(p,end) << " damage\n"; }
 else if(kind==MSG_WIN) { auto pl=read<uint8_t>(p,end); auto reason=read<uint8_t>(p,end); std::cout << "Player " << int(pl+1) << " wins (reason " << int(reason) << ").\n"; }
}

struct BoardCard { uint32_t code{}; uint8_t position{}; };
// Spell/Trap zone sequence 5 is the classic-format Field Spell Zone; DUEL_MODE_GOAT
// predates Pendulum/Extra Monster Zones, so no other zone geometry needs representing.
struct BoardState {
 std::array<int32_t,2> life{{8000,8000}};
 std::array<uint32_t,2> hand{};
 std::array<std::array<BoardCard,5>,2> monsters{};
 std::array<std::array<BoardCard,6>,2> spells{};
 std::array<uint32_t,2> deck_count{};
 std::array<uint32_t,2> grave_count{};
 std::array<uint32_t,2> extra_count{};
 std::array<uint32_t,2> banished_count{};
 std::array<std::vector<uint32_t>,2> hand_cards{};
 uint8_t turn_player{};
 uint32_t turn_number{};
 uint16_t phase{PHASE_DRAW};
 // LIFO stack of the card codes currently on the chain, pushed on
 // MSG_CHAINING and popped on MSG_CHAIN_SOLVED — mirrors the engine's own
 // core.current_chain ordering, so .back() is always whichever card's effect
 // is actively resolving. Several decision prompts (generic Yes/No, "choose
 // an effect option") carry no card identity of their own in the engine's
 // wire format; this is the best available context for naming them to the
 // player instead of a bare "Confirm effect".
 std::vector<uint32_t> chain_stack{};
};
struct BoardLocation { uint8_t player{}; uint8_t location{}; uint32_t sequence{}; uint32_t position{}; };
static BoardLocation read_location(const uint8_t*& p, const uint8_t* end) { return {read<uint8_t>(p,end), read<uint8_t>(p,end), read<uint32_t>(p,end), read<uint32_t>(p,end)}; }
static void track_board_message(const uint8_t* data, size_t length, BoardState& board) {
 const uint8_t *p=data,*end=data+length; const auto kind=read<uint8_t>(p,end);
 if(kind == MSG_LPUPDATE) { const auto player=read<uint8_t>(p,end); if(player<2) board.life[player]=read<int32_t>(p,end); return; }
 if(kind == MSG_DAMAGE || kind == MSG_PAY_LPCOST) { const auto player=read<uint8_t>(p,end); if(player<2) board.life[player]-=read<int32_t>(p,end); return; }
 if(kind == MSG_RECOVER) { const auto player=read<uint8_t>(p,end); if(player<2) board.life[player]+=read<int32_t>(p,end); return; }
 if(kind == MSG_NEW_TURN) {
  board.turn_player=read<uint8_t>(p,end); ++board.turn_number;
  // Every turn starts in the Draw Phase, but that phase's own MSG_NEW_PHASE
  // is a separate message that can land in a later OCG_DuelProcess batch.
  // Without this, a state.txt snapshot written in that gap would show the
  // new turn/player alongside the *previous* turn's stale trailing phase
  // (e.g. still "End Phase"), which is what made the UI look glitchy right
  // at a turn change.
  board.phase=PHASE_DRAW;
  return;
 }
 if(kind == MSG_NEW_PHASE) { board.phase=read<uint16_t>(p,end); return; }
 if(kind == MSG_DRAW) {
  // Draws (the initial hand and every draw-phase draw) are reported via their
  // own message rather than MSG_MOVE, so hand identity tracking needs this
  // in addition to the generic move handling below.
  const auto player=read<uint8_t>(p,end); const auto count=read<uint32_t>(p,end);
  for(uint32_t i=0;i<count;i++) { const auto code=read<uint32_t>(p,end); read<uint32_t>(p,end); if(player<2) board.hand_cards[player].push_back(code); }
  return;
 }
 if(kind == MSG_MOVE) {
  const auto code=read<uint32_t>(p,end); const auto previous=read_location(p,end); const auto current=read_location(p,end);
  if(previous.player<2 && previous.location==LOCATION_MZONE && previous.sequence<5) board.monsters[previous.player][previous.sequence]={};
  if(previous.player<2 && previous.location==LOCATION_SZONE && previous.sequence<6) board.spells[previous.player][previous.sequence]={};
  if(previous.player<2 && previous.location==LOCATION_HAND) {
   auto& cards=board.hand_cards[previous.player]; if(const auto it=std::find(cards.begin(),cards.end(),code); it!=cards.end()) cards.erase(it);
   if(board.hand[previous.player]) --board.hand[previous.player];
  }
  if(current.player<2 && current.location==LOCATION_MZONE && current.sequence<5) board.monsters[current.player][current.sequence]={code,static_cast<uint8_t>(current.position)};
  if(current.player<2 && current.location==LOCATION_SZONE && current.sequence<6) board.spells[current.player][current.sequence]={code,static_cast<uint8_t>(current.position)};
  if(current.player<2 && current.location==LOCATION_HAND) { board.hand_cards[current.player].push_back(code); ++board.hand[current.player]; }
  return;
 }
 if(kind == MSG_POS_CHANGE) {
  read<uint32_t>(p,end); const auto player=read<uint8_t>(p,end); const auto location=read<uint8_t>(p,end); const auto sequence=read<uint8_t>(p,end); read<uint8_t>(p,end); const auto position=read<uint8_t>(p,end);
  if(player<2 && location==LOCATION_MZONE && sequence<5 && board.monsters[player][sequence].code) board.monsters[player][sequence].position=position;
  if(player<2 && location==LOCATION_SZONE && sequence<6 && board.spells[player][sequence].code) board.spells[player][sequence].position=position;
  return;
 }
 if(kind == MSG_CHAINING) {
  // field::process(...) case 2 in processor.cpp: code, loc_info, triggering
  // controler/location/sequence, description, chain size — see the write
  // order at processor.cpp's MSG_CHAINING block.
  const auto code=read<uint32_t>(p,end); read_location(p,end);
  read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end); read<uint64_t>(p,end); read<uint32_t>(p,end);
  board.chain_stack.push_back(code);
  return;
 }
 if(kind == MSG_CHAIN_SOLVED) { read<uint8_t>(p,end); if(!board.chain_stack.empty()) board.chain_stack.pop_back(); return; }
 if(kind == MSG_CHAIN_END) { board.chain_stack.clear(); return; }
}
static void write_board_state(const fs::path& directory, const BoardState& board, int human_player) {
 const auto temporary=directory/"state.tmp", output=directory/"state.txt"; std::ofstream out(temporary,std::ios::trunc);
 out << "lp=" << std::max(0, board.life[0]) << ',' << std::max(0, board.life[1]) << "\nhand=" << board.hand[0] << ',' << board.hand[1] << '\n';
 out << "deck=" << board.deck_count[0] << ',' << board.deck_count[1] << '\n';
 out << "grave=" << board.grave_count[0] << ',' << board.grave_count[1] << '\n';
 out << "extra=" << board.extra_count[0] << ',' << board.extra_count[1] << '\n';
 out << "banished=" << board.banished_count[0] << ',' << board.banished_count[1] << '\n';
 out << "turn=" << int(board.turn_player) << ',' << board.turn_number << ',' << board.phase << '\n';
 for(uint8_t player=0;player<2;++player) for(uint8_t sequence=0;sequence<5;++sequence) if(const auto& card=board.monsters[player][sequence]; card.code) {
  const auto visible_code=(player==1 && (card.position & POS_FACEDOWN)) ? 0u : card.code;
  out << "monster=" << int(player) << ',' << int(sequence) << ',' << visible_code << ',' << int(card.position) << '\n';
 }
 for(uint8_t player=0;player<2;++player) for(uint8_t sequence=0;sequence<6;++sequence) if(const auto& card=board.spells[player][sequence]; card.code) {
  const auto visible_code=(player==1 && (card.position & POS_FACEDOWN)) ? 0u : card.code;
  out << "spell=" << int(player) << ',' << int(sequence) << ',' << visible_code << ',' << int(card.position) << '\n';
 }
 // Only the human player's own hand identities are ever written: the opponent's
 // hand must stay hidden information and is rendered client-side as card backs.
 if(human_player==0 || human_player==1) {
  const auto& cards=board.hand_cards[human_player];
  out << "handcards=" << human_player;
  for(const auto code : cards) out << ',' << code;
  out << '\n';
 }
 out.close(); std::error_code ignored; fs::rename(temporary,output,ignored); if(ignored) { fs::remove(output,ignored); fs::rename(temporary,output,ignored); }
}

int main(int argc,char** argv) try {
 if(argc == 3 && std::string(argv[1]) == "card-image") {
  auto path=fs::path("external/card_images")/(std::string(argv[2])+".jpg");
  if(!fs::exists(path)) throw std::runtime_error("no local image for card " + std::string(argv[2]));
  std::cout << fs::absolute(path).string() << '\n'; return 0;
 }
 if(argc < 4 || std::string(argv[1]) != "duel") { std::cerr << "usage: goat-sim duel A.ydk B.ydk [--seed N] [--max-turns N]\n"; return 2; }
 uint64_t seed=12345; int max_turns=200; int human_player=-1; bool allow_illegal=false; std::optional<fs::path> decision_directory; for(int i=4;i<argc;i++) { if(std::string(argv[i])=="--seed" && i+1<argc) seed=std::stoull(argv[++i]); else if(std::string(argv[i])=="--max-turns" && i+1<argc) max_turns=std::stoi(argv[++i]); else if(std::string(argv[i])=="--allow-illegal-deck") allow_illegal=true; else if(std::string(argv[i])=="--quiet") g_quiet=true; else if(std::string(argv[i])=="--decision-dir" && i+1<argc) decision_directory=argv[++i]; else if(std::string(argv[i])=="--result-file" && i+1<argc) g_result_file=argv[++i]; else if(std::string(argv[i])=="--human-player" && i+1<argc) { human_player=std::stoi(argv[++i])-1; if(human_player<0 || human_player>1) throw std::runtime_error("--human-player must be 1 or 2"); } }
 Context ctx{fs::path("external/CardScripts"), "external/BabelCDB/cards.cdb", "external/BabelCDB/goat-entries.cdb"}; g_database=&ctx.database;
 const auto banlist=goat::Banlist::load("external/LFLists/GOAT.lflist.conf");
 auto a=load_deck(argv[2], ctx.database, banlist, allow_illegal), b=load_deck(argv[3], ctx.database, banlist, allow_illegal);
 // OCG_DuelNewCard just stacks cards in call order — nothing about the engine
 // shuffles a deck's *starting* order on its own (options.seed only drives
 // the engine's own RNG for later effects). Without this, every duel dealt
 // the same file-order opening hand every time (traps first, since the
 // .ydk lists them last and the deck's "top" is whatever gets registered
 // last), regardless of --seed.
 std::mt19937_64 shuffle_rng(seed);
 std::shuffle(a.main.begin(), a.main.end(), shuffle_rng);
 std::shuffle(b.main.begin(), b.main.end(), shuffle_rng);
 OCG_DuelOptions options{}; options.seed[0]=seed; options.seed[1]=seed^0x9e3779b97f4a7c15ULL; options.seed[2]=seed+1; options.seed[3]=seed+2;
 options.flags=DUEL_MODE_GOAT; options.team1={8000,5,1}; options.team2={8000,5,1}; options.cardReader=card_reader; options.scriptReader=script_reader; options.logHandler=logger; options.payload1=&ctx; options.payload2=&ctx;
 OCG_Duel duel{}; if(OCG_CreateDuel(&duel,&options)!=OCG_DUEL_CREATION_SUCCESS) throw std::runtime_error("OCG_CreateDuel failed");
 // constant.lua/utility.lua define globals (GetID, Auxiliary, etc.) that every
 // other card script assumes already exist. They must load before any card is
 // added to the duel: OCG_DuelNewCard triggers that card's script immediately
 // (to register its initial_effect), and Normal Monsters are the only type
 // that has no script and so never surfaced this — every Effect Monster,
 // Spell, and Trap was silently failing to register its effect at all.
 for(const char* script : {"constant.lua","utility.lua"}) if(!script_reader(&ctx,duel,script)) throw std::runtime_error(std::string("failed to load ")+script);
 auto add=[&](const Deck& deck,uint8_t player){
  for(uint32_t i=0;i<deck.main.size();++i) { OCG_NewCardInfo info{}; info.team=player; info.con=player; info.code=deck.main[i]; info.loc=LOCATION_DECK; info.seq=2; info.pos=POS_FACEDOWN_DEFENSE; OCG_DuelNewCard(duel,&info); }
  for(uint32_t i=0;i<deck.extra.size();++i) { OCG_NewCardInfo info{}; info.team=player; info.con=player; info.code=deck.extra[i]; info.loc=LOCATION_EXTRA; info.seq=2; info.pos=POS_FACEDOWN_DEFENSE; OCG_DuelNewCard(duel,&info); }
 }; add(a,0); add(b,1);
 std::cout << "Loaded decks: Player 1=" << OCG_DuelQueryCount(duel,0,LOCATION_DECK) << "+" << OCG_DuelQueryCount(duel,0,LOCATION_EXTRA)
           << " extra, Player 2=" << OCG_DuelQueryCount(duel,1,LOCATION_DECK) << "+" << OCG_DuelQueryCount(duel,1,LOCATION_EXTRA) << " extra\n";
 OCG_StartDuel(duel); RandomAgent agent(human_player, decision_directory); BoardState board; int turns=0;
 for(int calls=0;calls<50000;calls++) { auto status=OCG_DuelProcess(duel); uint32_t bytes=0; bool won=false; int winner=-1, reason=-1; auto* raw=static_cast<uint8_t*>(OCG_DuelGetMessage(duel,&bytes)); const uint8_t *p=raw,*end=raw+bytes; while(p<end){auto n=read<uint32_t>(p,end); if(static_cast<size_t>(end-p)<n) throw std::runtime_error("bad message frame"); track_board_message(p,n,board); log_message(p,n); if(*p==MSG_WIN) { won=true; if(n>=3) { winner=p[1]; reason=p[2]; } } if(*p==MSG_NEW_TURN && ++turns>max_turns) throw std::runtime_error("turn limit reached"); p+=n;}
  if(decision_directory) {
   board.hand[0]=OCG_DuelQueryCount(duel,0,LOCATION_HAND); board.hand[1]=OCG_DuelQueryCount(duel,1,LOCATION_HAND);
   board.deck_count[0]=OCG_DuelQueryCount(duel,0,LOCATION_DECK); board.deck_count[1]=OCG_DuelQueryCount(duel,1,LOCATION_DECK);
   board.grave_count[0]=OCG_DuelQueryCount(duel,0,LOCATION_GRAVE); board.grave_count[1]=OCG_DuelQueryCount(duel,1,LOCATION_GRAVE);
   board.extra_count[0]=OCG_DuelQueryCount(duel,0,LOCATION_EXTRA); board.extra_count[1]=OCG_DuelQueryCount(duel,1,LOCATION_EXTRA);
   board.banished_count[0]=OCG_DuelQueryCount(duel,0,LOCATION_REMOVED); board.banished_count[1]=OCG_DuelQueryCount(duel,1,LOCATION_REMOVED);
   write_board_state(*decision_directory,board,human_player);
  }
  if(won) { if(g_result_file) { std::ofstream out(*g_result_file, std::ios::trunc); out << "winner=" << winner << "\nreason=" << reason << '\n'; } OCG_DestroyDuel(duel); return 0; }
  if(status==OCG_DUEL_STATUS_END) { OCG_DestroyDuel(duel); return 0; }
  if(status==OCG_DUEL_STATUS_AWAITING) { p=raw; while(p<end){auto n=read<uint32_t>(p,end); if(*p>=MSG_SELECT_BATTLECMD && *p<=MSG_SELECT_UNSELECT_CARD) { try { const uint32_t chainContext=board.chain_stack.empty()?0u:board.chain_stack.back(); auto answer=agent.choose(p,n,chainContext); OCG_DuelSetResponse(duel,answer.data(),answer.size()); } catch(...) { std::cerr << "decision message " << int(*p) << " has " << n << " bytes\n"; throw; } break; } p+=n;} }
 }
 OCG_DestroyDuel(duel); throw std::runtime_error("processor call limit reached");
} catch(const std::exception& e) { std::cerr << "goat-sim: " << e.what() << '\n'; return 1; }
