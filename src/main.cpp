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
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "cards/CardDatabase.hpp"
#include "deck/Banlist.hpp"

#include "ai/AnnounceCardSolver.hpp"
#include "ai/DecisionRequest.hpp"
#include "ai/DecisionResponse.hpp"
#include "ai/DuelAgent.hpp"
#include "ai/DuelObservation.hpp"
#include "ai/GoatAgent.hpp"
#include "ai/ObservationBuilder.hpp"
#include "ai/RandomAgent.hpp"
#include "ai/executors/DeckArchetype.hpp"
#include "ai/executors/GenericGoatExecutors.hpp"

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

// One legal candidate for a "select N cards" prompt (MSG_SELECT_CARD,
// MSG_SELECT_TRIBUTE) — the (controller, location, sequence) identity lets
// the client correlate a candidate to the physical zone it's rendered in,
// for click-to-select instead of a text menu; see choose_multi_menu.
struct SelectionCandidate { std::string label; uint32_t code{}; uint8_t controller{}; uint8_t location{}; uint32_t sequence{}; };

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
   // field::process(Processors::SelectChain&) in playerop.cpp only accepts a
   // -1 decline when `forced` is false (`if(!forced && returns.at<int32_t>(0)
   // == -1) return TRUE;`) — when forced, -1 fails that check *and* the
   // bounds check right after it (-1 is never a valid index), so the engine
   // answers with MSG_RETRY and asks the exact same question again. This
   // used to always decline regardless of `forced`, which meant any duel
   // where the CPU (or, before human_player_ was set, either side) hit a
   // forced chain link — a mandatory trigger effect, not uncommon in this
   // card pool — got stuck submitting the same rejected response forever:
   // an invisible infinite MSG_RETRY loop (this game loop's own message log
   // only names a handful of message kinds, so this produced no visible
   // output at all) that silently burned through the entire 50000-call
   // budget before dying with "processor call limit reached" — a duel
   // ending with no warning, often mid-battle, since forced chain windows
   // are common right after an attack or during the opponent's turn.
   // Reproduced directly: stress-testing every pair of this project's own
   // shipped GOAT tournament decks (decks/starter/*.ydk) across several
   // seeds hit this exact failure on 225 of 396 matchups (57%) before this
   // fix, and a debug build that logged every message kind confirmed the
   // pending state right before each hang was always MSG_SELECT_CHAIN.
   if(forced && count > 0) return raw_response(0);
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
   const auto player = read<uint8_t>(p,end); read<uint8_t>(p,end); const auto min = read<uint32_t>(p,end); const auto max = read<uint32_t>(p,end);
   const auto choices = read<uint32_t>(p,end);
   if(min > choices) throw std::runtime_error("invalid tribute-selection prompt");
   std::vector<SelectionCandidate> candidates;
   candidates.reserve(choices);
   for(uint32_t i=0;i<choices;++i) { const auto code=read<uint32_t>(p,end); const auto controller=read<uint8_t>(p,end); const auto location=read<uint8_t>(p,end); const auto sequence=read<uint32_t>(p,end); read<uint8_t>(p,end); candidates.push_back({name(code), code, out_index(controller), location, sequence}); }
   if(decision_directory_ && player == human_player_) {
    return choose_multi_menu(min == max ? (min == 1 ? "Choose a tribute" : "Choose tributes") : "Choose tributes", candidates, min, max);
   }
   return select_first(min);
  }
  if(kind == MSG_SELECT_CARD) {
   const auto player = read<uint8_t>(p,end); read<uint8_t>(p,end); const auto min = read<uint32_t>(p,end);
   const auto max = read<uint32_t>(p,end); const auto choices = read<uint32_t>(p,end);
   if(min > choices) throw std::runtime_error("invalid card-selection prompt");
   std::vector<SelectionCandidate> candidates;
   candidates.reserve(choices);
   for(uint32_t i=0;i<choices;++i) { const auto code=read<uint32_t>(p,end); const auto controller=read<uint8_t>(p,end); const auto location=read<uint8_t>(p,end); const auto sequence=read<uint32_t>(p,end); read<uint32_t>(p,end); candidates.push_back({name(code), code, out_index(controller), location, sequence}); }
   if(decision_directory_ && player == human_player_) {
    return choose_multi_menu(min == max ? (min == 1 ? "Choose a card" : "Choose cards") : "Choose cards", candidates, min, max);
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
  if(kind == MSG_ANNOUNCE_RACE) {
   // field::process(Processors::AnnounceRace&) in playerop.cpp: player,
   // count, available (u64 bitmask of RACE_* bits). Response is a u64
   // bitmask with exactly `count` bits set, all within `available`.
   const auto player=read<uint8_t>(p,end); const auto count=read<uint8_t>(p,end); const auto available=read<uint64_t>(p,end);
   if(decision_directory_ && player == human_player_ && count == 1) {
    std::vector<std::pair<std::string,Response>> choices;
    for(int bit=0; bit<64; ++bit) { const uint64_t mask=uint64_t(1)<<bit; if(available & mask) choices.emplace_back("Declare Race "+std::to_string(mask), raw_response64(mask)); }
    if(!choices.empty()) return choose_menu("Declare a Race", choices);
   }
   // count>1 (rare) has no interactive combination menu in this pass — the
   // greedy take is always a legal declaration, just not a chosen one.
   return raw_response64(take_first_n_bits(available, count));
  }
  if(kind == MSG_ANNOUNCE_ATTRIB) {
   // field::process(Processors::AnnounceAttribute&): same shape as
   // AnnounceRace, over a u32 bitmask of ATTRIBUTE_* bits.
   const auto player=read<uint8_t>(p,end); const auto count=read<uint8_t>(p,end); const auto available=read<uint32_t>(p,end);
   if(decision_directory_ && player == human_player_ && count == 1) {
    std::vector<std::pair<std::string,Response>> choices;
    for(int bit=0; bit<32; ++bit) { const uint32_t mask=uint32_t(1)<<bit; if(available & mask) choices.emplace_back("Declare Attribute "+std::to_string(mask), raw_response(static_cast<int32_t>(mask))); }
    if(!choices.empty()) return choose_menu("Declare an Attribute", choices);
   }
   return raw_response(static_cast<int32_t>(take_first_n_bits(available, count)));
  }
  if(kind == MSG_ANNOUNCE_CARD) {
   // field::process(Processors::AnnounceCard&): player, option count (u8),
   // then that many u64 predicate opcodes a declared card's name must
   // satisfy (see src/ai/AnnounceCardSolver.cpp — a full port of this
   // engine's own is_declarable). Response is the declared card's code.
   const auto player=read<uint8_t>(p,end); const auto count=read<uint8_t>(p,end);
   std::vector<uint64_t> opcodes; opcodes.reserve(count);
   for(uint8_t i=0;i<count;++i) opcodes.push_back(read<uint64_t>(p,end));
   if(decision_directory_ && player == human_player_) {
    const auto candidates = goat::ai::find_declarable_cards(*g_database, opcodes, 8);
    std::vector<std::pair<std::string,Response>> choices;
    for(const auto code : candidates) choices.emplace_back("Declare "+name(code)+" ["+image(code)+"]", raw_response(static_cast<int32_t>(code)));
    if(!choices.empty()) return choose_menu("Declare a card name", choices);
   }
   const auto code = goat::ai::find_declarable_card(*g_database, opcodes);
   if(!code) throw std::runtime_error("no card in the pinned pool satisfies this declare-a-card-name prompt");
   return raw_response(static_cast<int32_t>(*code));
  }
  if(kind == MSG_ANNOUNCE_NUMBER) {
   // field::process(Processors::AnnounceNumber&): same shape as
   // AnnounceCard, but the response is an *index* into the offered values,
   // not the value itself.
   const auto player=read<uint8_t>(p,end); const auto count=read<uint8_t>(p,end);
   std::vector<uint64_t> options; options.reserve(count);
   for(uint8_t i=0;i<count;++i) options.push_back(read<uint64_t>(p,end));
   if(options.empty()) throw std::runtime_error("empty announce-number prompt");
   if(decision_directory_ && player == human_player_) {
    std::vector<std::pair<std::string,Response>> choices;
    for(size_t i=0;i<options.size();++i) choices.emplace_back("Declare "+std::to_string(options[i]), raw_response(static_cast<int32_t>(i)));
    return choose_menu("Declare a number", choices);
   }
   return raw_response(0);
  }
  // A nonzero default is intentionally rejected by the engine, exposing
  // unsupported protocol instead of silently cheating.
  throw std::runtime_error("unsupported decision message " + std::to_string(kind));
 }
private:
 int human_player_;
 std::optional<fs::path> decision_directory_;
 // Translates a raw OCG seat (0 or 1, whichever the engine's own wire
 // protocol reports) into client-relative "0 = you, 1 = opponent" — the
 // same translation write_board_state's own out_index applies to every
 // monster=/spell= line. Needed here too: with the per-duel coin flip
 // (see main()'s seat_of_a/seat_of_b) the human isn't always raw seat 0,
 // but every SelectionCandidate below still gets rendered against
 // client-relative board state (state.monsters[0] is always "you" there).
 // Left untranslated, a genuine opponent-side candidate on a duel where
 // the human landed on raw seat 1 reported itself as controller=0 — the
 // client's own "mine" — matching it against the human's own (often
 // differently-occupied, sometimes empty) zone instead of the actual
 // opponent zone it named, with no indication anything was wrong.
 uint8_t out_index(uint8_t raw_seat) const { return (human_player_>=0 && raw_seat==static_cast<uint8_t>(human_player_)) ? 0u : 1u; }
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
    // `fs::exists` can turn true a moment before a different process's write
    // is actually visible to a read on this platform (observed via
    // timestamped tracing: the client's write-then-rename of response.tmp
    // to response.txt had *always* written "0", yet an immediate read here
    // sometimes saw an empty file, leaving `choice` at its unread sentinel
    // value — which happened to equal choices.size() whenever there was
    // only one legal choice, so the very check meant to catch a genuinely
    // invalid index instead fired on a merely-not-yet-flushed write, ending
    // the duel with "invalid graphical action index" a few prompts in).
    // Once the deck-aware CPU AI (src/ai/GoatAgent) started actively using
    // spells/traps instead of the old passive baseline, decision prompts on
    // both seats — including this one — started arriving close enough
    // together in wall-clock time to make this race easy to hit, where it
    // had been vanishingly rare before. Guard against it by only consuming
    // the file once it has visible content, and only ever treating a
    // *genuinely* out-of-range index (not a stream-extraction failure) as
    // an invalid response.
    std::error_code size_error; const auto size = fs::file_size(response_file, size_error);
    if(!size_error && size > 0) {
     std::ifstream in(response_file); size_t choice=choices.size(); in >> choice;
     if(!in.fail()) {
      fs::remove(response_file, ignored); fs::remove(request, ignored);
      if(choice < choices.size()) return choices[choice].second;
      throw std::runtime_error("invalid graphical action index");
     }
     // Content exists but didn't parse — same still-settling-write
     // situation; leave the file alone and retry rather than discarding an
     // answer that may simply not be fully flushed yet.
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
   }
   throw std::runtime_error("player decision timed out");
  }
  for(size_t i=0;i<choices.size();++i) std::cout << "  [" << i << "] " << choices[i].first << '\n';
  std::cout << "Choose action: "; size_t choice=choices.size(); std::cin >> choice;
  if(choice >= choices.size()) throw std::runtime_error("invalid human action index");
  return choices[choice].second;
 }
 // Answers a "select between min and max cards" prompt (MSG_SELECT_CARD,
 // MSG_SELECT_TRIBUTE) by publishing every individual candidate — not the
 // combinatorial "every N-card combination as one line" menu this project
 // used to generate — so the client can render them as clickable zones the
 // player toggles, rather than a text list. request.txt gets a reserved
 // "#SELECT <min> <max>" first line (a
 // plain title never starts with '#') so the client knows to switch into
 // toggle-selection mode; response.txt is a comma-separated list of chosen
 // candidate indices instead of a single line index.
 Response choose_multi_menu(const std::string& title, const std::vector<SelectionCandidate>& candidates, uint32_t min, uint32_t max) const {
  if(decision_directory_) {
   fs::create_directories(*decision_directory_);
   const auto request = *decision_directory_ / "request.txt";
   const auto temporary = *decision_directory_ / "request.tmp";
   const auto response_file = *decision_directory_ / "response.txt";
   std::error_code ignored; fs::remove(response_file, ignored);
   {
    std::ofstream out(temporary, std::ios::trunc);
    out << "#SELECT " << min << ' ' << max << '\n' << title << '\n';
    for(size_t i=0;i<candidates.size();++i) {
     const auto& c=candidates[i];
     out << i << '|' << c.label << " [" << image(c.code) << "][" << int(c.controller) << ',' << int(c.location) << ',' << c.sequence << "]\n";
    }
   }
   fs::rename(temporary, request, ignored); if(ignored) throw std::runtime_error("cannot publish player decision request");
   for(int wait=0;wait<6000;++wait) {
    std::error_code size_error; const auto size = fs::file_size(response_file, size_error);
    if(!size_error && size > 0) {
     std::ifstream in(response_file); std::string line; std::getline(in, line);
     if(!line.empty()) {
      std::vector<uint32_t> indices; bool valid=true; size_t cursor=0;
      while(cursor <= line.size()) {
       const auto comma=line.find(',', cursor);
       const auto token=line.substr(cursor, comma==std::string::npos ? std::string::npos : comma-cursor);
       if(token.empty()) { valid=false; break; }
       try { indices.push_back(static_cast<uint32_t>(std::stoul(token))); } catch(...) { valid=false; break; }
       if(comma==std::string::npos) break;
       cursor=comma+1;
      }
      std::sort(indices.begin(), indices.end());
      const bool unique = std::adjacent_find(indices.begin(), indices.end()) == indices.end();
      const bool inRange = std::all_of(indices.begin(), indices.end(), [&](uint32_t i){ return i < candidates.size(); });
      if(valid && unique && inRange && indices.size()>=min && indices.size()<=max) {
       fs::remove(response_file, ignored); fs::remove(request, ignored);
       return select_indices(indices);
      }
      // A malformed/out-of-window response is treated the same as an
      // incomplete write below — never consumed, always retried — rather
      // than failing the duel over what's usually just a UI-side bug that's
      // safer to give another chance than to crash over.
     }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
   }
   throw std::runtime_error("player decision timed out");
  }
  std::vector<uint32_t> indices; for(uint32_t i=0;i<min && i<candidates.size();++i) indices.push_back(i);
  return select_indices(indices);
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
 static Response raw_response64(uint64_t r){ Response out{}; std::memcpy(out.data(),&r,8); return out; }
 static uint64_t take_first_n_bits(uint64_t available, uint8_t count){ uint64_t taken=0; for(int bit=0; bit<64 && count>0; ++bit) { const uint64_t mask=uint64_t(1)<<bit; if(available & mask) { taken|=mask; --count; } } return taken; }
 static Response place_response(uint8_t player,uint8_t location,uint8_t sequence){ Response out{}; out[0]=player; out[1]=location; out[2]=sequence; return out; }
 static Response select_indices(const std::vector<uint32_t>& indices){ Response out{}; const auto count=static_cast<uint32_t>(indices.size()); std::memcpy(out.data()+4,&count,4); for(uint32_t i=0;i<count && 8+i*4+4<=out.size();++i) std::memcpy(out.data()+8+i*4,&indices[i],4); return out; }
 static Response select_first(uint32_t count){ std::vector<uint32_t> indices; for(uint32_t i=0;i<count;++i) indices.push_back(i); return select_indices(indices); }
 static Response select_counter_response(const std::vector<uint16_t>& amounts){ Response out{}; for(size_t i=0;i<amounts.size() && i*2+2<=out.size();++i) std::memcpy(out.data()+i*2,&amounts[i],2); return out; }
 static Response select_unselect_response(int32_t action,int32_t index){ Response out{}; std::memcpy(out.data(),&action,4); std::memcpy(out.data()+4,&index,4); return out; }
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
 else if(kind==MSG_WIN) {
  auto pl=read<uint8_t>(p,end); auto reason=read<uint8_t>(p,end);
  // pl==PLAYER_NONE (2) is a genuine draw (e.g. both players deck out on the
  // same draw, or simultaneous LP-to-zero neither side is immune to) — not a
  // third player. "Player 3 wins" would otherwise print here verbatim, and
  // run_automatic_duel (src/client_rl/main.cpp) surfaces this line as-is.
  if(pl==PLAYER_NONE) std::cout << "The duel is a draw (reason " << int(reason) << ").\n";
  else std::cout << "Player " << int(pl+1) << " wins (reason " << int(reason) << ").\n";
 }
}

// `attack`/`defense` are the monster's *current* (possibly effect-modified)
// stats, sentinel -1 when not applicable (spells/traps, empty slots) or not
// yet queried — see track_monster_stats. Always the true value here, same
// as `code`; visibility masking for face-down/opponent cards happens only
// at the write_board_state output boundary, not in this internal tracking.
struct BoardCard { uint32_t code{}; uint8_t position{}; int32_t attack{-1}; int32_t defense{-1}; };
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
 // Graveyard is always public information in real Yu-Gi-Oh (any card that
 // lands there, however it got there, is visible to both players), so this
 // is a plain code list — no position/masking needed, unlike hand_cards.
 std::array<std::vector<uint32_t>,2> grave_cards{};
 // Removed-from-play *can* be face-down for a handful of effects, so this
 // keeps position alongside the code (mirrors monsters/spells' BoardCard)
 // — write_board_state masks a face-down one the same way it already masks
 // a face-down monster/spell.
 // Extra deck is private information — visible only to the player who built
 // it, same as hand_cards (write_board_state writes only human_player's own
 // line, never the opponent's). Unlike hand/grave, its *starting* contents
 // never arrive via a tracked MSG_MOVE/MSG_DRAW at all (OCG_DuelNewCard just
 // silently stacks the deck at setup, before OCG_StartDuel even runs) — it's
 // seeded directly from the loaded Deck::extra list right after `board` is
 // constructed in main(), then MSG_MOVE below maintains it exactly like
 // grave_cards as cards leave for the field (Fusion/Ritual Summon) or, rarely,
 // return.
 std::array<std::vector<uint32_t>,2> extra_cards{};
 std::array<std::vector<BoardCard>,2> banished_cards{};
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
 // Which player controls the card on top of the chain right now (cleared once
 // the whole chain resolves), and which player most recently Normal Summoned
 // — the two pieces of context the AI's reactive-trap timing gate needs
 // (should_fire_reactive_trap in src/ai/Heuristics.cpp) to tell "the opponent
 // just did something" apart from "it's still our own uncontested turn".
 // Scoped to Normal Summons only (MSG_SUMMONING): MSG_SPSUMMONED carries no
 // payload at all, so Special Summons aren't attributed here in this pass.
 std::optional<uint8_t> last_chain_player{};
 std::optional<uint8_t> last_summon_player{};
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
 if(kind == MSG_SHUFFLE_HAND) {
  // Effects like Graceful Charity (draw 3, then Duel.ShuffleHand before the
  // discard prompt) reorder the engine's hand in place so the discard can't
  // be inferred from draw order. The engine reports the resulting order in
  // full here; without applying it, this mirror keeps the pre-shuffle slot
  // order while MSG_SELECT_CARD's candidate sequences are already the new
  // (post-shuffle) ones, so a click on the hand grid resolves to whatever
  // card the engine now has at that sequence instead of the one on screen —
  // the wrong card gets discarded, and the *actual* discarded code (correct)
  // then shows up in the graveyard looking like a mismatch.
  const auto player=read<uint8_t>(p,end); const auto count=read<uint32_t>(p,end);
  std::vector<uint32_t> shuffled; shuffled.reserve(count);
  for(uint32_t i=0;i<count;i++) shuffled.push_back(read<uint32_t>(p,end));
  if(player<2) board.hand_cards[player]=std::move(shuffled);
  return;
 }
 if(kind == MSG_MOVE) {
  const auto code=read<uint32_t>(p,end); const auto previous=read_location(p,end); const auto current=read_location(p,end);
  if(previous.player<2 && previous.location==LOCATION_MZONE && previous.sequence<5) board.monsters[previous.player][previous.sequence]={};
  if(previous.player<2 && previous.location==LOCATION_SZONE && previous.sequence<6) board.spells[previous.player][previous.sequence]={};
  if(previous.player<2 && previous.location==LOCATION_HAND) {
   // Erase by previous.sequence, not by first-matching code: the engine
   // itself removes the exact physical card at that hand slot (see
   // ygopro-core's field.cpp LOCATION_HAND case), so with 2+ copies of the
   // same code in hand, an erase-by-code here could delete a different copy
   // than the one that actually left — silently desyncing this mirror's
   // slot order from the engine's, which then made multi-select prompts
   // (e.g. Graceful Charity's discard) resolve the wrong physical card.
   auto& cards=board.hand_cards[previous.player];
   if(previous.sequence<cards.size()) cards.erase(cards.begin()+previous.sequence);
   else if(const auto it=std::find(cards.begin(),cards.end(),code); it!=cards.end()) cards.erase(it);
   if(board.hand[previous.player]) --board.hand[previous.player];
  }
  // Grave/banished-pile card lists (see BoardState's own field comments) —
  // erase-on-leave, push-on-arrive, exactly the hand_cards pattern above.
  // Fires regardless of where a card is arriving from (deck, field, the
  // other pile, ...), same as every case here — only `current`/`previous`'s
  // own location matters, not how the card got there.
  if(previous.player<2 && previous.location==LOCATION_GRAVE) {
   auto& cards=board.grave_cards[previous.player]; if(const auto it=std::find(cards.begin(),cards.end(),code); it!=cards.end()) cards.erase(it);
  }
  if(previous.player<2 && previous.location==LOCATION_REMOVED) {
   auto& cards=board.banished_cards[previous.player];
   if(const auto it=std::find_if(cards.begin(),cards.end(),[code](const BoardCard& c){ return c.code==code; }); it!=cards.end()) cards.erase(it);
  }
  // Extra deck's *starting* contents are seeded directly in main() (see the
  // field comment on BoardState::extra_cards) — this only maintains it
  // afterward, same erase-on-leave/push-on-arrive shape as grave, for the
  // rare case a card actually moves out of (Fusion/Ritual Summon) or back
  // into the extra deck mid-duel.
  if(previous.player<2 && previous.location==LOCATION_EXTRA) {
   auto& cards=board.extra_cards[previous.player]; if(const auto it=std::find(cards.begin(),cards.end(),code); it!=cards.end()) cards.erase(it);
  }
  if(current.player<2 && current.location==LOCATION_MZONE && current.sequence<5) board.monsters[current.player][current.sequence]={code,static_cast<uint8_t>(current.position)};
  if(current.player<2 && current.location==LOCATION_SZONE && current.sequence<6) board.spells[current.player][current.sequence]={code,static_cast<uint8_t>(current.position)};
  if(current.player<2 && current.location==LOCATION_HAND) { board.hand_cards[current.player].push_back(code); ++board.hand[current.player]; }
  if(current.player<2 && current.location==LOCATION_GRAVE) board.grave_cards[current.player].push_back(code);
  if(current.player<2 && current.location==LOCATION_REMOVED) board.banished_cards[current.player].push_back({code,static_cast<uint8_t>(current.position)});
  if(current.player<2 && current.location==LOCATION_EXTRA) board.extra_cards[current.player].push_back(code);
  return;
 }
 if(kind == MSG_POS_CHANGE) {
  read<uint32_t>(p,end); const auto player=read<uint8_t>(p,end); const auto location=read<uint8_t>(p,end); const auto sequence=read<uint8_t>(p,end); read<uint8_t>(p,end); const auto position=read<uint8_t>(p,end);
  if(player<2 && location==LOCATION_MZONE && sequence<5 && board.monsters[player][sequence].code) board.monsters[player][sequence].position=position;
  if(player<2 && location==LOCATION_SZONE && sequence<6 && board.spells[player][sequence].code) board.spells[player][sequence].position=position;
  return;
 }
 if(kind == MSG_FLIPSUMMONING) {
  // A facedown Defense Position monster manually turned face-up on your own
  // turn (a Flip Summon) is a position-only change on a card that's already
  // on the field, same as MSG_POS_CHANGE above — but field::process(...) in
  // operations.cpp (the FlipSummon processor) reports it via this entirely
  // different message instead of MSG_POS_CHANGE, since it's driven by
  // is_can_be_flip_summoned rather than the manual attack<->defense toggle.
  // Without a case here, board.monsters[][] kept the card's stale facedown
  // position forever (nothing else in this switch ever touches it), so
  // write_board_state's own facedown-hides-the-code check kept treating an
  // opponent's already-revealed Flip Summoned monster as still hidden.
  const auto code=read<uint32_t>(p,end); const auto loc=read_location(p,end);
  if(loc.player<2 && loc.location==LOCATION_MZONE && loc.sequence<5 && board.monsters[loc.player][loc.sequence].code==code)
   board.monsters[loc.player][loc.sequence].position=static_cast<uint8_t>(loc.position);
  return;
 }
 if(kind == MSG_CHAINING) {
  // field::process(...) case 2 in processor.cpp: code, loc_info, triggering
  // controler/location/sequence, description, chain size — see the write
  // order at processor.cpp's MSG_CHAINING block. loc_info's own `controler`
  // field is the controller of the card now on top of the chain, which is
  // exactly what the AI's reactive-trap timing gate needs.
  const auto code=read<uint32_t>(p,end); const auto loc=read_location(p,end);
  read<uint8_t>(p,end); read<uint8_t>(p,end); read<uint32_t>(p,end); read<uint64_t>(p,end); read<uint32_t>(p,end);
  board.chain_stack.push_back(code);
  if(loc.player<2) board.last_chain_player=loc.player;
  return;
 }
 if(kind == MSG_CHAIN_SOLVED) { read<uint8_t>(p,end); if(!board.chain_stack.empty()) board.chain_stack.pop_back(); return; }
 if(kind == MSG_CHAIN_END) { board.chain_stack.clear(); board.last_chain_player.reset(); return; }
 if(kind == MSG_SUMMONING) {
  // operations.cpp case 12: code, loc_info — loc_info's `controler` is the
  // summoning player. Special Summons (MSG_SPSUMMONED) carry no payload at
  // all, so only Normal Summons are attributed here; see the BoardState
  // field comment for why that's an acceptable scope for this pass.
  read<uint32_t>(p,end); const auto loc=read_location(p,end);
  if(loc.player<2) board.last_summon_player=loc.player;
  return;
 }
}
// Converts this file's own BoardState into goat::ai::ObservationInputs — the
// plain-data shape goat::ai::build_observation (src/ai/ObservationBuilder.cpp)
// actually performs the no-cheating masking on. Kept as a trivial field-by-
// field copy so the masking logic itself lives in, and is unit-tested from,
// the AI module rather than here.
static goat::ai::ObservationInputs to_observation_inputs(const BoardState& board) {
 goat::ai::ObservationInputs inputs;
 inputs.life=board.life; inputs.hand_count=board.hand; inputs.hand_cards=board.hand_cards;
 for(uint8_t player=0;player<2;++player) {
  for(uint8_t i=0;i<5;++i) inputs.monsters[player][i]={board.monsters[player][i].code, board.monsters[player][i].position};
  for(uint8_t i=0;i<6;++i) inputs.spells[player][i]={board.spells[player][i].code, board.spells[player][i].position};
 }
 inputs.deck_count=board.deck_count; inputs.grave_count=board.grave_count;
 inputs.extra_count=board.extra_count; inputs.banished_count=board.banished_count;
 inputs.turn_player=board.turn_player; inputs.turn_number=board.turn_number; inputs.phase=board.phase;
 inputs.chain_stack=board.chain_stack; inputs.last_chain_player=board.last_chain_player; inputs.last_summon_player=board.last_summon_player;
 return inputs;
}
// Queries each player's current (possibly effect-modified) monster ATK/DEF
// via OCG_DuelQueryLocation, so the UI can show a boosted/reduced stat in a
// different color from the printed baseline. Purely a human-UI concern —
// AI decisions already reason about effective power via
// src/ai/CardEvaluator.cpp using CardDatabase's printed stats — so this is
// only ever called when a decision-dir (human session) is active.
static void track_monster_stats(OCG_Duel duel, BoardState& board) {
 for(uint8_t player=0;player<2;++player) {
  OCG_QueryInfo info{}; info.flags=QUERY_ATTACK|QUERY_DEFENSE; info.con=player; info.loc=LOCATION_MZONE;
  uint32_t length=0; auto* raw=static_cast<uint8_t*>(OCG_DuelQueryLocation(duel,&length,&info));
  if(!raw || length<4) continue;
  // OCG_DuelQueryLocation prepends a uint32_t total-size header before the
  // per-slot data (see external/ygopro-core/ocgapi.cpp's OCG_DuelQueryLocation);
  // list_mzone is always allocated 7 wide (5 main zones + 2 Extra Monster
  // Zones, per external/ygopro-core/field.h) regardless of GOAT/modern
  // format, so exactly 7 slot-entries always need parsing even though GOAT
  // only ever uses the first 5.
  const uint8_t *p=raw+4, *end=raw+length;
  for(uint8_t seq=0;seq<7 && p<end;++seq) {
   const auto first_size=read<uint16_t>(p,end);
   if(first_size==0) { if(seq<5) { board.monsters[player][seq].attack=-1; board.monsters[player][seq].defense=-1; } continue; }
   // Two fixed TLV entries follow, in card::get_infos's declaration order:
   // [u32 QUERY_ATTACK][u32 value] then [u16 size][u32 QUERY_DEFENSE][u32 value].
   read<uint32_t>(p,end); const auto attack=read<uint32_t>(p,end);
   read<uint16_t>(p,end); read<uint32_t>(p,end); const auto defense=read<uint32_t>(p,end);
   // card::get_infos (external/ygopro-core/card.cpp) unconditionally appends
   // two more entries after whatever fields were actually requested — a
   // "HACK: to remove once the servers are updated to send this flag"
   // QUERY_IS_PUBLIC (u16 size=5, u32 id, u8 value) and a trailing
   // QUERY_END marker (u16 size=4, u32 id, no value) — both always present
   // regardless of the flags passed in, and must be consumed here to stay
   // aligned with the next slot.
   read<uint16_t>(p,end); read<uint32_t>(p,end); read<uint8_t>(p,end); // QUERY_IS_PUBLIC
   read<uint16_t>(p,end); read<uint32_t>(p,end); // QUERY_END
   if(seq<5) { board.monsters[player][seq].attack=static_cast<int32_t>(attack); board.monsters[player][seq].defense=static_cast<int32_t>(defense); }
  }
 }
}
// `human_player` here is the RAW OCG seat (0 or 1) the human actually
// occupies this duel — not necessarily 0, now that main() coin-flips which
// loaded deck lands on which seat (see the flip's own comment there). Every
// two-wide field below is written through `out_index`, so the *file* always
// presents the human's own side at index 0 and the opponent's at index 1
// regardless of which raw seat either of them actually landed on — the
// client's whole rendering model assumes "index 0 is always me," and this
// is the one place that assumption gets made true again after the flip;
// nothing else (BoardState, track_board_message, the AI's own observation
// pipeline) needs to know the flip happened at all.
static void write_board_state(const fs::path& directory, const BoardState& board, int human_player) {
 const auto temporary=directory/"state.tmp", output=directory/"state.txt"; std::ofstream out(temporary,std::ios::trunc);
 // A duel with no human seat at all (CPU vs CPU) has nothing to reorient
 // around — write raw seat order, same as always.
 const auto out_index=[&](uint8_t raw_seat) -> uint8_t { return (human_player>=0 && raw_seat==static_cast<uint8_t>(human_player)) ? 0u : 1u; };
 const uint8_t me = human_player>=0 ? static_cast<uint8_t>(human_player) : 0u, opp = 1u-me;
 out << "lp=" << std::max(0, board.life[me]) << ',' << std::max(0, board.life[opp]) << "\nhand=" << board.hand[me] << ',' << board.hand[opp] << '\n';
 out << "deck=" << board.deck_count[me] << ',' << board.deck_count[opp] << '\n';
 out << "grave=" << board.grave_count[me] << ',' << board.grave_count[opp] << '\n';
 out << "extra=" << board.extra_count[me] << ',' << board.extra_count[opp] << '\n';
 out << "banished=" << board.banished_count[me] << ',' << board.banished_count[opp] << '\n';
 out << "turn=" << int(out_index(board.turn_player)) << ',' << board.turn_number << ',' << board.phase << '\n';
 for(uint8_t player=0;player<2;++player) for(uint8_t sequence=0;sequence<5;++sequence) if(const auto& card=board.monsters[player][sequence]; card.code) {
  const bool hidden=(player!=me && (card.position & POS_FACEDOWN));
  const auto visible_code=hidden ? 0u : card.code;
  // Current ATK/DEF (possibly boosted/reduced by an equip/continuous
  // effect) — masked the same as the code itself for a hidden card, so a
  // face-down monster's stats are never leaked through this either.
  const auto visible_attack=hidden ? -1 : card.attack;
  const auto visible_defense=hidden ? -1 : card.defense;
  out << "monster=" << int(out_index(player)) << ',' << int(sequence) << ',' << visible_code << ',' << int(card.position) << ',' << visible_attack << ',' << visible_defense << '\n';
 }
 for(uint8_t player=0;player<2;++player) for(uint8_t sequence=0;sequence<6;++sequence) if(const auto& card=board.spells[player][sequence]; card.code) {
  const auto visible_code=(player!=me && (card.position & POS_FACEDOWN)) ? 0u : card.code;
  out << "spell=" << int(out_index(player)) << ',' << int(sequence) << ',' << visible_code << ',' << int(card.position) << '\n';
 }
 // Only the human player's own hand identities are ever written: the opponent's
 // hand must stay hidden information and is rendered client-side as card backs.
 if(human_player==0 || human_player==1) {
  const auto& cards=board.hand_cards[me];
  out << "handcards=0";
  for(const auto code : cards) out << ',' << code;
  out << '\n';
 }
 // Same visibility rule as hand: only the human player's own extra deck is
 // ever written — the opponent's stays unlisted (their own extra deck count
 // is still visible via extra= above, same as it always was).
 if(human_player==0 || human_player==1) {
  const auto& cards=board.extra_cards[me];
  out << "extracards=0";
  for(const auto code : cards) out << ',' << code;
  out << '\n';
 }
 // Graveyard is always public — both players' full code lists are written
 // unmasked, unlike hand. One line per player (player index first field),
 // same shape as handcards= above.
 for(uint8_t player=0;player<2;++player) {
  out << "gravecards=" << int(out_index(player));
  for(const auto code : board.grave_cards[player]) out << ',' << code;
  out << '\n';
 }
 // Banished can be face-down for a handful of effects — masked the same way
 // (and for the same reason) as a face-down monster/spell above: hidden
 // when it's the opponent's and face-down, visible otherwise.
 for(uint8_t player=0;player<2;++player) {
  out << "banishedcards=" << int(out_index(player));
  for(const auto& card : board.banished_cards[player]) {
   const bool hidden=(player!=me && (card.position & POS_FACEDOWN));
   out << ',' << (hidden ? 0u : card.code);
  }
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
 if(argc < 4 || std::string(argv[1]) != "duel") { std::cerr << "usage: goat-sim duel A.ydk B.ydk [--seed N] [--max-turns N] [--agent1 random|goat] [--agent2 random|goat] [--difficulty easy|normal|hard] [--ai-seed N]\n"; return 2; }
 uint64_t seed=12345; int max_turns=200; int human_player=-1; bool allow_illegal=false; std::optional<fs::path> decision_directory;
 std::string agent_name[2] = {"goat","goat"}; std::string difficulty_name = "normal"; std::optional<uint64_t> ai_seed;
 for(int i=4;i<argc;i++) {
  if(std::string(argv[i])=="--seed" && i+1<argc) seed=std::stoull(argv[++i]);
  else if(std::string(argv[i])=="--max-turns" && i+1<argc) max_turns=std::stoi(argv[++i]);
  else if(std::string(argv[i])=="--allow-illegal-deck") allow_illegal=true;
  else if(std::string(argv[i])=="--quiet") g_quiet=true;
  else if(std::string(argv[i])=="--decision-dir" && i+1<argc) decision_directory=argv[++i];
  else if(std::string(argv[i])=="--result-file" && i+1<argc) g_result_file=argv[++i];
  else if(std::string(argv[i])=="--human-player" && i+1<argc) { human_player=std::stoi(argv[++i])-1; if(human_player<0 || human_player>1) throw std::runtime_error("--human-player must be 1 or 2"); }
  else if(std::string(argv[i])=="--agent1" && i+1<argc) agent_name[0]=argv[++i];
  else if(std::string(argv[i])=="--agent2" && i+1<argc) agent_name[1]=argv[++i];
  else if(std::string(argv[i])=="--difficulty" && i+1<argc) difficulty_name=argv[++i];
  else if(std::string(argv[i])=="--ai-seed" && i+1<argc) ai_seed=std::stoull(argv[++i]);
 }
 // "heuristic-goat" is the exact string data/npcs.json's "agent" field has
 // used since before this AI module existed — accepted as a synonym for
 // "goat" so that already-authored NPC data starts mattering without
 // needing to rewrite npcs.json.
 for(auto& name : agent_name) { if(name=="heuristic-goat") name="goat"; if(name!="random" && name!="goat") throw std::runtime_error("--agent1/--agent2 must be 'random', 'goat', or 'heuristic-goat'"); }
 goat::ai::Difficulty difficulty = goat::ai::Difficulty::Normal;
 if(difficulty_name=="easy") difficulty=goat::ai::Difficulty::Easy; else if(difficulty_name=="hard") difficulty=goat::ai::Difficulty::Hard; else if(difficulty_name!="normal") throw std::runtime_error("--difficulty must be 'easy', 'normal' or 'hard'");
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
 // A real 50/50 coin flip for who goes first. ygopro-core itself has no
 // such concept — field::process(Processors::Startup&) (processor.cpp)
 // unconditionally hardcodes raw seat 0 as whoever takes the very first
 // turn, full stop. The only lever available is which loaded deck actually
 // gets put on that seat (see seat_of_a/seat_of_b below), so "who goes
 // first" is decided right here, before either deck is even loaded into
 // the duel. Drawn from the same seeded RNG as the deck shuffles below (and
 // before them) so the whole duel setup — shuffle included — stays fully
 // reproducible for a given --seed.
 const bool swap_seats = std::uniform_int_distribution<int>(0, 1)(shuffle_rng) != 0;
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
 // Which raw OCG seat each loaded deck actually lands on — normally a=seat 0,
 // b=seat 1 (matching argv[2]/argv[3] order), swapped when the coin flip
 // above came up tails. Every raw-seat comparison from here on (routing a
 // decision to the human file-IPC path vs a CPU agent, which deck an agent's
 // executor table is built from, RandomAgent's own player==human_player_
 // checks) needs to go through this, not argument order, since that's the
 // only thing the flip actually changes.
 const uint8_t seat_of_a = swap_seats ? 1 : 0, seat_of_b = 1-seat_of_a;
 auto add=[&](const Deck& deck,uint8_t player){
  for(uint32_t i=0;i<deck.main.size();++i) { OCG_NewCardInfo info{}; info.team=player; info.con=player; info.code=deck.main[i]; info.loc=LOCATION_DECK; info.seq=2; info.pos=POS_FACEDOWN_DEFENSE; OCG_DuelNewCard(duel,&info); }
  for(uint32_t i=0;i<deck.extra.size();++i) { OCG_NewCardInfo info{}; info.team=player; info.con=player; info.code=deck.extra[i]; info.loc=LOCATION_EXTRA; info.seq=2; info.pos=POS_FACEDOWN_DEFENSE; OCG_DuelNewCard(duel,&info); }
 }; add(a,seat_of_a); add(b,seat_of_b);
 std::cout << "Loaded decks: Player 1=" << OCG_DuelQueryCount(duel,0,LOCATION_DECK) << "+" << OCG_DuelQueryCount(duel,0,LOCATION_EXTRA)
           << " extra, Player 2=" << OCG_DuelQueryCount(duel,1,LOCATION_DECK) << "+" << OCG_DuelQueryCount(duel,1,LOCATION_EXTRA) << " extra\n";
 OCG_StartDuel(duel);
 // human_player (from --human-player) is expressed in "which .ydk argument"
 // terms (0 = argv[2]/deck a, 1 = argv[3]/deck b); human_seat is the actual
 // raw OCG seat that deck ended up on after the flip — everything past this
 // point that needs to know "is this raw seat the human's" (RandomAgent,
 // the cpu_agents loop below, the decision-routing check in the main loop,
 // write_board_state) uses this, not human_player directly.
 const int human_seat = human_player<0 ? human_player : static_cast<int>(human_player==0 ? seat_of_a : seat_of_b);
 RandomAgent agent(human_seat, decision_directory); BoardState board; int turns=0;
 // Extra deck's starting contents never arrive via a message MSG_MOVE-style
 // tracking would see (OCG_DuelNewCard above just silently stacks it before
 // OCG_StartDuel even runs) — seeded directly from the same `a`/`b` Deck
 // structs used to build the duel itself. See BoardState::extra_cards.
 board.extra_cards[seat_of_a]=a.extra; board.extra_cards[seat_of_b]=b.extra;
 // One goat::ai::DuelAgent per non-human seat — the human seat (if any)
 // keeps using the legacy `agent` menu/file-IPC path above untouched. Both
 // seats get their own agent for CPU vs CPU; --ai-seed (default: --seed)
 // plus a per-seat offset keeps the two seats' RNG streams independent
 // while staying fully deterministic for a given --seed/--ai-seed pair.
 std::array<std::unique_ptr<goat::ai::DuelAgent>,2> cpu_agents;
 for(uint8_t seat=0; seat<2; ++seat) {
  if(static_cast<int>(seat)==human_seat) continue;
  // agent_name/executor selection are keyed by argument order (--agent1/
  // --agent2, deck a/b), same as human_player above — translate this raw
  // seat back to that before looking either up.
  const uint8_t arg_index = (seat==seat_of_a) ? 0 : 1;
  if(agent_name[arg_index]=="random") cpu_agents[seat]=std::make_unique<goat::ai::RandomAgent>(ctx.database);
  else {
   // Picks a deck-specific executor table (Chaos Control, Gearfried, Burn)
   // when this seat's own deck matches one of those archetypes' signature
   // cards, falling back to the generic table otherwise — see
   // src/ai/executors/DeckArchetype.cpp.
   auto executors = goat::ai::build_executors_for_deck((seat==seat_of_a ? a : b).main);
   cpu_agents[seat]=std::make_unique<goat::ai::GoatAgent>(ctx.database, std::move(executors), difficulty, ai_seed.value_or(seed)+seat);
  }
 }
 for(int calls=0;calls<50000;calls++) { auto status=OCG_DuelProcess(duel); uint32_t bytes=0; bool won=false; int winner=-1, reason=-1; auto* raw=static_cast<uint8_t*>(OCG_DuelGetMessage(duel,&bytes)); const uint8_t *p=raw,*end=raw+bytes; while(p<end){auto n=read<uint32_t>(p,end); if(static_cast<size_t>(end-p)<n) throw std::runtime_error("bad message frame"); track_board_message(p,n,board); log_message(p,n); if(*p==MSG_WIN) { won=true; if(n>=3) { winner=p[1]; reason=p[2]; } } if(*p==MSG_NEW_TURN && ++turns>max_turns) throw std::runtime_error("turn limit reached"); p+=n;}
  // Queried unconditionally (not just when a decision-dir is set): a CPU
  // seat's goat::ai::DuelObservation needs these regardless of whether the
  // human file-IPC UI is active.
  board.hand[0]=OCG_DuelQueryCount(duel,0,LOCATION_HAND); board.hand[1]=OCG_DuelQueryCount(duel,1,LOCATION_HAND);
  board.deck_count[0]=OCG_DuelQueryCount(duel,0,LOCATION_DECK); board.deck_count[1]=OCG_DuelQueryCount(duel,1,LOCATION_DECK);
  board.grave_count[0]=OCG_DuelQueryCount(duel,0,LOCATION_GRAVE); board.grave_count[1]=OCG_DuelQueryCount(duel,1,LOCATION_GRAVE);
  board.extra_count[0]=OCG_DuelQueryCount(duel,0,LOCATION_EXTRA); board.extra_count[1]=OCG_DuelQueryCount(duel,1,LOCATION_EXTRA);
  board.banished_count[0]=OCG_DuelQueryCount(duel,0,LOCATION_REMOVED); board.banished_count[1]=OCG_DuelQueryCount(duel,1,LOCATION_REMOVED);
  if(decision_directory) { track_monster_stats(duel,board); write_board_state(*decision_directory,board,human_seat); }
  if(won) { if(g_result_file) { std::ofstream out(*g_result_file, std::ios::trunc); out << "winner=" << winner << "\nreason=" << reason << '\n'; } OCG_DestroyDuel(duel); return 0; }
  if(status==OCG_DUEL_STATUS_END) { OCG_DestroyDuel(duel); return 0; }
  if(status==OCG_DUEL_STATUS_AWAITING) { p=raw; while(p<end){auto n=read<uint32_t>(p,end); if((*p>=MSG_SELECT_BATTLECMD && *p<=MSG_SELECT_UNSELECT_CARD) || (*p>=MSG_ANNOUNCE_RACE && *p<=MSG_ANNOUNCE_NUMBER)) {
    try {
     const auto kind=*p;
     // MSG_SELECT_COUNTER carries no player byte at all and — in the
     // pre-existing behavior this is extracted from — is never routed
     // through a human/CPU distinction, it always auto-resolves the same
     // way regardless of whose turn it is. Route it through the legacy
     // agent unconditionally (its counter handling ignores human_player_
     // entirely) rather than through the per-seat cpu_agents array, which
     // is deliberately left unpopulated for the human's own seat.
     const bool is_counter = kind==MSG_SELECT_COUNTER;
     const uint8_t decision_player = is_counter ? 0 : (n>1 ? p[1] : 0);
     const bool is_human_decision = is_counter || (human_seat>=0 && decision_player==static_cast<uint8_t>(human_seat));
     if(is_human_decision) {
      const uint32_t chainContext=board.chain_stack.empty()?0u:board.chain_stack.back();
      auto answer=agent.choose(p,n,chainContext); OCG_DuelSetResponse(duel,answer.data(),answer.size());
     } else {
      auto& cpu_agent = cpu_agents[decision_player<2?decision_player:0];
      if(!cpu_agent) throw std::runtime_error("no CPU agent configured for this seat");
      auto request = goat::ai::parse_decision_request(kind, p+1, p+n);
      auto inputs = to_observation_inputs(board); inputs.self_player = decision_player<2?decision_player:0;
      auto observation = goat::ai::build_observation(inputs);
      auto answer = cpu_agent->choose(observation, request);
      OCG_DuelSetResponse(duel, answer.data(), answer.size());
     }
    } catch(...) { std::cerr << "decision message " << int(*p) << " has " << n << " bytes\n"; throw; }
    break;
   } p+=n;} }
 }
 OCG_DestroyDuel(duel); throw std::runtime_error("processor call limit reached");
} catch(const std::exception& e) { std::cerr << "goat-sim: " << e.what() << '\n'; return 1; }
