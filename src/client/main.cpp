#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Layout.hpp"
#include "cards/CardDatabase.hpp"
#include "game/Progression.hpp"
#include "game/Catalog.hpp"
#include "game/DeckBuilder.hpp"

namespace fs = std::filesystem;
using namespace goat::ui;

namespace {

// Local copies of the ygopro-core bit layout this client needs to interpret
// (position/type/attribute/race/phase). Kept local rather than including the
// engine's C headers so the GUI stays decoupled from ocgapi's extern "C" block.
namespace cardbits {
constexpr uint32_t TYPE_MONSTER = 0x1, TYPE_SPELL = 0x2, TYPE_TRAP = 0x4, TYPE_NORMAL = 0x10, TYPE_EFFECT = 0x20,
                    TYPE_FUSION = 0x40, TYPE_RITUAL = 0x80, TYPE_SPIRIT = 0x200, TYPE_UNION = 0x400, TYPE_GEMINI = 0x800,
                    TYPE_TUNER = 0x1000, TYPE_QUICKPLAY = 0x10000, TYPE_CONTINUOUS = 0x20000, TYPE_EQUIP = 0x40000,
                    TYPE_FIELD = 0x80000, TYPE_COUNTER = 0x100000, TYPE_FLIP = 0x200000, TYPE_TOON = 0x400000;
constexpr uint32_t ATTRIBUTE_EARTH = 0x01, ATTRIBUTE_WATER = 0x02, ATTRIBUTE_FIRE = 0x04, ATTRIBUTE_WIND = 0x08,
                    ATTRIBUTE_LIGHT = 0x10, ATTRIBUTE_DARK = 0x20, ATTRIBUTE_DIVINE = 0x40;
constexpr uint64_t RACE_WARRIOR = 0x1, RACE_SPELLCASTER = 0x2, RACE_FAIRY = 0x4, RACE_FIEND = 0x8, RACE_ZOMBIE = 0x10,
                    RACE_MACHINE = 0x20, RACE_AQUA = 0x40, RACE_PYRO = 0x80, RACE_ROCK = 0x100, RACE_WINGEDBEAST = 0x200,
                    RACE_PLANT = 0x400, RACE_INSECT = 0x800, RACE_THUNDER = 0x1000, RACE_DRAGON = 0x2000, RACE_BEAST = 0x4000,
                    RACE_BEASTWARRIOR = 0x8000, RACE_DINOSAUR = 0x10000, RACE_FISH = 0x20000, RACE_SEASERPENT = 0x40000,
                    RACE_REPTILE = 0x80000, RACE_PSYCHIC = 0x100000;
constexpr uint16_t POS_FACEUP_ATTACK = 0x1, POS_FACEDOWN_ATTACK = 0x2, POS_FACEUP_DEFENSE = 0x4, POS_FACEDOWN_DEFENSE = 0x8;
constexpr uint16_t POS_FACEDOWN = POS_FACEDOWN_ATTACK | POS_FACEDOWN_DEFENSE;
constexpr uint16_t POS_DEFENSE = POS_FACEUP_DEFENSE | POS_FACEDOWN_DEFENSE;
constexpr uint16_t PHASE_DRAW = 0x01, PHASE_STANDBY = 0x02, PHASE_MAIN1 = 0x04, PHASE_BATTLE_START = 0x08,
                    PHASE_BATTLE_STEP = 0x10, PHASE_DAMAGE = 0x20, PHASE_DAMAGE_CAL = 0x40, PHASE_BATTLE = 0x80,
                    PHASE_MAIN2 = 0x100, PHASE_END = 0x200;
}
using namespace cardbits;

namespace theme {
constexpr COLORREF background    = RGB(10, 16, 24);
constexpr COLORREF panel         = RGB(22, 32, 44);
constexpr COLORREF panelBorder   = RGB(58, 78, 96);
constexpr COLORREF field         = RGB(18, 46, 40);
constexpr COLORREF fieldZone     = RGB(27, 63, 55);
constexpr COLORREF fieldZoneLine = RGB(72, 112, 98);
constexpr COLORREF gold          = RGB(214, 178, 94);
constexpr COLORREF legal         = RGB(120, 196, 150);
constexpr COLORREF textPrimary   = RGB(232, 238, 240);
constexpr COLORREF textSecondary = RGB(158, 176, 186);
constexpr COLORREF danger        = RGB(214, 106, 96);
constexpr COLORREF cardBack      = RGB(46, 40, 92);
}

constexpr wchar_t kClassName[] = L"GoatSimulatorWindow";
constexpr int kMinWindowWidth = 1040;
constexpr int kMinWindowHeight = 660;
constexpr int kHeaderHeight = 76;
constexpr size_t kPromptRowsPerPage = 3;

enum class Screen { Title, Hub, Shop, PackOpening, Collection, DeckEditor, DeckList, CpuSelect, Duel };

// Control id for the one shared native text-entry child window used by every
// search/name box in the client (Collection search, Deck Editor search and
// "Save As" name entry) — there is no hand-rolled keyboard/caret handling
// anywhere else in this file, so a real EDIT control is the simplest correct
// way to get text input, IME support, and a blinking caret for free.
constexpr int kSearchEditId = 1001;

struct FieldCard { bool occupied{}; uint32_t code{}; uint8_t position{}; };

struct LegalAction { std::wstring label; fs::path image; uint32_t code{}; };

// Whenever every legal action is a numbered monster-zone placement (or a
// battle-phase "Attack with X"), those actions can be answered by clicking
// the board directly instead of a text list. Anything that can't be mapped
// this way always stays reachable via `panel_indices`.
struct ActionLayout {
    bool all_zone_placement = false;
    bool zone_placement_is_spell = false; // false = monster zones, true = spell/trap zones
    std::array<int, 5> zone_to_action{{-1, -1, -1, -1, -1}};
    bool has_attacks = false;
    std::unordered_map<int, int> attack_zone_to_action;
    int battle_phase_action = -1;
    int main_phase2_action = -1;
    int end_phase_action = -1;
    // Card-originated actions (Summon/Set/hand-Activate), keyed by card code,
    // so clicking that specific hand card can reveal just its own options.
    std::unordered_map<uint32_t, std::vector<size_t>> hand_actions;
    // Board-originated actions (Change Position/Activate for a card already on
    // the field), keyed by the player's own zone index. Zone index is
    // unambiguous (unlike hand cards, at most one card occupies a zone).
    std::unordered_map<int, std::vector<size_t>> monster_board_actions;
    std::unordered_map<int, std::vector<size_t>> spell_board_actions;
    std::vector<size_t> panel_indices;
};

// ---------- small pure utilities ----------

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return L"";
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return "";
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length, nullptr, nullptr);
    return result;
}

// Keeps a typed deck name usable as a Windows filename: strips path
// separators and other characters the shell would otherwise choke on.
std::wstring sanitize_filename(std::wstring name) {
    static const std::wstring forbidden = L"\\/:*?\"<>|";
    for (auto& ch : name) if (forbidden.find(ch) != std::wstring::npos) ch = L'_';
    while (!name.empty() && (name.front() == L' ' || name.back() == L' ')) { if (name.front() == L' ') name.erase(name.begin()); if (!name.empty() && name.back() == L' ') name.pop_back(); }
    return name;
}

std::wstring action_caption(std::wstring text) {
    const auto image = text.find(L" [");
    if (image != std::wstring::npos) text.erase(image);
    return text;
}

uint32_t parse_code_from_image_path(const fs::path& path) {
    const auto stem = path.stem().wstring();
    if (stem.empty()) return 0;
    for (wchar_t c : stem) if (!std::iswdigit(static_cast<wint_t>(c))) return 0;
    try { return static_cast<uint32_t>(std::stoul(stem)); } catch (...) { return 0; }
}

fs::path project_root() {
    std::array<wchar_t, 32768> buffer{};
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length) {
        const fs::path executable(std::wstring(buffer.data(), length));
        if (executable.parent_path().filename() == L"build") return executable.parent_path().parent_path();
    }
    return fs::current_path();
}

const wchar_t* attribute_name(uint32_t attribute) {
    switch (attribute) {
        case ATTRIBUTE_EARTH: return L"EARTH"; case ATTRIBUTE_WATER: return L"WATER";
        case ATTRIBUTE_FIRE: return L"FIRE"; case ATTRIBUTE_WIND: return L"WIND";
        case ATTRIBUTE_LIGHT: return L"LIGHT"; case ATTRIBUTE_DARK: return L"DARK";
        case ATTRIBUTE_DIVINE: return L"DIVINE"; default: return L"";
    }
}

const wchar_t* race_name(uint64_t race) {
    switch (race) {
        case RACE_WARRIOR: return L"Warrior"; case RACE_SPELLCASTER: return L"Spellcaster"; case RACE_FAIRY: return L"Fairy";
        case RACE_FIEND: return L"Fiend"; case RACE_ZOMBIE: return L"Zombie"; case RACE_MACHINE: return L"Machine";
        case RACE_AQUA: return L"Aqua"; case RACE_PYRO: return L"Pyro"; case RACE_ROCK: return L"Rock";
        case RACE_WINGEDBEAST: return L"Winged Beast"; case RACE_PLANT: return L"Plant"; case RACE_INSECT: return L"Insect";
        case RACE_THUNDER: return L"Thunder"; case RACE_DRAGON: return L"Dragon"; case RACE_BEAST: return L"Beast";
        case RACE_BEASTWARRIOR: return L"Beast-Warrior"; case RACE_DINOSAUR: return L"Dinosaur"; case RACE_FISH: return L"Fish";
        case RACE_SEASERPENT: return L"Sea Serpent"; case RACE_REPTILE: return L"Reptile"; case RACE_PSYCHIC: return L"Psychic";
        default: return L"";
    }
}

std::wstring describe_card_stats(const goat::CardDefinition& def) {
    if (!(def.type & TYPE_MONSTER)) return L"";
    std::wstring line = L"ATK " + std::to_wstring(def.attack) + L" / DEF " + std::to_wstring(def.defense);
    const uint32_t level = def.level & 0xffu;
    if (level) line += L" • Level " + std::to_wstring(level);
    return line;
}

std::wstring describe_card_type(const goat::CardDefinition& def) {
    if (def.type & TYPE_MONSTER) {
        std::vector<std::wstring> parts;
        const auto race = race_name(def.race);
        if (*race) parts.emplace_back(race);
        if (def.type & TYPE_FUSION) parts.emplace_back(L"Fusion");
        if (def.type & TYPE_RITUAL) parts.emplace_back(L"Ritual");
        if (def.type & TYPE_EFFECT) parts.emplace_back(L"Effect"); else if (def.type & TYPE_NORMAL) parts.emplace_back(L"Normal");
        if (def.type & TYPE_TUNER) parts.emplace_back(L"Tuner");
        if (def.type & TYPE_UNION) parts.emplace_back(L"Union");
        if (def.type & TYPE_SPIRIT) parts.emplace_back(L"Spirit");
        if (def.type & TYPE_GEMINI) parts.emplace_back(L"Gemini");
        if (def.type & TYPE_FLIP) parts.emplace_back(L"Flip");
        if (def.type & TYPE_TOON) parts.emplace_back(L"Toon");
        std::wstring line;
        for (size_t i = 0; i < parts.size(); ++i) { if (i) line += L" / "; line += parts[i]; }
        line += L" Monster";
        const auto attr = attribute_name(def.attribute);
        if (*attr) line += std::wstring(L" — ") + attr;
        return line;
    }
    if (def.type & TYPE_SPELL) {
        if (def.type & TYPE_QUICKPLAY) return L"Quick-Play Spell Card";
        if (def.type & TYPE_CONTINUOUS) return L"Continuous Spell Card";
        if (def.type & TYPE_EQUIP) return L"Equip Spell Card";
        if (def.type & TYPE_FIELD) return L"Field Spell Card";
        if (def.type & TYPE_RITUAL) return L"Ritual Spell Card";
        return L"Normal Spell Card";
    }
    if (def.type & TYPE_TRAP) {
        if (def.type & TYPE_CONTINUOUS) return L"Continuous Trap Card";
        if (def.type & TYPE_COUNTER) return L"Counter Trap Card";
        return L"Normal Trap Card";
    }
    return L"";
}

const wchar_t* phase_name(uint16_t phase) {
    if (phase & PHASE_DRAW) return L"Draw Phase";
    if (phase & PHASE_STANDBY) return L"Standby Phase";
    if (phase & PHASE_MAIN1) return L"Main Phase 1";
    if (phase & (PHASE_BATTLE_START | PHASE_BATTLE_STEP | PHASE_BATTLE)) return L"Battle Phase";
    if (phase & (PHASE_DAMAGE | PHASE_DAMAGE_CAL)) return L"Damage Step";
    if (phase & PHASE_MAIN2) return L"Main Phase 2";
    if (phase & PHASE_END) return L"End Phase";
    return L"";
}

// ---------- pure geometry: card footprints ----------

namespace {

Rect fit_card_in_cell(const Rect& cell) {
    Rect byHeight = card_from_height(cell.center_x(), cell.top, cell.height());
    if (byHeight.width() <= cell.width()) return byHeight;
    const int height = static_cast<int>(cell.width() / kCardAspect);
    return card_from_width(cell.center_x(), cell.top + (cell.height() - height) / 2, cell.width());
}

// A defense-position monster is rotated 90 degrees; its footprint keeps the
// upright card's width (so it stays aligned with the zone column) but its
// height shrinks to width*kCardAspect, matching a landscape card silhouette.
Rect defense_footprint(const Rect& cell) {
    const Rect upright = fit_card_in_cell(cell);
    const int width = upright.width();
    const int height = static_cast<int>(width * kCardAspect);
    const int top = cell.top + (cell.height() - height) / 2;
    return {upright.left, top, upright.left + width, top + height};
}

std::vector<Rect> layout_hand(const Rect& area, size_t count) {
    std::vector<Rect> rects;
    if (count == 0 || area.empty()) return rects;
    const int cardHeight = static_cast<int>(area.height() * 0.94);
    const int cardWidth = static_cast<int>(cardHeight * kCardAspect);
    if (cardWidth <= 0) return rects;
    const int gap = std::max(4, cardWidth / 6);
    int step = cardWidth + gap;
    int totalWidth = static_cast<int>(count) * cardWidth + static_cast<int>(count - 1) * gap;
    if (totalWidth > area.width() && count > 1) {
        step = std::max(cardWidth / 4, (area.width() - cardWidth) / static_cast<int>(count - 1));
        totalWidth = cardWidth + step * static_cast<int>(count - 1);
    }
    const int startX = area.left + (area.width() - totalWidth) / 2;
    const int top = area.top + (area.height() - cardHeight) / 2;
    for (size_t i = 0; i < count; ++i) {
        const int x = startX + static_cast<int>(i) * step;
        rects.push_back({x, top, x + cardWidth, top + cardHeight});
    }
    return rects;
}

// ---------- duel screen layout ----------

struct DuelLayout {
    Rect inspector;
    Rect opponent_hud, player_hud;
    Rect turn_indicator;
    Rect prompt_panel;
    Rect opponent_hand, player_hand;
    std::array<Rect, 5> opponent_monsters{}, player_monsters{};
    std::array<Rect, 6> opponent_spells{}, player_spells{}; // index 5 = field spell zone
    Rect opponent_deck, opponent_grave, opponent_banished, opponent_extra;
    Rect player_deck, player_grave, player_banished, player_extra;
};

DuelLayout compute_duel_layout(const Rect& client, const UiScale& scale) {
    DuelLayout L;
    Rect area = inset(client, scale.px(16));

    const int inspectorWidth = std::clamp(scale.px(260), 210, std::max(210, area.width() / 4));
    L.inspector = cut_left(area, inspectorWidth);
    cut_left(area, scale.px(14));

    L.opponent_hud = cut_top(area, scale.px(50));
    cut_top(area, scale.px(6));
    L.opponent_hand = cut_top(area, scale.px(56));
    cut_top(area, scale.px(6));

    L.prompt_panel = cut_bottom(area, scale.px(172));
    cut_bottom(area, scale.px(6));
    L.player_hud = cut_bottom(area, scale.px(50));
    cut_bottom(area, scale.px(6));
    L.player_hand = cut_bottom(area, scale.px(84));
    cut_bottom(area, scale.px(6));

    const int dividerHeight = scale.px(34);
    Rect opponentHalf = area;
    Rect playerHalf = cut_bottom(opponentHalf, std::max(0, (area.height() - dividerHeight) / 2));
    L.turn_indicator = cut_bottom(opponentHalf, dividerHeight);

    const int sideColumnWidth = std::clamp(scale.px(72), 44, std::max(44, opponentHalf.width() / 10));
    const int columnGap = scale.px(10);
    const int zoneGap = scale.px(8);

    // The player's row is a true point-symmetric mirror of the opponent's row
    // (as if the opponent's side of a physical mat were rotated 180 degrees):
    // deck/grave move to the east column instead of west, field/banish move
    // to west instead of east, and the five zone columns run right-to-left.
    // Zone *array index* still means "zone N" for engine/action purposes —
    // only which screen column each index is drawn in changes.
    auto layout_side = [&](Rect half, bool player_side, std::array<Rect, 5>& monsterZones, std::array<Rect, 6>& spellZones,
                           Rect& deckZone, Rect& graveZone, Rect& banishZone, Rect& extraZone) {
        Rect h = half;
        Rect leftCol = cut_left(h, sideColumnWidth);
        cut_left(h, columnGap);
        Rect rightCol = cut_right(h, sideColumnWidth);
        cut_right(h, columnGap);

        Rect& deckGraveCol = player_side ? rightCol : leftCol;
        Rect& fieldBanishCol = player_side ? leftCol : rightCol;
        // Player side also flips top/bottom within the column (Grave above
        // Deck, with Extra Deck between them) to match the point-symmetric
        // mirror of the opponent's row.
        const int third = deckGraveCol.height() / 3;
        if (player_side) {
            graveZone = cut_top(deckGraveCol, third);
            extraZone = cut_top(deckGraveCol, third);
            deckZone = deckGraveCol;
        } else {
            deckZone = cut_top(deckGraveCol, third);
            extraZone = cut_top(deckGraveCol, third);
            graveZone = deckGraveCol;
        }
        // The CPU's Field/Banish column runs Banish-above-Field (deliberately
        // not the same top/bottom order as the player's Field-above-Banish).
        Rect fieldSpellZone;
        if (player_side) { fieldSpellZone = cut_top(fieldBanishCol, fieldBanishCol.height() / 2); banishZone = fieldBanishCol; }
        else { banishZone = cut_top(fieldBanishCol, fieldBanishCol.height() / 2); fieldSpellZone = fieldBanishCol; }
        spellZones[5] = fieldSpellZone;

        // The monster row sits nearer the center divider and the S/T row sits
        // behind it, matching where each zone actually appears on a physical field.
        Rect monsterRow, spellRow;
        if (player_side) { monsterRow = cut_top(h, h.height() / 2); spellRow = h; }
        else { spellRow = cut_top(h, h.height() / 2); monsterRow = h; }

        const int zoneWidth = std::max(1, (monsterRow.width() - zoneGap * 4) / 5);
        for (int i = 0; i < 5; ++i) {
            const int column = player_side ? (4 - i) : i;
            const int x = monsterRow.left + column * (zoneWidth + zoneGap);
            monsterZones[i] = inset(Rect{x, monsterRow.top, x + zoneWidth, monsterRow.bottom}, 0, scale.px(2));
            spellZones[i] = inset(Rect{x, spellRow.top, x + zoneWidth, spellRow.bottom}, 0, scale.px(2));
        }
    };

    layout_side(opponentHalf, false, L.opponent_monsters, L.opponent_spells, L.opponent_deck, L.opponent_grave, L.opponent_banished, L.opponent_extra);
    layout_side(playerHalf, true, L.player_monsters, L.player_spells, L.player_deck, L.player_grave, L.player_banished, L.player_extra);
    return L;
}

struct PromptLayout {
    Rect title;
    std::vector<Rect> rows;
    Rect prev_button, next_button, pager_label;
    bool has_pager = false;
    size_t page_count = 1;
};

PromptLayout compute_prompt_layout(const Rect& panelRect, size_t action_count, const UiScale& scale) {
    PromptLayout out;
    Rect panel = inset(panelRect, scale.px(8));
    out.title = cut_top(panel, scale.px(20));
    out.page_count = std::max<size_t>(1, (action_count + kPromptRowsPerPage - 1) / kPromptRowsPerPage);
    out.has_pager = out.page_count > 1;
    if (out.has_pager) {
        Rect pager = cut_bottom(panel, scale.px(22));
        out.prev_button = cut_left(pager, scale.px(64));
        out.next_button = cut_right(pager, scale.px(64));
        out.pager_label = pager;
    }
    const int rowGap = scale.px(4);
    const int rowHeight = std::max(1, (panel.height() - rowGap * static_cast<int>(kPromptRowsPerPage - 1)) / static_cast<int>(kPromptRowsPerPage));
    for (size_t i = 0; i < kPromptRowsPerPage; ++i) {
        const int top = panel.top + static_cast<int>(i) * (rowHeight + rowGap);
        out.rows.push_back({panel.left, top, panel.right, top + rowHeight});
    }
    return out;
}

struct PhaseBarLayout { Rect label, battle, main2, end; };

// Always reserves the same three button slots regardless of which phases are
// currently legal, so the bar never jitters between frames — illegal buttons
// are simply rendered dim and are not clickable.
PhaseBarLayout compute_phase_bar_layout(const Rect& bar, const UiScale& scale) {
    PhaseBarLayout out;
    Rect r = bar;
    const int buttonWidth = std::clamp(scale.px(112), 70, std::max(70, r.width() / 4));
    const int gap = scale.px(6);
    out.end = cut_right(r, buttonWidth);
    cut_right(r, gap);
    out.main2 = cut_right(r, buttonWidth);
    cut_right(r, gap);
    out.battle = cut_right(r, buttonWidth);
    cut_right(r, gap);
    out.label = r;
    return out;
}

// The engine's action text repeats the card name ("Summon Protector of the
// Throne") which is redundant once the popup is already anchored to that
// specific card, so the popup shows just the verb.
std::wstring hand_action_short_label(const std::wstring& fullLabel) {
    static const std::vector<std::pair<std::wstring, std::wstring>> verbs = {
        {L"Special summon ", L"Special Summon"}, {L"Summon ", L"Summon"}, {L"Set monster ", L"Set (Defense)"},
        {L"Set spell/trap ", L"Set"}, {L"Activate ", L"Activate"},
    };
    for (const auto& [prefix, shortLabel] : verbs) if (fullLabel.rfind(prefix, 0) == 0) return shortLabel;
    return fullLabel;
}

struct HandPopupLayout { Rect panel; std::vector<Rect> buttons; };

HandPopupLayout compute_hand_popup_layout(const Rect& cardRect, size_t optionCount, const UiScale& scale, const Rect& clampBounds) {
    HandPopupLayout out;
    const int width = std::max(scale.px(150), cardRect.width() + scale.px(24));
    const int buttonHeight = scale.px(30);
    const int gap = scale.px(4);
    const int height = static_cast<int>(optionCount) * buttonHeight + static_cast<int>(optionCount > 0 ? optionCount - 1 : 0) * gap + scale.px(12);
    const int bottom = cardRect.top - scale.px(10);
    const int top = bottom - height;
    int left = cardRect.center_x() - width / 2;
    int right = left + width;
    if (left < clampBounds.left) { right += clampBounds.left - left; left = clampBounds.left; }
    if (right > clampBounds.right) { left -= right - clampBounds.right; right = clampBounds.right; }
    out.panel = {left, top, right, bottom};
    Rect inner = inset(out.panel, scale.px(6));
    for (size_t i = 0; i < optionCount; ++i) {
        const int y = inner.top + static_cast<int>(i) * (buttonHeight + gap);
        out.buttons.push_back({inner.left, y, inner.right, y + buttonHeight});
    }
    return out;
}

// ---------- menu screen layouts ----------

struct TitleLayout { Rect cta; };
TitleLayout compute_title_layout(const Rect& client, const UiScale& scale) {
    Rect area = inset(client, scale.px(28));
    cut_top(area, scale.px(kHeaderHeight));
    Rect bottom = cut_bottom(area, static_cast<int>(area.height() * 0.35));
    return {centered(bottom, std::min(bottom.width(), scale.px(320)), scale.px(76))};
}

struct HubLayout { Rect header; std::array<Rect, 4> nav{}; std::array<Rect, 2> select{}; Rect details, status; };
HubLayout compute_hub_layout(const Rect& client, const UiScale& scale) {
    HubLayout L;
    Rect area = inset(client, scale.px(28));
    cut_top(area, scale.px(kHeaderHeight));
    L.header = cut_top(area, scale.px(30));
    cut_top(area, scale.px(14));
    Rect navRow = cut_top(area, scale.px(84));
    cut_top(area, scale.px(14));
    Rect selectRow = cut_top(area, scale.px(84));
    cut_top(area, scale.px(14));
    L.details = cut_top(area, scale.px(56));
    L.status = cut_bottom(area, scale.px(40));

    const int navGap = scale.px(14);
    const int navWidth = (navRow.width() - navGap * 3) / 4;
    for (int i = 0; i < 4; ++i) { const int x = navRow.left + i * (navWidth + navGap); L.nav[i] = {x, navRow.top, x + navWidth, navRow.bottom}; }
    const int selGap = scale.px(14);
    const int selWidth = (selectRow.width() - selGap) / 2;
    for (int i = 0; i < 2; ++i) { const int x = selectRow.left + i * (selWidth + selGap); L.select[i] = {x, selectRow.top, x + selWidth, selectRow.bottom}; }
    return L;
}

// A reusable multi-row-and-column card grid for one "page" worth of tiles,
// shared by the Shop pack grid, the Collection browser, the Deck Editor's
// owned-card pool, and the Pack Opening reveal grid — rather than four
// near-identical grid-math blocks. Each tile reserves `labelHeight` below its
// art for a name/count caption, matching how the Collection screen already
// draws that caption.
struct CardGridLayout { std::vector<Rect> cards; int columns = 1; int rows = 1; size_t per_page = 1; size_t page_count = 1; };
CardGridLayout compute_card_grid_layout(const Rect& area, size_t total_count, const UiScale& scale) {
    CardGridLayout L;
    const int labelHeight = scale.px(30);
    const int gap = scale.px(14);
    const int cardWidth = std::max(1, scale.px(104));
    const int cardHeight = std::max(1, static_cast<int>(cardWidth / kCardAspect));
    L.columns = std::max(1, (area.width() + gap) / (cardWidth + gap));
    L.rows = std::max(1, (area.height() + gap) / (cardHeight + labelHeight + gap));
    L.per_page = static_cast<size_t>(L.columns) * static_cast<size_t>(L.rows);
    L.page_count = std::max<size_t>(1, (total_count + L.per_page - 1) / L.per_page);
    const int totalWidth = L.columns * cardWidth + (L.columns - 1) * gap;
    const int totalHeight = L.rows * (cardHeight + labelHeight) + (L.rows - 1) * gap;
    const int startX = area.left + std::max(0, (area.width() - totalWidth) / 2);
    const int startY = area.top + std::max(0, (area.height() - totalHeight) / 2);
    for (int r = 0; r < L.rows; ++r) {
        for (int c = 0; c < L.columns; ++c) {
            const int x = startX + c * (cardWidth + gap);
            const int y = startY + r * (cardHeight + labelHeight + gap);
            L.cards.push_back({x, y, x + cardWidth, y + cardHeight});
        }
    }
    return L;
}

struct ShopLayout { Rect header; Rect grid_area; CardGridLayout grid; Rect detail; std::array<Rect, 2> buttons{}; Rect status; };
ShopLayout compute_shop_layout(const Rect& client, const UiScale& scale, size_t pack_count) {
    ShopLayout L;
    Rect area = inset(client, scale.px(28));
    cut_top(area, scale.px(kHeaderHeight));
    L.header = cut_top(area, scale.px(34));
    cut_top(area, scale.px(10));
    L.status = cut_bottom(area, scale.px(40));

    const int detailWidth = std::clamp(scale.px(260), 200, std::max(200, area.width() / 3));
    Rect detailArea = cut_right(area, detailWidth);
    cut_right(area, scale.px(14));
    L.grid_area = area;
    L.grid = compute_card_grid_layout(area, pack_count, scale);

    Rect buttonRow = cut_bottom(detailArea, scale.px(44));
    cut_bottom(detailArea, scale.px(10));
    const int gap = scale.px(10);
    const int w = (buttonRow.width() - gap) / 2;
    L.buttons[0] = {buttonRow.left, buttonRow.top, buttonRow.left + w, buttonRow.bottom};
    L.buttons[1] = {buttonRow.right - w, buttonRow.top, buttonRow.right, buttonRow.bottom};
    L.detail = detailArea;
    return L;
}

struct PackOpeningLayout { Rect header; Rect grid_area; CardGridLayout grid; Rect detail; Rect reveal_all; Rect done; Rect status; };
PackOpeningLayout compute_pack_opening_layout(const Rect& client, const UiScale& scale, size_t card_count) {
    PackOpeningLayout L;
    Rect area = inset(client, scale.px(28));
    cut_top(area, scale.px(kHeaderHeight));
    L.header = cut_top(area, scale.px(34));
    cut_top(area, scale.px(10));
    L.status = cut_bottom(area, scale.px(40));
    Rect buttonRow = cut_bottom(area, scale.px(44));
    cut_bottom(area, scale.px(10));
    const int gap = scale.px(10);
    const int w = (buttonRow.width() - gap) / 2;
    L.reveal_all = {buttonRow.left, buttonRow.top, buttonRow.left + w, buttonRow.bottom};
    L.done = {buttonRow.right - w, buttonRow.top, buttonRow.right, buttonRow.bottom};

    const int detailWidth = std::clamp(scale.px(240), 180, std::max(180, area.width() / 4));
    L.detail = cut_right(area, detailWidth);
    cut_right(area, scale.px(14));
    L.grid_area = area;
    L.grid = compute_card_grid_layout(area, card_count, scale);
    return L;
}

struct CollectionLayout { Rect header; Rect search_box; std::array<Rect, 3> filters{}; Rect grid_area; CardGridLayout grid; Rect detail; std::array<Rect, 3> buttons{}; Rect status; };
CollectionLayout compute_collection_layout(const Rect& client, const UiScale& scale, size_t visible_count) {
    CollectionLayout L;
    Rect area = inset(client, scale.px(28));
    cut_top(area, scale.px(kHeaderHeight));
    L.header = cut_top(area, scale.px(34));
    cut_top(area, scale.px(10));
    L.status = cut_bottom(area, scale.px(40));
    Rect buttonRow = cut_bottom(area, scale.px(60));
    cut_bottom(area, scale.px(14));
    const int gap = scale.px(14);
    const int w = (buttonRow.width() - gap * 2) / 3;
    L.buttons[0] = {buttonRow.left, buttonRow.top, buttonRow.left + w, buttonRow.bottom};
    L.buttons[1] = {buttonRow.left + w + gap, buttonRow.top, buttonRow.left + 2 * w + gap, buttonRow.bottom};
    L.buttons[2] = {buttonRow.right - w, buttonRow.top, buttonRow.right, buttonRow.bottom};

    Rect searchRow = cut_top(area, scale.px(30));
    cut_top(area, scale.px(8));
    L.search_box = cut_left(searchRow, std::max(scale.px(160), searchRow.width() * 2 / 5));
    cut_left(searchRow, scale.px(10));
    const int filterGap = scale.px(8);
    const int filterWidth = (searchRow.width() - filterGap * 2) / 3;
    for (int i = 0; i < 3; ++i) { const int x = searchRow.left + i * (filterWidth + filterGap); L.filters[static_cast<size_t>(i)] = {x, searchRow.top, x + filterWidth, searchRow.bottom}; }
    cut_top(area, scale.px(10));

    const int detailWidth = std::clamp(scale.px(240), 180, std::max(180, area.width() / 4));
    L.detail = cut_right(area, detailWidth);
    cut_right(area, scale.px(14));
    L.grid_area = area;
    L.grid = compute_card_grid_layout(area, visible_count, scale);
    return L;
}

// Lays out `count` rows in as many left-to-right columns as needed to fit
// within `area`'s height, each row `row_height` tall — used for the Deck
// List screen's Main/Extra panels so every card is always reachable however
// many unique entries there are, instead of truncating past one column's
// worth of rows the way a single scroll-less list would.
struct ColumnListLayout { std::vector<Rect> rows; int columns = 1; };
ColumnListLayout compute_column_list_layout(const Rect& area, size_t count, int row_height, const UiScale& scale) {
    ColumnListLayout L;
    if (count == 0 || area.empty()) return L;
    const int columnGap = scale.px(16);
    const int minColumnWidth = scale.px(150);
    const int maxRowsPerColumn = std::max(1, area.height() / row_height);
    const int neededColumns = static_cast<int>((count + static_cast<size_t>(maxRowsPerColumn) - 1) / static_cast<size_t>(maxRowsPerColumn));
    const int widthLimitedColumns = std::max(1, (area.width() + columnGap) / (minColumnWidth + columnGap));
    L.columns = std::clamp(neededColumns, 1, widthLimitedColumns);
    const int rowsPerColumn = static_cast<int>((count + static_cast<size_t>(L.columns) - 1) / static_cast<size_t>(L.columns));
    const int columnWidth = (area.width() - columnGap * (L.columns - 1)) / L.columns;
    for (size_t i = 0; i < count; ++i) {
        const int col = static_cast<int>(i) / rowsPerColumn;
        const int row = static_cast<int>(i) % rowsPerColumn;
        const int x = area.left + col * (columnWidth + columnGap);
        const int y = area.top + row * row_height;
        L.rows.push_back(y + row_height <= area.bottom ? Rect{x, y, x + columnWidth, y + row_height} : Rect{});
    }
    return L;
}

// Buttons: Save, Save As, Load, Set Active, View Decklist, Back.
struct DeckEditorLayout {
    Rect header; Rect search_box; std::array<Rect, 3> filters{};
    Rect pool_area; CardGridLayout pool_grid;
    Rect pool_prev, pool_next, pool_page_label;
    Rect detail;
    std::array<Rect, 6> buttons{}; Rect status;
};
DeckEditorLayout compute_deck_editor_layout(const Rect& client, const UiScale& scale, size_t pool_count) {
    DeckEditorLayout L;
    Rect area = inset(client, scale.px(28));
    cut_top(area, scale.px(kHeaderHeight));
    L.header = cut_top(area, scale.px(34));
    cut_top(area, scale.px(10));
    L.status = cut_bottom(area, scale.px(40));
    Rect buttonRow = cut_bottom(area, scale.px(44));
    cut_bottom(area, scale.px(10));
    const int gap = scale.px(8);
    const int w = (buttonRow.width() - gap * 5) / 6;
    for (int i = 0; i < 6; ++i) { const int x = buttonRow.left + i * (w + gap); L.buttons[static_cast<size_t>(i)] = {x, buttonRow.top, x + w, buttonRow.bottom}; }

    Rect searchRow = cut_top(area, scale.px(30));
    cut_top(area, scale.px(8));
    L.search_box = cut_left(searchRow, std::max(scale.px(160), searchRow.width() * 2 / 5));
    cut_left(searchRow, scale.px(10));
    const int filterGap = scale.px(8);
    const int filterWidth = (searchRow.width() - filterGap * 2) / 3;
    for (int i = 0; i < 3; ++i) { const int x = searchRow.left + i * (filterWidth + filterGap); L.filters[static_cast<size_t>(i)] = {x, searchRow.top, x + filterWidth, searchRow.bottom}; }
    cut_top(area, scale.px(10));

    // The Main/Extra decklists now live on their own dedicated screen (see
    // DeckListLayout), so this screen is purely "browse owned cards, click to
    // add" — the right column only needs the detail panel, freeing the rest
    // of the width for a bigger pool grid.
    const int detailWidth = std::clamp(scale.px(260), 200, std::max(200, area.width() / 4));
    L.detail = cut_right(area, detailWidth);
    cut_right(area, scale.px(14));

    // A small pager strip along the bottom of the pool panel — the pool can
    // easily hold more owned cards than fit on one page, and the global
    // button row below is already full, so paging lives here instead, the
    // same way the duel prompt panel's own pager is local to that panel
    // rather than sharing the screen's buttons.
    Rect pagerRow = cut_bottom(area, scale.px(26));
    cut_bottom(area, scale.px(6));
    const int pagerButtonWidth = scale.px(70);
    L.pool_prev = {pagerRow.left, pagerRow.top, pagerRow.left + pagerButtonWidth, pagerRow.bottom};
    L.pool_next = {pagerRow.right - pagerButtonWidth, pagerRow.top, pagerRow.right, pagerRow.bottom};
    L.pool_page_label = {L.pool_prev.right + scale.px(8), pagerRow.top, L.pool_next.left - scale.px(8), pagerRow.bottom};

    L.pool_area = area;
    L.pool_grid = compute_card_grid_layout(area, pool_count, scale);
    return L;
}

// Buttons: Save, Save As, Load, Set Active, Add Cards, Back.
struct DeckListLayout {
    Rect header;
    Rect main_panel, main_list; ColumnListLayout main_columns;
    Rect extra_panel, extra_list; ColumnListLayout extra_columns;
    std::array<Rect, 6> buttons{}; Rect status;
};
DeckListLayout compute_deck_list_layout(const Rect& client, const UiScale& scale, size_t main_row_count, size_t extra_row_count) {
    DeckListLayout L;
    Rect area = inset(client, scale.px(28));
    cut_top(area, scale.px(kHeaderHeight));
    L.header = cut_top(area, scale.px(34));
    cut_top(area, scale.px(14));
    L.status = cut_bottom(area, scale.px(40));
    Rect buttonRow = cut_bottom(area, scale.px(44));
    cut_bottom(area, scale.px(14));
    const int gap = scale.px(8);
    const int w = (buttonRow.width() - gap * 5) / 6;
    for (int i = 0; i < 6; ++i) { const int x = buttonRow.left + i * (w + gap); L.buttons[static_cast<size_t>(i)] = {x, buttonRow.top, x + w, buttonRow.bottom}; }

    // Main Deck gets roughly twice Extra Deck's width (it commonly has far
    // more unique entries), with the full remaining screen height for both —
    // easily enough room for every realistic deck size across a few columns.
    const int extraWidth = std::clamp(area.width() * 3 / 8, scale.px(220), area.width() / 2);
    L.extra_panel = cut_right(area, extraWidth);
    cut_right(area, scale.px(16));
    L.main_panel = area;

    const int rowHeight = scale.px(24);
    auto layout_panel = [&](const Rect& panel, size_t count, Rect& listArea, ColumnListLayout& columns) {
        Rect inner = inset(panel, scale.px(12));
        cut_top(inner, scale.px(26)); // header line, drawn separately
        listArea = inner;
        columns = compute_column_list_layout(inner, count, rowHeight, scale);
    };
    layout_panel(L.main_panel, main_row_count, L.main_list, L.main_columns);
    layout_panel(L.extra_panel, extra_row_count, L.extra_list, L.extra_columns);
    return L;
}

// Buttons: Prev, Next, Back.
struct CpuSelectLayout { Rect header; std::vector<Rect> rows; std::array<Rect, 3> buttons{}; Rect status; size_t per_page = 1; size_t page_count = 1; };
CpuSelectLayout compute_cpu_select_layout(const Rect& client, const UiScale& scale, size_t npc_count) {
    CpuSelectLayout L;
    Rect area = inset(client, scale.px(28));
    cut_top(area, scale.px(kHeaderHeight));
    L.header = cut_top(area, scale.px(34));
    cut_top(area, scale.px(10));
    L.status = cut_bottom(area, scale.px(40));
    Rect buttonRow = cut_bottom(area, scale.px(50));
    cut_bottom(area, scale.px(14));
    const int gap = scale.px(14);
    const int w = (buttonRow.width() - gap * 2) / 3;
    L.buttons[0] = {buttonRow.left, buttonRow.top, buttonRow.left + w, buttonRow.bottom};
    L.buttons[1] = {buttonRow.left + w + gap, buttonRow.top, buttonRow.left + 2 * w + gap, buttonRow.bottom};
    L.buttons[2] = {buttonRow.right - w, buttonRow.top, buttonRow.right, buttonRow.bottom};

    // The roster grew past what always fit in one screen's worth of rows
    // (12 NPCs at ~86px each needs more height than a typical window has),
    // so this pages the same way Collection/Deck Editor do rather than
    // silently truncating the rows that don't fit.
    const int rowGap = scale.px(12);
    const int rowHeight = scale.px(74);
    L.per_page = std::max<size_t>(1, static_cast<size_t>((area.height() + rowGap) / (rowHeight + rowGap)));
    L.page_count = std::max<size_t>(1, (npc_count + L.per_page - 1) / L.per_page);
    for (size_t i = 0; i < L.per_page; ++i) {
        L.rows.push_back(cut_top(area, rowHeight));
        cut_top(area, rowGap);
    }
    return L;
}

} // namespace

namespace {

// ---------- application state ----------
// Duel-board fields here mirror only what the engine bridge publishes in
// state.txt; interaction state (hover/last-inspected) is kept alongside but
// never fed back into anything the engine reads, keeping the UI/engine
// boundary one-directional.
struct AppState {
    HBITMAP featured_card{};
    HBITMAP card_back_texture{};
    HBITMAP title_background{};
    Screen screen{Screen::Title};
    std::wstring status = L"Choose a mode to begin your GOAT Format campaign.";
    std::wstring prompt_title;
    goat::game::Progression progression{goat::game::ProfileStore::load("saves/default.sav")};
    goat::game::Catalog catalog{goat::game::load_catalog("data/npcs.json", "data/packs.json")};
    std::mt19937 random{std::random_device{}()};
    PROCESS_INFORMATION player_process{};
    fs::path session_directory = fs::path("sessions") / "active-duel";
    std::vector<LegalAction> legal_actions;
    ActionLayout action_layout;
    size_t action_page{};
    size_t selected_npc{};
    size_t collection_page{};
    size_t selected_pack{};
    std::unique_ptr<goat::CardDatabase> card_database =
        std::make_unique<goat::CardDatabase>("external/BabelCDB/cards.cdb", "external/BabelCDB/goat-entries.cdb");

    // ---- shared search/name text entry (one native EDIT control, moved and
    // shown/hidden per screen; see kSearchEditId and window_proc's WM_COMMAND) ----
    HWND search_edit{};
    Rect search_edit_last_box{}; // last rect actually applied — see position_search_edit
    bool search_edit_visible = false;
    std::wstring search_text;
    bool naming_deck{}; // true: the shared edit control's text goes to deck_new_name (Save As) instead of search_text

    // ---- Shop / Pack Opening (see paint_shop / paint_pack_opening) ----
    std::vector<uint32_t> opening_cards;
    size_t opening_revealed{};
    DWORD opening_reveal_tick{};
    DWORD opening_flip_start_tick{};
    int opening_selected = -1;

    // ---- Collection browser (see paint_collection) ----
    bool collection_filter_monster = true, collection_filter_spell = true, collection_filter_trap = true;
    uint32_t browse_selected_code{}; // shared by Collection and the Deck Editor pool's detail panel

    // ---- Deck Editor (see paint_deck_editor) ----
    goat::game::DeckContents editing_deck;
    std::string editing_deck_path;
    bool deck_editor_dirty = false;
    size_t deck_pool_page{};
    bool deck_filter_monster = true, deck_filter_spell = true, deck_filter_trap = true;
    int deck_list_selected = -1; // index into the combined main+extra rows shown in the current-deck panel
    bool deck_save_as_active = false;
    std::wstring deck_new_name;
    std::wstring deck_status; // legality banner, recomputed after every deck edit

    std::array<int, 2> life{{8000, 8000}};
    std::array<double, 2> life_display{{8000.0, 8000.0}};
    std::array<uint32_t, 2> hand_count{};
    std::array<uint32_t, 2> deck_count{};
    std::array<uint32_t, 2> grave_count{};
    std::array<uint32_t, 2> extra_count{};
    std::array<uint32_t, 2> banished_count{};
    std::array<std::array<FieldCard, 5>, 2> monsters{};
    std::array<std::array<FieldCard, 6>, 2> spells{};
    std::vector<uint32_t> hand_cards;
    int open_hand_card = -1;         // index into hand_cards whose per-card action popup is showing, or -1
    int selected_monster_zone = -1;  // own monster zone whose action buttons show in the inspector, or -1
    int selected_spell_zone = -1;    // own spell/trap zone whose action buttons show in the inspector, or -1
    bool duel_is_test_mode = false;  // true: no ownership/banlist checks, no rewards on completion
    DWORD last_submit_tick = 0;      // debounce window after any submit_action (see handle_duel_click)
    // The write-time of the request.txt this client has already ingested (or
    // is currently waiting on the engine to replace) — see poll_player_duel.
    fs::file_time_type last_request_write_time{};
    bool cpu_select_test_mode = false; // which mode the CpuSelect roster screen was entered in
    size_t cpu_select_page{};
    uint8_t turn_player{};
    uint32_t turn_number{};
    uint16_t phase{};
    std::string last_board_snapshot;

    uint32_t last_inspected_code{};
    bool last_inspected_is_back{};

    std::unordered_map<uint32_t, HBITMAP> texture_cache;
    std::unordered_map<std::string, HBITMAP> pack_texture_cache;

    HDC buffer_dc{};
    HBITMAP buffer_bitmap{};
    int buffer_width{};
    int buffer_height{};
    bool paint_pending{};
};

HBITMAP load_jpeg(const fs::path& file, const COLORREF* flatten_over = nullptr);
void paint_pack_opening(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY);
void paint_deck_editor(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY);
void paint_deck_list(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY);
void open_deck_editor(AppState& state, const std::string& deck_path);
void handle_deck_editor_click(AppState& state, const Rect& client, const UiScale& scale, int x, int y);
void handle_deck_list_click(AppState& state, const Rect& client, const UiScale& scale, int x, int y);

// Moves/shows the one shared search-or-name EDIT control (see kSearchEditId)
// into `box` for the screen currently painting it; every other screen calls
// hide_search_edit so it never floats over unrelated UI.
void position_search_edit(AppState& state, const Rect& box) {
    if (!state.search_edit) return;
    // Every screen that owns this control recomputes its layout (and calls
    // this) on every repaint, including on mere mouse-move — MoveWindow's
    // repaint flag forces the child to redraw itself even when its rect is
    // unchanged, which against a parent that repaints just as often reads as
    // a constant flicker. Only touch the child window when something about
    // its state actually changed.
    const auto& last = state.search_edit_last_box;
    if (box.left != last.left || box.top != last.top || box.right != last.right || box.bottom != last.bottom) {
        MoveWindow(state.search_edit, box.left, box.top, box.width(), box.height(), TRUE);
        state.search_edit_last_box = box;
    }
    if (!state.search_edit_visible) { ShowWindow(state.search_edit, SW_SHOW); state.search_edit_visible = true; }
}
void hide_search_edit(AppState& state) {
    if (!state.search_edit) return;
    if (state.search_edit_visible) { ShowWindow(state.search_edit, SW_HIDE); state.search_edit_visible = false; }
}

// A code->texture cache so field/hand/inspector art is decoded from disk once
// per card instead of every repaint (the previous client re-decoded JPEGs on
// every WM_PAINT for every visible card).
HBITMAP get_card_texture(AppState& state, uint32_t code) {
    if (code == 0) return nullptr;
    const auto it = state.texture_cache.find(code);
    if (it != state.texture_cache.end()) return it->second;
    HBITMAP bitmap = load_jpeg(fs::path("external/card_images") / (std::to_string(code) + ".jpg"));
    if (!bitmap) {
        // Cards resolved through goat-entries.cdb (a GOAT-accurate ruling
        // variant) live on the board under a synthetic id distinct from the
        // real card's id, but art is only ever filed under the real id
        // (CardDefinition::alias) — fall back to that before giving up.
        const auto& def = state.card_database->resolve(code);
        if (def.alias != 0 && def.alias != code) bitmap = load_jpeg(fs::path("external/card_images") / (std::to_string(def.alias) + ".jpg"));
    }
    state.texture_cache.emplace(code, bitmap);
    return bitmap;
}

// Same cache-once-per-image idea as get_card_texture, keyed by the pack's art
// filename since packs (unlike cards) aren't identified by a numeric code.
HBITMAP get_pack_texture(AppState& state, const goat::game::Pack& pack) {
    const auto it = state.pack_texture_cache.find(pack.art);
    if (it != state.pack_texture_cache.end()) return it->second;
    HBITMAP bitmap = load_jpeg(fs::path("external/packart") / pack.art);
    state.pack_texture_cache.emplace(pack.art, bitmap);
    return bitmap;
}

ActionLayout build_action_layout(AppState& state) {
    ActionLayout layout;
    bool everyEntryIsZonePlacement = !state.legal_actions.empty();
    std::unordered_set<size_t> mappedAttacks;
    std::unordered_set<size_t> mappedHandActions;
    std::unordered_set<size_t> mappedBoardActions;
    // MSG_SELECT_PLACE can ask for either a monster zone or a spell/trap zone
    // (e.g. Set Spell/Trap) — both use this same "Place in <kind> zone N" text.
    static const std::wstring monsterZonePrefix = L"Place in monster zone ";
    static const std::wstring spellZonePrefix = L"Place in spell/trap zone ";
    static const std::wstring attackPrefix = L"Attack with ";
    static const std::array<std::wstring, 4> handVerbs = {L"Summon ", L"Special summon ", L"Set monster ", L"Set spell/trap "};
    static const std::wstring changePositionPrefix = L"Change position ";
    static const std::wstring activatePrefix = L"Activate ";

    for (size_t i = 0; i < state.legal_actions.size(); ++i) {
        const auto& action = state.legal_actions[i];
        const std::wstring& text = action.label;
        bool isZonePlacement = false;
        const bool isSpellZone = text.rfind(spellZonePrefix, 0) == 0;
        const bool isMonsterZone = !isSpellZone && text.rfind(monsterZonePrefix, 0) == 0;
        if (isSpellZone || isMonsterZone) {
            const size_t prefixLength = isSpellZone ? spellZonePrefix.size() : monsterZonePrefix.size();
            try {
                const int zoneNum = std::stoi(text.substr(prefixLength));
                if (zoneNum >= 1 && zoneNum <= 5) {
                    layout.zone_to_action[static_cast<size_t>(zoneNum - 1)] = static_cast<int>(i);
                    layout.zone_placement_is_spell = isSpellZone;
                    isZonePlacement = true;
                }
            } catch (...) {}
        }
        if (!isZonePlacement) everyEntryIsZonePlacement = false;

        if (text.rfind(attackPrefix, 0) == 0) {
            layout.has_attacks = true;
            for (int slot = 0; slot < 5; ++slot) {
                const auto& card = state.monsters[0][static_cast<size_t>(slot)];
                if (!card.occupied || card.code == 0 || card.code != action.code) continue;
                if (layout.attack_zone_to_action.count(slot)) continue;
                layout.attack_zone_to_action[slot] = static_cast<int>(i);
                mappedAttacks.insert(i);
                break;
            }
            continue;
        }

        if (text == L"Enter Battle Phase") { layout.battle_phase_action = static_cast<int>(i); continue; }
        if (text == L"Main Phase 2") { layout.main_phase2_action = static_cast<int>(i); continue; }
        if (text == L"End Phase") { layout.end_phase_action = static_cast<int>(i); continue; }

        if (action.code != 0 && !isZonePlacement) {
            bool isHandVerb = false;
            for (const auto& verb : handVerbs) if (text.rfind(verb, 0) == 0) { isHandVerb = true; break; }
            const bool isChangePosition = text.rfind(changePositionPrefix, 0) == 0;
            const bool isActivate = text.rfind(activatePrefix, 0) == 0;
            const bool inHand = std::find(state.hand_cards.begin(), state.hand_cards.end(), action.code) != state.hand_cards.end();
            // "Activate" can name either a hand card (quick-play) or a board
            // card (a set trap, a continuous effect); only route it to the
            // per-card hand popup when the named card is actually in hand,
            // otherwise try to match it to the board zone it's actually on.
            if (isHandVerb || (isActivate && inHand)) {
                // Multiple copies of the same card in hand each produce their
                // own legal-action entry from the engine (one per physical
                // hand slot), but they're indistinguishable to the player —
                // summoning "the" copy of a card has no difference from
                // summoning "another" copy. Without this dedupe, N copies of
                // a card produced N identical "Summon"/"Set"/"Activate"
                // buttons in its popup; keep just the first of each verb.
                auto& actions = layout.hand_actions[action.code];
                const bool alreadyHasVerb = std::any_of(actions.begin(), actions.end(), [&](size_t existing) {
                    return hand_action_short_label(state.legal_actions[existing].label) == hand_action_short_label(text);
                });
                if (!alreadyHasVerb) actions.push_back(i);
                mappedHandActions.insert(i);
            } else if (isChangePosition || isActivate) {
                bool matched = false;
                for (int slot = 0; slot < 5 && !matched; ++slot) {
                    const auto& card = state.monsters[0][static_cast<size_t>(slot)];
                    if (card.occupied && card.code == action.code) {
                        layout.monster_board_actions[slot].push_back(i);
                        mappedBoardActions.insert(i);
                        matched = true;
                    }
                }
                for (int slot = 0; slot < 6 && !matched && isActivate; ++slot) {
                    const auto& card = state.spells[0][static_cast<size_t>(slot)];
                    if (card.occupied && card.code == action.code) {
                        layout.spell_board_actions[slot].push_back(i);
                        mappedBoardActions.insert(i);
                        matched = true;
                    }
                }
                // If no board zone matches either, it falls through to the
                // panel below rather than silently disappearing.
            }
        }
    }
    layout.all_zone_placement = everyEntryIsZonePlacement;

    for (size_t i = 0; i < state.legal_actions.size(); ++i) {
        if (layout.all_zone_placement) continue; // fully handled by zone glow instead
        if (mappedAttacks.count(i)) continue;     // fully handled by clicking the attacker
        if (mappedHandActions.count(i)) continue; // fully handled by clicking the hand card
        if (mappedBoardActions.count(i)) continue; // fully handled by clicking the board card
        if (static_cast<int>(i) == layout.battle_phase_action || static_cast<int>(i) == layout.main_phase2_action || static_cast<int>(i) == layout.end_phase_action) continue; // handled by phase buttons
        layout.panel_indices.push_back(i);
    }
    return layout;
}

struct DuelHover { uint32_t inspect_code = 0; bool inspect_is_back = false; int hand_index = -1; int player_zone = -1; };

DuelHover compute_duel_hover(AppState& state, const DuelLayout& L, int x, int y) {
    DuelHover hover;
    const auto handRects = layout_hand(L.player_hand, state.hand_cards.size());
    for (int i = static_cast<int>(handRects.size()) - 1; i >= 0; --i) {
        if (handRects[static_cast<size_t>(i)].contains(x, y)) { hover.hand_index = i; hover.inspect_code = state.hand_cards[static_cast<size_t>(i)]; return hover; }
    }
    const auto oppHandRects = layout_hand(L.opponent_hand, state.hand_count[1]);
    for (const auto& r : oppHandRects) if (r.contains(x, y)) { hover.inspect_is_back = true; return hover; }

    for (int i = 0; i < 5; ++i) {
        const size_t si = static_cast<size_t>(i);
        if (L.player_monsters[si].contains(x, y)) {
            hover.player_zone = i;
            // The engine never redacts the player's own cards (only the
            // opponent's face-down code is zeroed), so a set monster/spell is
            // just as inspectable as a face-up one — only its code being 0
            // (which never happens for our own occupied zones) means unknown.
            const auto& card = state.monsters[0][si];
            if (card.occupied) { if (card.code == 0) hover.inspect_is_back = true; else hover.inspect_code = card.code; }
            return hover;
        }
        if (L.player_spells[si].contains(x, y)) {
            const auto& card = state.spells[0][si];
            if (card.occupied) { if (card.code == 0) hover.inspect_is_back = true; else hover.inspect_code = card.code; }
            return hover;
        }
        if (L.opponent_monsters[si].contains(x, y)) {
            const auto& card = state.monsters[1][si];
            if (card.occupied) { if (card.code == 0) hover.inspect_is_back = true; else hover.inspect_code = card.code; }
            return hover;
        }
        if (L.opponent_spells[si].contains(x, y)) {
            const auto& card = state.spells[1][si];
            if (card.occupied) { if (card.code == 0) hover.inspect_is_back = true; else hover.inspect_code = card.code; }
            return hover;
        }
    }
    if (L.player_spells[5].contains(x, y)) { const auto& card = state.spells[0][5]; if (card.occupied) hover.inspect_code = card.code; return hover; }
    if (L.opponent_spells[5].contains(x, y)) { const auto& card = state.spells[1][5]; if (card.occupied && card.code) hover.inspect_code = card.code; return hover; }
    return hover;
}

// ---------- board-card selection (inspector-panel action buttons) ----------
// Clicking an occupied own monster/spell zone (outside zone-placement/attack
// mode) selects it; the inspector then shows buttons for just that card's
// legal actions (Change Position, Activate) instead of a flat list entry.

struct BoardSelection { uint32_t code = 0; const std::vector<size_t>* actions = nullptr; };

BoardSelection resolve_board_selection(AppState& state) {
    BoardSelection info;
    if (state.selected_monster_zone >= 0 && static_cast<size_t>(state.selected_monster_zone) < 5) {
        const auto& card = state.monsters[0][static_cast<size_t>(state.selected_monster_zone)];
        if (card.occupied) info.code = card.code;
        const auto it = state.action_layout.monster_board_actions.find(state.selected_monster_zone);
        if (it != state.action_layout.monster_board_actions.end()) info.actions = &it->second;
    } else if (state.selected_spell_zone >= 0 && static_cast<size_t>(state.selected_spell_zone) < 6) {
        const auto& card = state.spells[0][static_cast<size_t>(state.selected_spell_zone)];
        if (card.occupied) info.code = card.code;
        const auto it = state.action_layout.spell_board_actions.find(state.selected_spell_zone);
        if (it != state.action_layout.spell_board_actions.end()) info.actions = &it->second;
    }
    return info;
}

// The full action label repeats the card's own name ("Change position X"),
// which is redundant once that specific card's info is already showing.
std::wstring short_action_label(const std::wstring& text) {
    static const std::vector<std::pair<std::wstring, std::wstring>> verbs = {
        {L"Change position ", L"Change Position"}, {L"Special summon ", L"Special Summon"},
        {L"Summon ", L"Summon"}, {L"Set monster ", L"Set (Defense)"},
        {L"Set spell/trap ", L"Set"}, {L"Activate ", L"Activate"},
    };
    for (const auto& [prefix, label] : verbs) if (text.rfind(prefix, 0) == 0) return label;
    return text;
}

struct InspectorActionLayout { Rect area; std::vector<Rect> buttons; };

// Cuts (and shrinks `inner`) a fixed band of up to 2-per-row buttons off the
// bottom, sized purely from `count` so paint and hit-testing always agree.
InspectorActionLayout compute_inspector_action_buttons(Rect& inner, size_t count, const UiScale& scale) {
    InspectorActionLayout out;
    if (count == 0) return out;
    constexpr size_t kPerRow = 2;
    const size_t rows = (count + kPerRow - 1) / kPerRow;
    const int rowHeight = scale.px(30);
    const int gap = scale.px(6);
    const int totalHeight = static_cast<int>(rows) * rowHeight + static_cast<int>(rows - 1) * gap;
    out.area = cut_bottom(inner, totalHeight);
    cut_bottom(inner, scale.px(8));
    for (size_t r = 0; r < rows; ++r) {
        const size_t colsThisRow = std::min(kPerRow, count - r * kPerRow);
        const int colWidth = (out.area.width() - gap * static_cast<int>(colsThisRow - 1)) / static_cast<int>(colsThisRow);
        for (size_t c = 0; c < colsThisRow; ++c) {
            const int x = out.area.left + static_cast<int>(c) * (colWidth + gap);
            const int y = out.area.top + static_cast<int>(r) * (rowHeight + gap);
            out.buttons.push_back({x, y, x + colWidth, y + rowHeight});
        }
    }
    return out;
}

// ---------- engine IPC (unchanged protocol; only richer parsing) ----------

// A fresh random seed per duel so the shuffle (and therefore every opening
// hand and topdeck) differs game to game — the engine otherwise defaults to
// a fixed seed, which made every duel replay an identical deck order.
uint64_t random_duel_seed() {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
}

std::wstring run_automatic_duel() {
    const auto root = fs::current_path();
    const auto executable = root / "build" / "goat-sim.exe";
    if (!fs::exists(executable)) return L"Engine executable is missing. Run scripts/build-smoke.sh first.";
    SECURITY_ATTRIBUTES attributes{}; attributes.nLength = sizeof(attributes); attributes.bInheritHandle = TRUE;
    HANDLE read_pipe = nullptr, write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &attributes, 0)) return L"Could not create the duel output channel.";
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    std::wstring command = L"\"" + executable.wstring() + L"\" duel decks/vanilla-a.ydk decks/vanilla-b.ydk --seed " +
        std::to_wstring(random_duel_seed()) + L" --max-turns 100 --quiet";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_pipe; startup.hStdError = write_pipe; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                         root.wstring().c_str(), &startup, &process)) {
        CloseHandle(read_pipe); CloseHandle(write_pipe); return L"Could not start the Project Ignis duel process.";
    }
    CloseHandle(write_pipe);
    WaitForSingleObject(process.hProcess, INFINITE);
    std::string output; std::array<char, 512> chunk{}; DWORD received = 0;
    while (ReadFile(read_pipe, chunk.data(), static_cast<DWORD>(chunk.size()), &received, nullptr) && received)
        output.append(chunk.data(), received);
    DWORD exit_code = 1; GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(read_pipe); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    if (exit_code != 0) return L"Automatic duel failed:\n" + utf8_to_wide(output.substr(output.rfind('\n') + 1));
    const auto winner = output.rfind("wins (reason");
    if (winner == std::string::npos) return L"Automatic duel finished without a final result message.";
    const auto line_start = output.rfind('\n', winner);
    const auto line_end = output.find('\n', winner);
    return L"Automatic duel complete — " + utf8_to_wide(output.substr(line_start == std::string::npos ? 0 : line_start + 1, line_end - line_start - 1));
}

// test_mode duels skip the profile's card-ownership check and the GOAT
// banlist (both decks, via --allow-illegal-deck) and never grant rewards on
// completion — a sandbox for trying out a deck file before it's "earned".
void start_player_duel(AppState& state, bool test_mode) {
    if (state.player_process.hProcess) return;
    const auto& npc = state.catalog.npcs.at(state.selected_npc % state.catalog.npcs.size());
    if (!test_mode) {
        try {
            goat::CardDatabase database("external/BabelCDB/cards.cdb", "external/BabelCDB/goat-entries.cdb");
            const auto banlist = goat::Banlist::load("external/LFLists/GOAT.lflist.conf");
            goat::game::validate_player_deck(goat::game::read_deck(state.progression.profile().selected_deck), state.progression.profile(), database, banlist);
            goat::game::validate_npc_deck(goat::game::read_deck(npc.deck_path), database, banlist);
        } catch (const std::exception& error) {
            state.status = L"Deck cannot be used: " + utf8_to_wide(error.what()); return;
        }
    }
    std::error_code ignored;
    fs::remove_all(state.session_directory, ignored);
    fs::create_directories(state.session_directory);
    const auto root = fs::current_path();
    const auto executable = root / "build" / "goat-sim.exe";
    if (!fs::exists(executable)) { state.status = L"Duel engine is missing: " + executable.wstring(); return; }
    std::wstring command = L"\"" + executable.wstring() + L"\" duel \"" +
        fs::path(state.progression.profile().selected_deck).wstring() + L"\" \"" + fs::path(npc.deck_path).wstring() +
        L"\" --human-player 1 --decision-dir \"" + state.session_directory.wstring() + L"\" --result-file \"" +
        (state.session_directory / "result.txt").wstring() + L"\" --seed " + std::to_wstring(random_duel_seed()) + L" --quiet";
    if (test_mode) command += L" --allow-illegal-deck";
    // The engine reports its actual failure reason (an uncaught exception's
    // .what(), e.g. "empty option-selection prompt" or "turn limit reached")
    // to stderr before exiting — see main()'s outer catch in src/main.cpp.
    // Without capturing that, an unexpected exit only ever showed the client
    // its own generic "ended before reporting a result", with the real cause
    // gone forever (this is a GUI process with no console to see it on).
    // Redirecting to a file here means poll_player_duel can surface it.
    SECURITY_ATTRIBUTES inheritable{}; inheritable.nLength = sizeof(inheritable); inheritable.bInheritHandle = TRUE;
    const auto logPath = state.session_directory / "engine-log.txt";
    HANDLE logHandle = CreateFileW(logPath.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inheritable,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    if (logHandle != INVALID_HANDLE_VALUE) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = logHandle; startup.hStdError = logHandle; startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, logHandle != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW, nullptr,
                                         root.wstring().c_str(), &startup, &state.player_process);
    if (logHandle != INVALID_HANDLE_VALUE) CloseHandle(logHandle);
    if (!created) {
        state.player_process = {}; state.status = L"Could not start the player-vs-CPU duel."; return;
    }
    state.duel_is_test_mode = test_mode;
    state.life = {8000, 8000}; state.life_display = {8000.0, 8000.0};
    state.hand_count = {}; state.deck_count = {}; state.grave_count = {}; state.extra_count = {}; state.banished_count = {};
    state.monsters = {}; state.spells = {}; state.hand_cards.clear();
    state.turn_player = 0; state.turn_number = 0; state.phase = 0;
    state.legal_actions.clear(); state.action_layout = ActionLayout{}; state.action_page = 0; state.prompt_title.clear();
    state.open_hand_card = -1; state.selected_monster_zone = -1; state.selected_spell_zone = -1;
    state.last_board_snapshot.clear();
    state.last_request_write_time = fs::file_time_type{};
    state.status = (test_mode ? L"Test duel started against " : L"Duel started against ") + utf8_to_wide(npc.name) +
        (test_mode ? L" — no rewards, restrictions bypassed." : L". Waiting for the engine's legal actions.");
}

void poll_player_duel(AppState& state) {
    if (!state.player_process.hProcess) return;
    const auto request = state.session_directory / "request.txt";
    // The engine (see choose_menu in src/main.cpp) writes request.txt once
    // per decision and only deletes/replaces it once it has consumed our
    // response.txt for that decision — a step that happens on its own
    // 100ms poll, not synchronously with our submit. Without the write-time
    // check below, a client poll landing in that gap would see the *same*
    // already-answered request.txt still on disk (legal_actions was already
    // cleared by submit_action) and re-ingest it as if it were fresh. The
    // player would then be looking at a stale, already-resolved prompt while
    // the engine is really waiting on a brand-new one; clicking anything in
    // it sends a response.txt that gets misapplied to that new prompt
    // instead — which is exactly what "clicking confirm summons a random
    // card, and eventually the duel just ends" looks like. Tracking the
    // file's last-write-time (updated only by the engine's atomic
    // rename-in-place) lets us tell "still the prompt we already answered"
    // apart from "a genuinely new prompt", even when the two happen to have
    // identical text.
    if (fs::exists(request) && state.legal_actions.empty()) {
        std::error_code timeError;
        const auto writeTime = fs::last_write_time(request, timeError);
        if (!timeError && writeTime != state.last_request_write_time) {
            state.last_request_write_time = writeTime;
            std::ifstream input(request);
            std::string line;
            std::getline(input, line);
            state.prompt_title = utf8_to_wide(line);
            state.legal_actions.clear();
            while (std::getline(input, line)) {
                const auto split = line.find('|');
                if (split == std::string::npos) continue;
                const auto raw = utf8_to_wide(line.substr(split + 1));
                const auto begin = raw.find(L'['), end = raw.rfind(L']');
                LegalAction action;
                if (begin != std::wstring::npos && end != std::wstring::npos && begin < end) {
                    action.image = fs::path(raw.substr(begin + 1, end - begin - 1));
                    action.code = parse_code_from_image_path(action.image);
                }
                action.label = action_caption(raw);
                state.legal_actions.push_back(std::move(action));
            }
            state.action_page = 0;
        }
    }
    const auto result = state.session_directory / "result.txt";
    if (fs::exists(result)) {
        std::ifstream input(result); std::string winner; std::getline(input, winner);
        const bool player_won = winner == "winner=0";
        const auto& npc = state.catalog.npcs.at(state.selected_npc % state.catalog.npcs.size());
        if (state.duel_is_test_mode) {
            state.status = player_won ? L"Test duel complete — Victory! (no rewards, test mode)" : L"Test duel complete — Defeat. (no penalty, test mode)";
        } else if (player_won) {
            state.progression.award_npc_victory(npc);
            goat::game::ProfileStore::save(state.progression.profile(), "saves/default.sav");
            state.status = L"Victory! Earned " + std::to_wstring(npc.reward.credits) + L" credits and a sealed pack.";
        } else state.status = L"Defeat. Try a different line or strengthen your collection in the shop.";
        CloseHandle(state.player_process.hThread); CloseHandle(state.player_process.hProcess); state.player_process = {};
        state.legal_actions.clear(); state.action_layout = ActionLayout{}; state.prompt_title.clear();
        state.open_hand_card = -1; state.selected_monster_zone = -1; state.selected_spell_zone = -1; return;
    }
    DWORD code = STILL_ACTIVE; GetExitCodeProcess(state.player_process.hProcess, &code);
    if (code != STILL_ACTIVE) {
        state.status = L"The duel process ended before reporting a result.";
        // Surface whatever the engine actually reported (its uncaught
        // exception's .what(), captured to engine-log.txt — see
        // start_player_duel) instead of leaving the player with only the
        // generic message above and no way to tell what went wrong.
        std::ifstream log(state.session_directory / "engine-log.txt");
        if (log) {
            std::string logText{std::istreambuf_iterator<char>(log), {}};
            while (!logText.empty() && (logText.back() == '\n' || logText.back() == '\r')) logText.pop_back();
            const auto lastLine = logText.find_last_of("\r\n");
            const auto reason = lastLine == std::string::npos ? logText : logText.substr(lastLine + 1);
            if (!reason.empty()) state.status += L" (" + utf8_to_wide(reason) + L")";
        }
        CloseHandle(state.player_process.hThread); CloseHandle(state.player_process.hProcess); state.player_process = {};
        state.legal_actions.clear(); state.action_layout = ActionLayout{}; state.prompt_title.clear();
        state.open_hand_card = -1; state.selected_monster_zone = -1; state.selected_spell_zone = -1;
    }
}

void poll_board_snapshot(AppState& state) {
    const auto filename = state.session_directory / "state.txt";
    std::ifstream input(filename); if (!input) return;
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    if (text.empty() || text == state.last_board_snapshot) return;
    state.last_board_snapshot = text;
    for (auto& player : state.monsters) for (auto& cell : player) cell = {};
    for (auto& player : state.spells) for (auto& cell : player) cell = {};
    state.hand_cards.clear();
    std::istringstream lines(text); std::string line;
    while (std::getline(lines, line)) {
        const auto equals = line.find('='); if (equals == std::string::npos) continue;
        const auto key = line.substr(0, equals), value = line.substr(equals + 1);
        std::vector<uint32_t> fields; size_t cursor = 0;
        while (cursor < value.size()) {
            const auto comma = value.find(',', cursor);
            fields.push_back(static_cast<uint32_t>(std::stoul(value.substr(cursor, comma - cursor))));
            if (comma == std::string::npos) break;
            cursor = comma + 1;
        }
        if (key == "lp" && fields.size() == 2) { state.life[0] = static_cast<int>(fields[0]); state.life[1] = static_cast<int>(fields[1]); }
        else if (key == "hand" && fields.size() == 2) { state.hand_count[0] = fields[0]; state.hand_count[1] = fields[1]; }
        else if (key == "deck" && fields.size() == 2) { state.deck_count[0] = fields[0]; state.deck_count[1] = fields[1]; }
        else if (key == "grave" && fields.size() == 2) { state.grave_count[0] = fields[0]; state.grave_count[1] = fields[1]; }
        else if (key == "extra" && fields.size() == 2) { state.extra_count[0] = fields[0]; state.extra_count[1] = fields[1]; }
        else if (key == "banished" && fields.size() == 2) { state.banished_count[0] = fields[0]; state.banished_count[1] = fields[1]; }
        else if (key == "turn" && fields.size() == 3) { state.turn_player = static_cast<uint8_t>(fields[0]); state.turn_number = fields[1]; state.phase = static_cast<uint16_t>(fields[2]); }
        else if (key == "monster" && fields.size() == 4 && fields[0] < 2 && fields[1] < 5) state.monsters[fields[0]][fields[1]] = {true, fields[2], static_cast<uint8_t>(fields[3])};
        else if (key == "spell" && fields.size() == 4 && fields[0] < 2 && fields[1] < 6) state.spells[fields[0]][fields[1]] = {true, fields[2], static_cast<uint8_t>(fields[3])};
        else if (key == "handcards" && !fields.empty() && fields[0] < 2) state.hand_cards.assign(fields.begin() + 1, fields.end());
    }
}

void submit_action(AppState& state, size_t action) {
    if (action >= state.legal_actions.size()) return;
    state.last_submit_tick = GetTickCount();
    if (state.legal_actions[action].code) state.last_inspected_code = state.legal_actions[action].code;
    // The engine (choose_menu in src/main.cpp) polls for this file's
    // existence every 100ms and reads it the instant fs::exists() sees it.
    // Writing directly to response.txt left a window where the engine could
    // observe the file mid-write (created but not yet flushed) and read it
    // empty; `in >> choice` then fails silently and the engine falls back to
    // its default `choice = choices.size()`, which is always out of range —
    // guaranteed "invalid graphical action index", killing the duel. This is
    // confirmed reproducible: a plain non-atomic write of "0" from an
    // external driver hit it directly. Writing to a temp file and renaming
    // (matching how the engine itself publishes request.txt) makes the
    // response only ever appear on disk fully formed.
    const auto responsePath = state.session_directory / "response.txt";
    const auto tempPath = state.session_directory / "response.tmp";
    { std::ofstream output(tempPath, std::ios::trunc); output << action << '\n'; }
    std::error_code renameError;
    fs::rename(tempPath, responsePath, renameError);
    state.legal_actions.clear();
    state.action_layout = ActionLayout{};
    state.action_page = 0;
    state.prompt_title.clear();
    state.open_hand_card = -1; state.selected_monster_zone = -1; state.selected_spell_zone = -1;
}

// ---------- drawing primitives ----------

void draw_text(HDC dc, const RECT& bounds, const wchar_t* text, int points, COLORREF color, UINT format) {
    HFONT font = CreateFontW(-points, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    const auto old_font = SelectObject(dc, font);
    SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text, -1, const_cast<RECT*>(&bounds), format);
    SelectObject(dc, old_font); DeleteObject(font);
}

void draw_panel(HDC dc, const Rect& r, COLORREF fill, bool has_border, COLORREF border) {
    if (r.empty()) return;
    RECT rc = r.win32();
    HBRUSH brush = CreateSolidBrush(fill); FillRect(dc, &rc, brush); DeleteObject(brush);
    if (has_border) {
        HPEN pen = CreatePen(PS_SOLID, 1, border);
        auto oldPen = SelectObject(dc, pen); auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
    }
}

void draw_button(HDC dc, const Rect& box, const wchar_t* title, const wchar_t* subtitle, const UiScale& scale, bool hovered) {
    draw_panel(dc, box, hovered ? RGB(45, 84, 112) : theme::panel, true, hovered ? theme::gold : theme::panelBorder);
    Rect inner = inset(box, scale.px(10), scale.px(6));
    const bool hasSubtitle = subtitle && *subtitle;
    Rect titleRect = hasSubtitle ? cut_top(inner, scale.px(22)) : inner;
    draw_text(dc, titleRect.win32(), title, scale.points(hasSubtitle ? 14 : 13), theme::gold,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (hasSubtitle) draw_text(dc, inner.win32(), subtitle, scale.points(11), theme::textSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK);
}

void draw_bitmap(HDC dc, HBITMAP bitmap, const RECT& destination) {
    if (!bitmap) return;
    HDC memory = CreateCompatibleDC(dc); const auto old = SelectObject(memory, bitmap); BITMAP source{}; GetObject(bitmap, sizeof(source), &source);
    SetStretchBltMode(dc, HALFTONE);
    StretchBlt(dc, destination.left, destination.top, destination.right - destination.left, destination.bottom - destination.top,
               memory, 0, 0, source.bmWidth, source.bmHeight, SRCCOPY);
    SelectObject(memory, old); DeleteDC(memory);
}

// Letterboxes `bitmap` into `destination`, preserving the bitmap's own aspect
// ratio instead of stretching it to the destination's — plain draw_bitmap
// visibly distorts art whose native aspect doesn't match the caller's rect
// (e.g. pack-art images are ~0.55 wide/tall, not the ~0.686 card aspect most
// rects assume). `background`, if non-null, fills the full destination first
// so the letterbox bars match the surrounding panel instead of showing
// whatever was drawn underneath.
void draw_bitmap_fit(HDC dc, HBITMAP bitmap, const Rect& destination, const COLORREF* background = nullptr) {
    if (!bitmap) return;
    BITMAP source{}; GetObject(bitmap, sizeof(source), &source);
    if (source.bmWidth <= 0 || source.bmHeight <= 0) return;
    if (background) { HBRUSH brush = CreateSolidBrush(*background); RECT full = destination.win32(); FillRect(dc, &full, brush); DeleteObject(brush); }
    const double sourceAspect = static_cast<double>(source.bmWidth) / source.bmHeight;
    const double destAspect = static_cast<double>(destination.width()) / std::max(1, destination.height());
    Rect fitted = destination;
    if (sourceAspect > destAspect) {
        const int height = static_cast<int>(destination.width() / sourceAspect);
        fitted.top = destination.top + (destination.height() - height) / 2;
        fitted.bottom = fitted.top + height;
    } else {
        const int width = static_cast<int>(destination.height() * sourceAspect);
        fitted.left = destination.left + (destination.width() - width) / 2;
        fitted.right = fitted.left + width;
    }
    draw_bitmap(dc, bitmap, fitted.win32());
}

// Fills `destination` edge-to-edge with `bitmap`, cropping (never
// letterboxing) whichever axis overflows — the usual "background cover" fit
// for a full-bleed backdrop image, as opposed to draw_bitmap_fit's
// letterboxed "contain" fit used for card/pack art tiles.
void draw_bitmap_cover(HDC dc, HBITMAP bitmap, const Rect& destination) {
    if (!bitmap) return;
    HDC memory = CreateCompatibleDC(dc); const auto old = SelectObject(memory, bitmap); BITMAP source{}; GetObject(bitmap, sizeof(source), &source);
    if (source.bmWidth <= 0 || source.bmHeight <= 0) { SelectObject(memory, old); DeleteDC(memory); return; }
    SetStretchBltMode(dc, HALFTONE);
    const double sourceAspect = static_cast<double>(source.bmWidth) / source.bmHeight;
    const double destAspect = static_cast<double>(destination.width()) / std::max(1, destination.height());
    int srcX = 0, srcY = 0, srcW = source.bmWidth, srcH = source.bmHeight;
    if (sourceAspect > destAspect) {
        srcW = std::max(1, static_cast<int>(source.bmHeight * destAspect));
        srcX = (source.bmWidth - srcW) / 2;
    } else {
        srcH = std::max(1, static_cast<int>(source.bmWidth / destAspect));
        srcY = (source.bmHeight - srcH) / 2;
    }
    StretchBlt(dc, destination.left, destination.top, destination.width(), destination.height(), memory, srcX, srcY, srcW, srcH, SRCCOPY);
    SelectObject(memory, old); DeleteDC(memory);
}

// 90-degree clockwise rotation for defense-position monsters: source
// (0,0)->dest top-right, (w,0)->dest bottom-right, (0,h)->dest top-left.
void draw_bitmap_rotated(HDC dc, HBITMAP bitmap, const Rect& footprint) {
    if (!bitmap) return;
    HDC memory = CreateCompatibleDC(dc); const auto old = SelectObject(memory, bitmap); BITMAP source{}; GetObject(bitmap, sizeof(source), &source);
    SetStretchBltMode(dc, HALFTONE);
    POINT pts[3] = {{footprint.right, footprint.top}, {footprint.right, footprint.bottom}, {footprint.left, footprint.top}};
    PlgBlt(dc, pts, memory, 0, 0, source.bmWidth, source.bmHeight, nullptr, 0, 0);
    SelectObject(memory, old); DeleteDC(memory);
}

void draw_rect_outline(HDC dc, const Rect& r, int width, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    auto oldPen = SelectObject(dc, pen); auto oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
}

// Renders one field zone: empty outline, face-down back (rotated for defense
// monsters), face-up art, or a graceful "image unavailable" fallback so a
// missing JPEG never leaves undefined texture memory on screen.
void draw_card_slot(HDC dc, AppState& state, const Rect& cell, const FieldCard& card, bool is_monster_zone,
                     const wchar_t* empty_label, const UiScale& scale, bool hovered, bool legal_target) {
    draw_panel(dc, cell, theme::fieldZone, false, 0);
    const COLORREF outline = legal_target ? theme::legal : (hovered ? theme::gold : theme::fieldZoneLine);
    draw_rect_outline(dc, cell, legal_target || hovered ? 2 : 1, outline);

    if (!card.occupied) {
        if (empty_label && *empty_label) {
            // Word-wrap rather than single-line: the narrow deck/grave/field-spell
            // side columns can be too tight for a label at small window sizes,
            // and a clipped single line reads worse than a wrapped one.
            RECT labelRect{cell.left, cell.bottom - scale.px(30), cell.right, cell.bottom};
            draw_text(dc, labelRect, empty_label, scale.points(9), theme::textSecondary, DT_CENTER | DT_BOTTOM | DT_WORDBREAK);
        }
        return;
    }

    const bool faceDown = (card.position & POS_FACEDOWN) != 0;
    const bool defense = is_monster_zone && (card.position & POS_DEFENSE) != 0;
    const Rect footprint = defense ? defense_footprint(cell) : fit_card_in_cell(cell);

    if (faceDown) {
        // Always render a face-down card as a card back on the board — even
        // your own — matching a real table. The engine only redacts the
        // opponent's code (never our own), so hovering/clicking still reveals
        // the true identity in the inspector for our own cards specifically;
        // see compute_duel_hover. The board art itself never leaks it.
        if (state.card_back_texture) {
            if (defense) draw_bitmap_rotated(dc, state.card_back_texture, footprint);
            else draw_bitmap(dc, state.card_back_texture, footprint.win32());
        } else {
            draw_panel(dc, footprint, theme::cardBack, false, 0);
        }
        draw_rect_outline(dc, footprint, 1, theme::gold);
        if (hovered) draw_rect_outline(dc, inset(footprint, -2), 2, theme::gold);
        return;
    }

    HBITMAP texture = get_card_texture(state, card.code);
    if (texture) {
        if (defense) draw_bitmap_rotated(dc, texture, footprint);
        else draw_bitmap(dc, texture, footprint.win32());
    } else {
        draw_panel(dc, footprint, RGB(30, 30, 38), false, 0);
        const auto name = utf8_to_wide(state.card_database->resolve(card.code).name);
        draw_text(dc, footprint.win32(), (name + L"\n(image unavailable)").c_str(), scale.points(9), theme::textSecondary, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    }
    if (hovered) draw_rect_outline(dc, inset(footprint, -2), 2, theme::gold);
}

void draw_pile_zone(HDC dc, const Rect& cell, const wchar_t* label, uint32_t count, const UiScale& scale) {
    draw_panel(dc, cell, theme::panel, true, theme::panelBorder);
    RECT labelRect{cell.left, cell.top + scale.px(2), cell.right, cell.top + cell.height() / 2};
    draw_text(dc, labelRect, label, scale.points(8), theme::textSecondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT countRect{cell.left, cell.top + cell.height() / 2, cell.right, cell.bottom - scale.px(2)};
    draw_text(dc, countRect, std::to_wstring(count).c_str(), scale.points(13), theme::textPrimary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void draw_hud_bar(HDC dc, const Rect& r, AppState& state, bool opponent, const UiScale& scale) {
    draw_panel(dc, r, theme::panel, true, theme::panelBorder);
    Rect inner = inset(r, scale.px(12), scale.px(4));
    Rect nameRect = cut_left(inner, inner.width() / 2);
    const std::wstring name = opponent ? utf8_to_wide(state.catalog.npcs.at(state.selected_npc % state.catalog.npcs.size()).name) : L"YOU";
    draw_text(dc, nameRect.win32(), name.c_str(), scale.points(15), theme::gold, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    const size_t idx = opponent ? 1u : 0u;
    const int lp = static_cast<int>(std::lround(state.life_display[idx]));
    std::wstring info = L"LP " + std::to_wstring(lp) + L"   Hand " + std::to_wstring(opponent ? state.hand_count[1] : state.hand_cards.size()) +
        L"   Deck " + std::to_wstring(state.deck_count[idx]) + L"   GY " + std::to_wstring(state.grave_count[idx]);
    if (state.banished_count[idx]) info += L"   Banished " + std::to_wstring(state.banished_count[idx]);
    draw_text(dc, inner.win32(), info.c_str(), scale.points(13), lp <= 1000 ? theme::danger : theme::textPrimary, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}

void draw_phase_button(HDC dc, const Rect& box, const wchar_t* title, const UiScale& scale, bool enabled, bool hovered) {
    const COLORREF fill = !enabled ? RGB(16, 24, 32) : (hovered ? RGB(45, 84, 112) : theme::panel);
    const COLORREF border = !enabled ? theme::panelBorder : (hovered ? theme::gold : theme::legal);
    draw_panel(dc, box, fill, true, border);
    draw_text(dc, box.win32(), title, scale.points(11), enabled ? theme::gold : theme::textSecondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void draw_turn_indicator(HDC dc, const Rect& r, AppState& state, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, r, RGB(14, 22, 30), true, theme::panelBorder);
    const auto pb = compute_phase_bar_layout(r, scale);
    std::wstring text = L"TURN " + std::to_wstring(state.turn_number) + L" • " + (state.turn_player == 0 ? L"YOUR TURN" : L"OPPONENT'S TURN");
    const auto phase = phase_name(state.phase);
    if (*phase) text += std::wstring(L"  —  ") + phase;
    draw_text(dc, pb.label.win32(), text.c_str(), scale.points(12), theme::textPrimary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_phase_button(dc, pb.battle, L"Battle Phase", scale, state.action_layout.battle_phase_action >= 0, pb.battle.contains(mouseX, mouseY));
    draw_phase_button(dc, pb.main2, L"Main Phase 2", scale, state.action_layout.main_phase2_action >= 0, pb.main2.contains(mouseX, mouseY));
    draw_phase_button(dc, pb.end, L"End Phase", scale, state.action_layout.end_phase_action >= 0, pb.end.contains(mouseX, mouseY));
}

void draw_hand_row(HDC dc, AppState& state, const Rect& area, bool own, const DuelHover& hover, const UiScale& scale) {
    const size_t count = own ? state.hand_cards.size() : static_cast<size_t>(state.hand_count[1]);
    const auto rects = layout_hand(area, count);
    if (rects.empty()) {
        if (own) draw_text(dc, area.win32(), L"Hand empty", scale.points(10), theme::textSecondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    }
    const int hoveredIdx = own ? hover.hand_index : -1;
    auto drawOne = [&](size_t i, bool isHovered) {
        const bool isOpen = own && state.open_hand_card == static_cast<int>(i);
        const bool hasActions = own && state.action_layout.hand_actions.count(state.hand_cards[i]) > 0;
        Rect r = rects[i];
        if (isHovered || isOpen) r.top -= scale.px(10);
        if (own) {
            HBITMAP texture = get_card_texture(state, state.hand_cards[i]);
            if (texture) draw_bitmap(dc, texture, r.win32());
            else draw_panel(dc, r, RGB(30, 30, 38), true, theme::panelBorder);
        } else if (state.card_back_texture) {
            draw_bitmap(dc, state.card_back_texture, r.win32());
            draw_rect_outline(dc, r, 1, theme::gold);
        } else {
            draw_panel(dc, r, theme::cardBack, true, theme::gold);
        }
        // Legal (green) = has clickable actions right now; gold = actively
        // hovered/open — a stronger cue always wins over a weaker one.
        if (isOpen || isHovered) draw_rect_outline(dc, inset(r, -2), 2, theme::gold);
        else if (hasActions) draw_rect_outline(dc, inset(r, -2), 2, theme::legal);
    };
    const int openIdx = own ? state.open_hand_card : -1;
    for (size_t i = 0; i < rects.size(); ++i) if (static_cast<int>(i) != hoveredIdx && static_cast<int>(i) != openIdx) drawOne(i, false);
    if (hoveredIdx >= 0 && static_cast<size_t>(hoveredIdx) < rects.size() && hoveredIdx != openIdx) drawOne(static_cast<size_t>(hoveredIdx), true);
    if (openIdx >= 0 && static_cast<size_t>(openIdx) < rects.size()) drawOne(static_cast<size_t>(openIdx), hoveredIdx == openIdx);
}

void draw_inspector(HDC dc, AppState& state, const Rect& panel, const DuelHover& hover, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, panel, theme::panel, true, theme::panelBorder);
    Rect inner = inset(panel, scale.px(12));

    // An explicit board-card selection (a click, not just a hover) always
    // wins the inspector and gets its own action buttons at the bottom.
    const BoardSelection selection = resolve_board_selection(state);
    const bool hasSelectionActions = selection.actions && !selection.actions->empty();
    InspectorActionLayout actionLayout;
    if (hasSelectionActions) actionLayout = compute_inspector_action_buttons(inner, selection.actions->size(), scale);

    const uint32_t code = selection.code ? selection.code : (hover.inspect_code ? hover.inspect_code : state.last_inspected_code);
    const bool showBack = !selection.code && hover.inspect_code == 0 && hover.inspect_is_back;

    Rect artArea = cut_top(inner, static_cast<int>(inner.width() / kCardAspect));
    if (code != 0 && !showBack) {
        HBITMAP texture = get_card_texture(state, code);
        if (texture) draw_bitmap(dc, texture, artArea.win32());
        else { draw_panel(dc, artArea, RGB(24, 24, 30), true, theme::panelBorder); draw_text(dc, artArea.win32(), L"CARD IMAGE\nUNAVAILABLE", scale.points(11), theme::textSecondary, DT_CENTER | DT_VCENTER | DT_WORDBREAK); }
    } else {
        draw_panel(dc, artArea, RGB(24, 24, 30), true, theme::panelBorder);
        draw_text(dc, artArea.win32(), showBack ? L"FACE-DOWN CARD" : L"Hover a card\nto inspect it", scale.points(11), theme::textSecondary, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    }
    cut_top(inner, scale.px(10));
    if (code == 0 || showBack) {
        draw_text(dc, inner.win32(), L"Tip: monster-zone and attack choices during your turn can be clicked directly on the board. Everything else appears in the panel below the field.", scale.points(10), theme::textSecondary, DT_LEFT | DT_WORDBREAK);
        return;
    }
    const auto& def = state.card_database->resolve(code);
    Rect nameRect = cut_top(inner, scale.px(24));
    draw_text(dc, nameRect.win32(), utf8_to_wide(def.name).c_str(), scale.points(15), theme::gold, DT_LEFT | DT_VCENTER | DT_WORDBREAK);
    Rect statRect = cut_top(inner, scale.px(20));
    draw_text(dc, statRect.win32(), describe_card_stats(def).c_str(), scale.points(11), theme::textPrimary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Rect typeRect = cut_top(inner, scale.px(34));
    draw_text(dc, typeRect.win32(), describe_card_type(def).c_str(), scale.points(10), theme::textSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK);
    if (!def.text.empty()) {
        cut_top(inner, scale.px(6));
        draw_text(dc, inner.win32(), utf8_to_wide(def.text).c_str(), scale.points(10), theme::textPrimary, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    if (hasSelectionActions) {
        for (size_t i = 0; i < actionLayout.buttons.size(); ++i) {
            const auto& label = state.legal_actions[(*selection.actions)[i]].label;
            draw_button(dc, actionLayout.buttons[i], short_action_label(label).c_str(), nullptr, scale, actionLayout.buttons[i].contains(mouseX, mouseY));
        }
    }
}

void draw_prompt_panel(HDC dc, AppState& state, const Rect& panelRect, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, panelRect, theme::panel, true, theme::panelBorder);
    if (!state.player_process.hProcess && state.legal_actions.empty()) {
        Rect inner = inset(panelRect, scale.px(10));
        draw_text(dc, inner.win32(), L"Duel finished. Click anywhere to return to the campaign hub.", scale.points(13), theme::textPrimary, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        return;
    }
    if (state.legal_actions.empty()) {
        Rect inner = inset(panelRect, scale.px(10));
        draw_text(dc, inner.win32(), L"Waiting for the rules engine…", scale.points(13), theme::textSecondary, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
        return;
    }

    const auto pl = compute_prompt_layout(panelRect, state.action_layout.panel_indices.size(), scale);
    std::wstring title = state.prompt_title;
    if (state.action_layout.all_zone_placement) title += state.action_layout.zone_placement_is_spell ? L" — click a highlighted Spell/Trap Zone" : L" — click a highlighted Monster Zone";
    else if (state.action_layout.has_attacks) title += L" — click a monster on the field to attack";
    else if (!state.action_layout.hand_actions.empty()) title += L" — also check the highlighted cards in your hand";
    draw_text(dc, pl.title.win32(), title.c_str(), scale.points(12), theme::gold, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (state.action_layout.all_zone_placement) return;

    for (size_t i = 0; i < pl.rows.size(); ++i) {
        const size_t globalIndex = state.action_page * kPromptRowsPerPage + i;
        if (globalIndex >= state.action_layout.panel_indices.size()) break;
        const auto& action = state.legal_actions[state.action_layout.panel_indices[globalIndex]];
        draw_button(dc, pl.rows[i], action.label.c_str(), nullptr, scale, pl.rows[i].contains(mouseX, mouseY));
    }
    if (pl.has_pager) {
        draw_button(dc, pl.prev_button, L"‹ Prev", nullptr, scale, state.action_page > 0 && pl.prev_button.contains(mouseX, mouseY));
        draw_button(dc, pl.next_button, L"Next ›", nullptr, scale, state.action_page + 1 < pl.page_count && pl.next_button.contains(mouseX, mouseY));
        const std::wstring pageText = std::to_wstring(state.action_page + 1) + L" / " + std::to_wstring(pl.page_count);
        draw_text(dc, pl.pager_label.win32(), pageText.c_str(), scale.points(11), theme::textSecondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

// Drawn last (top of the z-order) so its buttons are never covered by
// anything else on the board.
void draw_hand_popup(HDC dc, AppState& state, const DuelLayout& L, const UiScale& scale, int mouseX, int mouseY) {
    if (state.open_hand_card < 0 || static_cast<size_t>(state.open_hand_card) >= state.hand_cards.size()) return;
    const uint32_t code = state.hand_cards[static_cast<size_t>(state.open_hand_card)];
    const auto it = state.action_layout.hand_actions.find(code);
    if (it == state.action_layout.hand_actions.end() || it->second.empty()) return;
    const auto handRects = layout_hand(L.player_hand, state.hand_cards.size());
    if (static_cast<size_t>(state.open_hand_card) >= handRects.size()) return;

    const auto popup = compute_hand_popup_layout(handRects[static_cast<size_t>(state.open_hand_card)], it->second.size(), scale, L.opponent_hud);
    draw_panel(dc, popup.panel, theme::panel, true, theme::gold);
    for (size_t i = 0; i < popup.buttons.size(); ++i) {
        const auto& action = state.legal_actions[it->second[i]];
        draw_button(dc, popup.buttons[i], hand_action_short_label(action.label).c_str(), nullptr, scale, popup.buttons[i].contains(mouseX, mouseY));
    }
}

void draw_app_header(HDC dc, const Rect& client, const UiScale& scale) {
    Rect area = inset(client, scale.px(28));
    Rect header = cut_top(area, scale.px(50));
    draw_text(dc, header.win32(), L"PROJECT GOAT", scale.points(26), theme::gold, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Rect sub = cut_top(area, scale.px(26));
    draw_text(dc, sub.win32(), L"Project Ignis rules • 2005 GOAT Format", scale.points(12), theme::textSecondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

// `flatten_over`, if given, flattens any transparency onto that solid color
// at load time rather than leaving it for a (nonexistent) alpha-blended
// draw: everything in this client draws bitmaps with plain StretchBlt/BitBlt,
// which ignores the alpha channel entirely, so a PNG with real transparency
// would otherwise show its raw (typically near-black, since WIC decodes to
// *premultiplied* BGRA — RGB is scaled by alpha, so alpha=0 pixels store
// RGB=0 regardless of their original color) channel data instead of the
// panel color behind it.
HBITMAP load_jpeg(const fs::path& file, const COLORREF* flatten_over) {
    if (!fs::exists(file)) return nullptr;
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    HBITMAP result = nullptr;
    UINT width = 0, height = 0;
    BITMAPINFO info{};
    void* pixels = nullptr;
    HDC screen = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) goto done;
    if (FAILED(factory->CreateDecoderFromFilename(file.wstring().c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder))) goto done;
    if (FAILED(decoder->GetFrame(0, &frame))) goto done;
    if (FAILED(factory->CreateFormatConverter(&converter))) goto done;
    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) goto done;
    converter->GetSize(&width, &height);
    if (width == 0 || height == 0) goto done;
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    screen = GetDC(nullptr);
    result = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!result || FAILED(converter->CopyPixels(nullptr, width * 4, width * height * 4, static_cast<BYTE*>(pixels)))) {
        if (result) DeleteObject(result);
        result = nullptr;
    } else if (flatten_over) {
        // Standard premultiplied-alpha "over" compositing against a solid
        // background: since RGB is already alpha-premultiplied, the
        // remaining (1-alpha) share of each channel is just the background
        // color's own contribution.
        const int bgB = GetBValue(*flatten_over), bgG = GetGValue(*flatten_over), bgR = GetRValue(*flatten_over);
        auto* bytes = static_cast<BYTE*>(pixels);
        const size_t pixelCount = static_cast<size_t>(width) * height;
        for (size_t i = 0; i < pixelCount; ++i) {
            BYTE* px = bytes + i * 4;
            const int inv = 255 - px[3];
            px[0] = static_cast<BYTE>(std::min(255, px[0] + inv * bgB / 255));
            px[1] = static_cast<BYTE>(std::min(255, px[1] + inv * bgG / 255));
            px[2] = static_cast<BYTE>(std::min(255, px[2] + inv * bgR / 255));
            px[3] = 255;
        }
    }
done:
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    return result;
}

// The highest NPC tier currently selectable in normal (non-test) play: tier 1
// is always open; tier N+1 opens once every tier-N NPC has 10 recorded wins.
// Also used to gate shop pack purchases via Pack::required_tier.
int highest_unlocked_tier(const goat::game::Catalog& catalog, const goat::game::Profile& profile) {
    int maxTier = 1;
    for (const auto& npc : catalog.npcs) maxTier = std::max(maxTier, npc.tier);
    int unlocked = 1;
    for (int tier = 1; tier < maxTier; ++tier) {
        bool anyInTier = false, cleared = true;
        for (const auto& npc : catalog.npcs) {
            if (npc.tier != tier) continue;
            anyInTier = true;
            const auto it = profile.npc_wins.find(npc.id);
            if (it == profile.npc_wins.end() || it->second < 10) { cleared = false; break; }
        }
        if (anyInTier && cleared) unlocked = tier + 1; else break;
    }
    return unlocked;
}

// ---------- per-screen paint ----------

void paint_title(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, client, theme::background, false, 0);
    if (state.title_background) draw_bitmap_cover(dc, state.title_background, client);
    // The big hero title below already carries the app's branding, so the
    // small persistent app header (used on every other screen) is skipped
    // here rather than showing it twice over the backdrop art.
    const auto L = compute_title_layout(client, scale);
    Rect area = inset(client, scale.px(28));
    cut_top(area, scale.px(kHeaderHeight));
    Rect titleRect = cut_top(area, scale.px(120));
    draw_text(dc, titleRect.win32(), L"PROJECT GOAT", scale.points(40), theme::gold, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
    cut_top(area, scale.px(10));
    Rect introRect = cut_top(area, scale.px(70));
    draw_text(dc, introRect.win32(), L"Build a collection, challenge GOAT-format NPCs, and duel with Project Ignis rules.", scale.points(15), theme::textPrimary, DT_CENTER | DT_WORDBREAK);
    draw_button(dc, L.cta, L"ENTER CAMPAIGN", L"Start your journey", scale, L.cta.contains(mouseX, mouseY));
}

void paint_hub(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, client, theme::background, false, 0);
    draw_app_header(dc, client, scale);
    const auto L = compute_hub_layout(client, scale);
    const auto& npc = state.catalog.npcs.at(state.selected_npc % state.catalog.npcs.size());
    const std::wstring headerText = L"Campaign hub — " + std::to_wstring(state.progression.profile().credits) + L" credits";
    draw_text(dc, L.header.win32(), headerText.c_str(), scale.points(17), theme::gold, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    static const wchar_t* navTitles[4] = {L"AUTO DUEL", L"PLAY CPU", L"SHOP", L"COLLECTION"};
    static const wchar_t* navSubs[4] = {L"Watch CPU vs CPU", L"Choose an opponent", L"Buy GOAT packs", L"Browse owned cards"};
    for (int i = 0; i < 4; ++i) draw_button(dc, L.nav[i], navTitles[i], navSubs[i], scale, L.nav[i].contains(mouseX, mouseY));
    draw_button(dc, L.select[0], L"EDIT DECK", utf8_to_wide(fs::path(state.progression.profile().selected_deck).stem().string()).c_str(), scale, L.select[0].contains(mouseX, mouseY));
    draw_button(dc, L.select[1], L"TEST DUEL", L"Any opponent, no rewards", scale, L.select[1].contains(mouseX, mouseY));
    const std::wstring detail = L"Last opponent: " + utf8_to_wide(npc.name) + L" • Reward: " + std::to_wstring(npc.reward.credits) + L" credits";
    draw_text(dc, L.details.win32(), detail.c_str(), scale.points(14), theme::textPrimary, DT_LEFT | DT_WORDBREAK);
    draw_text(dc, L.status.win32(), state.status.c_str(), scale.points(13), theme::textSecondary, DT_LEFT | DT_WORDBREAK);
}

void paint_cpu_select(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, client, theme::background, false, 0);
    draw_app_header(dc, client, scale);
    const auto L = compute_cpu_select_layout(client, scale, state.catalog.npcs.size());
    state.cpu_select_page = std::min(state.cpu_select_page, L.page_count - 1);
    draw_text(dc, L.header.win32(), state.cpu_select_test_mode ? L"TEST DUEL — choose any opponent" : L"PLAY CPU — choose an opponent",
              scale.points(20), theme::gold, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const int unlockedTier = highest_unlocked_tier(state.catalog, state.progression.profile());
    const auto& profile = state.progression.profile();
    const size_t first = state.cpu_select_page * L.per_page;
    for (size_t slot = 0; slot < L.rows.size() && first + slot < state.catalog.npcs.size(); ++slot) {
        const size_t i = first + slot;
        const auto& npc = state.catalog.npcs[i];
        const bool locked = !state.cpu_select_test_mode && npc.tier > unlockedTier;
        const Rect& row = L.rows[slot];
        const bool hovered = !locked && row.contains(mouseX, mouseY);
        draw_panel(dc, row, locked ? RGB(16, 20, 26) : (hovered ? RGB(45, 84, 112) : theme::panel), true,
                   locked ? theme::panelBorder : (hovered ? theme::gold : theme::panelBorder));
        Rect inner = inset(row, scale.px(14), scale.px(8));
        Rect nameRect = cut_top(inner, scale.px(24));
        const std::wstring nameLine = utf8_to_wide(npc.name) + (locked ? L"  —  LOCKED" : L"");
        draw_text(dc, nameRect.win32(), nameLine.c_str(), scale.points(16), locked ? theme::textSecondary : theme::gold, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        std::wstring subtitle;
        if (locked) {
            subtitle = L"Clear every Tier " + std::to_wstring(npc.tier - 1) + L" opponent 10 times to unlock this opponent.";
        } else {
            const auto winsIt = profile.npc_wins.find(npc.id);
            const int wins = winsIt != profile.npc_wins.end() ? winsIt->second : 0;
            subtitle = L"Difficulty " + std::to_wstring(npc.difficulty) + L" • Tier " + std::to_wstring(npc.tier) + L" • " +
                utf8_to_wide(fs::path(npc.deck_path).stem().string());
            subtitle += state.cpu_select_test_mode ? L" • test mode: no rewards" : (L" • " + std::to_wstring(std::min(wins, 10)) + L"/10 wins");
        }
        draw_text(dc, inner.win32(), subtitle.c_str(), scale.points(11), theme::textSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }
    draw_button(dc, L.buttons[0], L"‹ PREV", nullptr, scale, state.cpu_select_page > 0 && L.buttons[0].contains(mouseX, mouseY));
    const std::wstring pageLabel = L"PAGE " + std::to_wstring(state.cpu_select_page + 1) + L"/" + std::to_wstring(L.page_count);
    draw_button(dc, L.buttons[1], pageLabel.c_str(), L"Next page", scale, state.cpu_select_page + 1 < L.page_count && L.buttons[1].contains(mouseX, mouseY));
    draw_button(dc, L.buttons[2], L"BACK", L"Campaign hub", scale, L.buttons[2].contains(mouseX, mouseY));
    draw_text(dc, L.status.win32(), state.status.c_str(), scale.points(13), theme::textSecondary, DT_LEFT | DT_WORDBREAK);
}

void paint_shop(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, client, theme::background, false, 0);
    draw_app_header(dc, client, scale);
    const auto& packs = state.catalog.packs;
    const auto L = compute_shop_layout(client, scale, packs.size());
    const std::wstring headerText = L"CARD SHOP — " + std::to_wstring(state.progression.profile().credits) + L" credits";
    draw_text(dc, L.header.win32(), headerText.c_str(), scale.points(20), theme::gold, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const auto& profile = state.progression.profile();
    const int unlockedTier = highest_unlocked_tier(state.catalog, profile);
    for (size_t i = 0; i < packs.size() && i < L.grid.cards.size(); ++i) {
        const auto& pack = packs[i];
        const Rect& r = L.grid.cards[i];
        const bool locked = pack.required_tier > unlockedTier;
        const bool selected = state.selected_pack == i;
        const bool hovered = r.contains(mouseX, mouseY);
        draw_panel(dc, r, RGB(24, 24, 30), true, selected ? theme::gold : (hovered ? theme::legal : theme::panelBorder));
        if (HBITMAP art = get_pack_texture(state, pack)) draw_bitmap_fit(dc, art, r);
        if (locked) draw_text(dc, r.win32(), L"LOCKED", scale.points(12), theme::danger, DT_CENTER | DT_VCENTER);
        RECT labelRect{r.left, r.bottom + scale.px(4), r.right, r.bottom + scale.px(30)};
        const auto ownedIt = profile.sealed_packs.find(pack.id);
        const int owned = ownedIt != profile.sealed_packs.end() ? ownedIt->second : 0;
        std::wstring label = utf8_to_wide(pack.name);
        if (owned > 0) label += L" (" + std::to_wstring(owned) + L" sealed)";
        draw_text(dc, labelRect, label.c_str(), scale.points(11), theme::textPrimary, DT_CENTER | DT_WORDBREAK);
    }
    if (packs.empty()) draw_text(dc, L.grid_area.win32(), L"No packs available.", scale.points(13), theme::textSecondary, DT_CENTER | DT_VCENTER);

    draw_panel(dc, L.detail, theme::panel, true, theme::panelBorder);
    if (!packs.empty()) {
        const auto& pack = packs[state.selected_pack % packs.size()];
        const bool packLocked = pack.required_tier > unlockedTier;
        Rect inner = inset(L.detail, scale.px(12));
        std::wstring info = utf8_to_wide(pack.name) + L"\n\n" + std::to_wstring(pack.cards_per_pack) + L" cards per pack\n" +
            std::to_wstring(pack.price) + L" credits";
        if (packLocked) info += L"\n\nLocked — clear every Tier " + std::to_wstring(pack.required_tier - 1) + L" opponent 10 times to unlock purchase.";
        draw_text(dc, inner.win32(), info.c_str(), scale.points(14), theme::textPrimary, DT_LEFT | DT_WORDBREAK);
        draw_button(dc, L.buttons[0], packLocked ? L"LOCKED" : L"BUY PACK", packLocked ? L"Requires a higher tier" : L"Opens immediately", scale, !packLocked && L.buttons[0].contains(mouseX, mouseY));
    }
    draw_button(dc, L.buttons[1], L"BACK", L"Campaign hub", scale, L.buttons[1].contains(mouseX, mouseY));
    draw_text(dc, L.status.win32(), state.status.c_str(), scale.points(13), theme::textSecondary, DT_LEFT | DT_WORDBREAK);
}

void paint_pack_opening(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, client, theme::background, false, 0);
    draw_app_header(dc, client, scale);
    const auto L = compute_pack_opening_layout(client, scale, state.opening_cards.size());
    draw_text(dc, L.header.win32(), L"PACK OPENING", scale.points(20), theme::gold, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    constexpr DWORD kFlipDurationMs = 200;
    const DWORD now = GetTickCount();
    for (size_t i = 0; i < state.opening_cards.size() && i < L.grid.cards.size(); ++i) {
        const Rect& r = L.grid.cards[i];
        const bool revealed = i < state.opening_revealed;
        const bool selected = revealed && state.opening_selected == static_cast<int>(i);
        draw_panel(dc, r, RGB(24, 24, 30), true, selected ? theme::gold : (revealed && r.contains(mouseX, mouseY) ? theme::legal : theme::panelBorder));
        if (!revealed) {
            if (state.card_back_texture) draw_bitmap_fit(dc, state.card_back_texture, r);
            continue;
        }
        // The just-revealed card (index == opening_revealed-1) plays a short
        // width-collapse "flip": its tile narrows from full width to 0 and
        // back out, swapping from card-back to face art at the midpoint —
        // every earlier card is simply drawn at full width already revealed.
        Rect drawRect = r;
        bool showBack = false;
        if (i + 1 == state.opening_revealed) {
            const DWORD elapsed = now - state.opening_flip_start_tick;
            if (elapsed < kFlipDurationMs) {
                const double t = static_cast<double>(elapsed) / kFlipDurationMs; // 0..1
                const double widthFactor = std::abs(1.0 - 2.0 * t); // 1 -> 0 -> 1
                const int width = std::max(1, static_cast<int>(r.width() * widthFactor));
                drawRect = {r.center_x() - width / 2, r.top, r.center_x() - width / 2 + width, r.bottom};
                showBack = t < 0.5;
            }
        }
        HBITMAP art = showBack ? state.card_back_texture : get_card_texture(state, state.opening_cards[i]);
        if (art) draw_bitmap_fit(dc, art, drawRect);
    }

    draw_panel(dc, L.detail, theme::panel, true, theme::panelBorder);
    Rect inner = inset(L.detail, scale.px(12));
    if (state.opening_selected >= 0 && static_cast<size_t>(state.opening_selected) < state.opening_cards.size()) {
        const auto& def = state.card_database->resolve(state.opening_cards[static_cast<size_t>(state.opening_selected)]);
        Rect nameRect = cut_top(inner, scale.px(24));
        draw_text(dc, nameRect.win32(), utf8_to_wide(def.name).c_str(), scale.points(14), theme::gold, DT_LEFT | DT_VCENTER | DT_WORDBREAK);
        Rect statRect = cut_top(inner, scale.px(20));
        draw_text(dc, statRect.win32(), describe_card_stats(def).c_str(), scale.points(11), theme::textPrimary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        Rect typeRect = cut_top(inner, scale.px(34));
        draw_text(dc, typeRect.win32(), describe_card_type(def).c_str(), scale.points(10), theme::textSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK);
        if (!def.text.empty()) { cut_top(inner, scale.px(6)); draw_text(dc, inner.win32(), utf8_to_wide(def.text).c_str(), scale.points(10), theme::textPrimary, DT_LEFT | DT_TOP | DT_WORDBREAK); }
    } else {
        draw_text(dc, inner.win32(), L"Click a revealed card to inspect it.", scale.points(11), theme::textSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    const bool allRevealed = state.opening_revealed >= state.opening_cards.size();
    draw_button(dc, L.reveal_all, allRevealed ? L"ALL REVEALED" : L"REVEAL ALL", allRevealed ? L"" : L"Skip the animation", scale, !allRevealed && L.reveal_all.contains(mouseX, mouseY));
    draw_button(dc, L.done, L"DONE", L"Back to the shop", scale, L.done.contains(mouseX, mouseY));
    draw_text(dc, L.status.win32(), state.status.c_str(), scale.points(13), theme::textSecondary, DT_LEFT | DT_WORDBREAK);
}

// Shared by the Collection browser and the Deck Editor's owned-card pool:
// codes from `collection` whose name contains `search` (case-insensitive) and
// whose type matches at least one enabled filter, sorted by name.
std::vector<uint32_t> filtered_collection(goat::CardDatabase& database, const std::map<uint32_t, int>& collection,
                                           const std::wstring& search, bool includeMonster, bool includeSpell, bool includeTrap) {
    std::wstring needle = search;
    std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
    std::vector<uint32_t> result;
    for (const auto& [code, copies] : collection) {
        const auto& def = database.resolve(code);
        if ((def.type & TYPE_MONSTER) && !includeMonster) continue;
        if ((def.type & TYPE_SPELL) && !includeSpell) continue;
        if ((def.type & TYPE_TRAP) && !includeTrap) continue;
        if (!needle.empty()) {
            std::wstring name = utf8_to_wide(def.name);
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name.find(needle) == std::wstring::npos) continue;
        }
        result.push_back(code);
    }
    std::sort(result.begin(), result.end(), [&](uint32_t a, uint32_t b) { return database.resolve(a).name < database.resolve(b).name; });
    return result;
}

// Draws a name/stats/type/text detail panel for `code` into `area` — shared
// by Collection, the Deck Editor panes, and the Pack Opening reveal grid,
// mirroring the duel screen's own card inspector (draw_inspector).
void draw_card_detail(HDC dc, AppState& state, const Rect& area, uint32_t code, const UiScale& scale) {
    if (code == 0) { draw_text(dc, area.win32(), L"Click a card to inspect it.", scale.points(11), theme::textSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK); return; }
    Rect inner = area;
    const auto& def = state.card_database->resolve(code);
    Rect nameRect = cut_top(inner, scale.px(24));
    draw_text(dc, nameRect.win32(), utf8_to_wide(def.name).c_str(), scale.points(14), theme::gold, DT_LEFT | DT_VCENTER | DT_WORDBREAK);
    Rect statRect = cut_top(inner, scale.px(20));
    draw_text(dc, statRect.win32(), describe_card_stats(def).c_str(), scale.points(11), theme::textPrimary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    Rect typeRect = cut_top(inner, scale.px(34));
    draw_text(dc, typeRect.win32(), describe_card_type(def).c_str(), scale.points(10), theme::textSecondary, DT_LEFT | DT_TOP | DT_WORDBREAK);
    if (!def.text.empty()) { cut_top(inner, scale.px(6)); draw_text(dc, inner.win32(), utf8_to_wide(def.text).c_str(), scale.points(10), theme::textPrimary, DT_LEFT | DT_TOP | DT_WORDBREAK); }
}

void paint_collection(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, client, theme::background, false, 0);
    draw_app_header(dc, client, scale);
    const auto& collection = state.progression.profile().collection;
    const std::vector<uint32_t> visible = filtered_collection(*state.card_database, collection, state.search_text,
                                                                state.collection_filter_monster, state.collection_filter_spell, state.collection_filter_trap);
    const auto L = compute_collection_layout(client, scale, visible.size());
    state.collection_page = std::min(state.collection_page, L.grid.page_count - 1);
    position_search_edit(state, L.search_box);
    draw_text(dc, L.header.win32(), L"YOUR COLLECTION", scale.points(20), theme::gold, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    draw_panel(dc, L.filters[0], state.collection_filter_monster ? theme::panel : RGB(16, 20, 26), true, state.collection_filter_monster ? theme::legal : theme::panelBorder);
    draw_text(dc, L.filters[0].win32(), L"Monster", scale.points(11), state.collection_filter_monster ? theme::textPrimary : theme::textSecondary, DT_CENTER | DT_VCENTER);
    draw_panel(dc, L.filters[1], state.collection_filter_spell ? theme::panel : RGB(16, 20, 26), true, state.collection_filter_spell ? theme::legal : theme::panelBorder);
    draw_text(dc, L.filters[1].win32(), L"Spell", scale.points(11), state.collection_filter_spell ? theme::textPrimary : theme::textSecondary, DT_CENTER | DT_VCENTER);
    draw_panel(dc, L.filters[2], state.collection_filter_trap ? theme::panel : RGB(16, 20, 26), true, state.collection_filter_trap ? theme::legal : theme::panelBorder);
    draw_text(dc, L.filters[2].win32(), L"Trap", scale.points(11), state.collection_filter_trap ? theme::textPrimary : theme::textSecondary, DT_CENTER | DT_VCENTER);

    const size_t first = state.collection_page * L.grid.per_page;
    for (size_t slot = 0; slot < L.grid.cards.size() && first + slot < visible.size(); ++slot) {
        const uint32_t code = visible[first + slot];
        const Rect& r = L.grid.cards[slot];
        const bool selected = state.browse_selected_code == code;
        draw_panel(dc, r, RGB(24, 24, 30), true, selected ? theme::gold : (r.contains(mouseX, mouseY) ? theme::legal : theme::panelBorder));
        if (HBITMAP texture = get_card_texture(state, code)) draw_bitmap_fit(dc, texture, r);
        RECT labelRect{r.left, r.bottom + scale.px(4), r.right, r.bottom + scale.px(28)};
        const auto& card = state.card_database->resolve(code);
        const auto copies = collection.at(code);
        const std::wstring label = utf8_to_wide(card.name) + L" x" + std::to_wstring(copies);
        draw_text(dc, labelRect, label.c_str(), scale.points(11), theme::textPrimary, DT_CENTER | DT_WORDBREAK);
    }
    if (visible.empty()) draw_text(dc, L.grid_area.win32(), collection.empty() ? L"Your collection is empty." : L"No cards match this search/filter.", scale.points(13), theme::textSecondary, DT_CENTER | DT_VCENTER);

    draw_panel(dc, L.detail, theme::panel, true, theme::panelBorder);
    draw_card_detail(dc, state, inset(L.detail, scale.px(12)), state.browse_selected_code, scale);

    draw_button(dc, L.buttons[0], L"‹ PREV", nullptr, scale, state.collection_page > 0 && L.buttons[0].contains(mouseX, mouseY));
    const std::wstring pageLabel = L"PAGE " + std::to_wstring(state.collection_page + 1) + L"/" + std::to_wstring(L.grid.page_count);
    draw_button(dc, L.buttons[1], pageLabel.c_str(), L"Next page", scale, state.collection_page + 1 < L.grid.page_count && L.buttons[1].contains(mouseX, mouseY));
    draw_button(dc, L.buttons[2], L"BACK", L"Campaign hub", scale, L.buttons[2].contains(mouseX, mouseY));
    draw_text(dc, L.status.win32(), state.status.c_str(), scale.points(13), theme::textSecondary, DT_LEFT | DT_WORDBREAK);
}

// ---------- Deck Editor ----------

// A shipped deck (starter/NPC) is never overwritten in place — Save on one
// redirects to Save As, which writes under decks/player/ instead.
bool is_shipped_deck_path(const std::string& path) {
    return fs::path(path).parent_path() == fs::path("decks") / "starter";
}

struct DeckRow { uint32_t code; int count; bool is_extra; };
// One row per unique card in `list`, grouped by count — mirrors how every
// other grid tile in this file already shows "Name xN" instead of one row
// per physical copy.
std::vector<DeckRow> deck_rows(const std::vector<uint32_t>& list, bool is_extra) {
    std::map<uint32_t, int> counts;
    for (const auto code : list) ++counts[code];
    std::vector<DeckRow> rows;
    for (const auto& [code, n] : counts) rows.push_back({code, n, is_extra});
    return rows;
}

// Draws one deck-list panel (Main or Extra) on the Deck List screen: a
// header line with the running count, then `rows` laid out across
// `columns` — see compute_column_list_layout, which already sized `columns`
// to fit every row somewhere on screen.
void draw_deck_list_panel(HDC dc, AppState& state, const Rect& panel, const Rect& listArea, const ColumnListLayout& columns,
                           const std::vector<DeckRow>& rows, const std::wstring& headerText, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, panel, theme::panel, true, theme::panelBorder);
    Rect header = inset(panel, scale.px(12));
    header.bottom = header.top + scale.px(22);
    draw_text(dc, header.win32(), headerText.c_str(), scale.points(13), theme::gold, DT_LEFT | DT_VCENTER);
    for (size_t i = 0; i < rows.size() && i < columns.rows.size(); ++i) {
        const Rect& r = columns.rows[i];
        if (r.empty()) continue;
        if (r.contains(mouseX, mouseY)) draw_panel(dc, r, RGB(45, 84, 112), false, 0);
        const auto& def = state.card_database->resolve(rows[i].code);
        const std::wstring label = utf8_to_wide(def.name) + L" x" + std::to_wstring(rows[i].count);
        draw_text(dc, r.win32(), label.c_str(), scale.points(12), theme::textPrimary, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (rows.empty()) draw_text(dc, listArea.win32(), L"(empty)", scale.points(11), theme::textSecondary, DT_LEFT | DT_TOP);
}

// Re-validates the in-progress deck and turns the result into the banner
// shown at the bottom of the editor — reuses the same validation the engine
// itself runs before a real duel (goat::game::validate_player_deck) instead
// of reimplementing size/banlist checks client-side.
void recompute_deck_status(AppState& state) {
    try {
        const auto banlist = goat::Banlist::load("external/LFLists/GOAT.lflist.conf");
        goat::game::validate_player_deck(state.editing_deck, state.progression.profile(), *state.card_database, banlist);
        state.deck_status = L"Legal — " + std::to_wstring(state.editing_deck.main.size()) + L" main / " +
            std::to_wstring(state.editing_deck.extra.size()) + L" extra";
    } catch (const std::exception& error) {
        state.deck_status = utf8_to_wide(error.what());
    }
}

void open_deck_editor(AppState& state, const std::string& deck_path) {
    try {
        state.editing_deck = goat::game::read_deck(deck_path);
    } catch (const std::exception& error) {
        state.status = L"Cannot open deck: " + utf8_to_wide(error.what());
        return;
    }
    state.editing_deck_path = deck_path;
    state.deck_editor_dirty = false;
    state.deck_pool_page = 0;
    state.deck_list_selected = -1;
    state.browse_selected_code = 0;
    state.deck_save_as_active = false;
    state.naming_deck = false;
    state.deck_new_name.clear();
    state.search_text.clear();
    if (state.search_edit) SetWindowTextW(state.search_edit, L"");
    recompute_deck_status(state);
    state.screen = Screen::DeckEditor;
}

// Cycles to the next saved deck under decks/player/ each click (there's no
// list/picker control anywhere else in this client, so this follows the same
// "repeated click browses options" idiom already used for pack/NPC browsing).
void load_next_player_deck(AppState& state) {
    std::vector<fs::path> files;
    std::error_code ignored;
    if (fs::exists("decks/player", ignored)) {
        for (const auto& entry : fs::directory_iterator("decks/player", ignored)) if (entry.path().extension() == L".ydk") files.push_back(entry.path());
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { state.status = L"No saved decks in decks/player/ yet — use Save As first."; return; }
    const auto it = std::find(files.begin(), files.end(), fs::path(state.editing_deck_path));
    const size_t nextIndex = it != files.end() ? (static_cast<size_t>(it - files.begin()) + 1) % files.size() : 0;
    open_deck_editor(state, files[nextIndex].string());
    state.status = L"Loaded " + utf8_to_wide(files[nextIndex].stem().string());
}

// Save / Save As / Load / Set Active / Back — identical on both the Deck
// Editor (pool/add-cards) screen and the Deck List screen, since both act on
// the same in-progress `editing_deck`; only buttons[4] differs between them
// (View Decklist vs. Add Cards) and is handled by each screen's own click
// handler. Returns true if one of these five buttons was clicked.
bool handle_deck_action_buttons(AppState& state, const std::array<Rect, 6>& buttons, int x, int y) {
    if (buttons[0].contains(x, y)) { // Save / Cancel(save-as)
        if (state.deck_save_as_active) {
            state.deck_save_as_active = false; state.naming_deck = false;
            SetWindowTextW(state.search_edit, state.search_text.c_str());
        } else if (is_shipped_deck_path(state.editing_deck_path)) {
            state.deck_save_as_active = true; state.naming_deck = true; state.deck_new_name.clear();
            SetWindowTextW(state.search_edit, L"");
            state.status = L"This is a shipped deck — type a name and click CONFIRM to save your own copy.";
        } else {
            try { goat::game::write_deck(state.editing_deck_path, state.editing_deck); state.deck_editor_dirty = false; state.status = L"Saved."; }
            catch (const std::exception& error) { state.status = utf8_to_wide(error.what()); }
        }
        return true;
    }
    if (buttons[1].contains(x, y)) { // Save As / Confirm
        if (!state.deck_save_as_active) {
            state.deck_save_as_active = true; state.naming_deck = true; state.deck_new_name.clear();
            SetWindowTextW(state.search_edit, L"");
        } else {
            const auto name = sanitize_filename(state.deck_new_name);
            if (name.empty()) { state.status = L"Type a deck name first."; return true; }
            const auto path = (fs::path("decks/player") / (wide_to_utf8(name) + ".ydk")).string();
            try {
                goat::game::write_deck(path, state.editing_deck);
                state.editing_deck_path = path;
                state.deck_editor_dirty = false;
                state.status = L"Saved as " + name + L".";
            } catch (const std::exception& error) { state.status = utf8_to_wide(error.what()); }
            state.deck_save_as_active = false; state.naming_deck = false;
            SetWindowTextW(state.search_edit, state.search_text.c_str());
        }
        return true;
    }
    if (buttons[2].contains(x, y)) { load_next_player_deck(state); return true; } // Load
    if (buttons[3].contains(x, y)) { // Set Active — must be saved (to a non-shipped path) first
        if (state.deck_editor_dirty && !is_shipped_deck_path(state.editing_deck_path)) {
            try { goat::game::write_deck(state.editing_deck_path, state.editing_deck); state.deck_editor_dirty = false; }
            catch (const std::exception& error) { state.status = utf8_to_wide(error.what()); return true; }
        }
        if (state.deck_editor_dirty) { state.status = L"Save your changes first — SAVE AS if this is a shipped deck."; return true; }
        try {
            state.progression.select_starter_deck(state.editing_deck_path);
            goat::game::ProfileStore::save(state.progression.profile(), "saves/default.sav");
            state.status = L"Set as your active duel deck.";
        } catch (const std::exception& error) { state.status = utf8_to_wide(error.what()); }
        return true;
    }
    if (buttons[5].contains(x, y)) { state.screen = Screen::Hub; hide_search_edit(state); return true; } // Back
    return false;
}

void paint_deck_editor(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, client, theme::background, false, 0);
    draw_app_header(dc, client, scale);
    const auto& collection = state.progression.profile().collection;
    const std::vector<uint32_t> pool = filtered_collection(*state.card_database, collection, state.search_text,
                                                             state.deck_filter_monster, state.deck_filter_spell, state.deck_filter_trap);
    const auto L = compute_deck_editor_layout(client, scale, pool.size());
    state.deck_pool_page = std::min(state.deck_pool_page, L.pool_grid.page_count - 1);
    position_search_edit(state, L.search_box);

    draw_text(dc, L.header.win32(), L"DECK EDITOR — ADD CARDS", scale.points(20), theme::gold, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    draw_panel(dc, L.filters[0], state.deck_filter_monster ? theme::panel : RGB(16, 20, 26), true, state.deck_filter_monster ? theme::legal : theme::panelBorder);
    draw_text(dc, L.filters[0].win32(), L"Monster", scale.points(11), state.deck_filter_monster ? theme::textPrimary : theme::textSecondary, DT_CENTER | DT_VCENTER);
    draw_panel(dc, L.filters[1], state.deck_filter_spell ? theme::panel : RGB(16, 20, 26), true, state.deck_filter_spell ? theme::legal : theme::panelBorder);
    draw_text(dc, L.filters[1].win32(), L"Spell", scale.points(11), state.deck_filter_spell ? theme::textPrimary : theme::textSecondary, DT_CENTER | DT_VCENTER);
    draw_panel(dc, L.filters[2], state.deck_filter_trap ? theme::panel : RGB(16, 20, 26), true, state.deck_filter_trap ? theme::legal : theme::panelBorder);
    draw_text(dc, L.filters[2].win32(), L"Trap", scale.points(11), state.deck_filter_trap ? theme::textPrimary : theme::textSecondary, DT_CENTER | DT_VCENTER);

    const size_t first = state.deck_pool_page * L.pool_grid.per_page;
    for (size_t slot = 0; slot < L.pool_grid.cards.size() && first + slot < pool.size(); ++slot) {
        const uint32_t code = pool[first + slot];
        const Rect& r = L.pool_grid.cards[slot];
        const bool selected = state.browse_selected_code == code;
        draw_panel(dc, r, RGB(24, 24, 30), true, selected ? theme::gold : (r.contains(mouseX, mouseY) ? theme::legal : theme::panelBorder));
        if (HBITMAP texture = get_card_texture(state, code)) draw_bitmap_fit(dc, texture, r);
        RECT labelRect{r.left, r.bottom + scale.px(4), r.right, r.bottom + scale.px(28)};
        const auto& def = state.card_database->resolve(code);
        const int owned = collection.at(code);
        const int inDeck = static_cast<int>(std::count(state.editing_deck.main.begin(), state.editing_deck.main.end(), code)) +
                            static_cast<int>(std::count(state.editing_deck.extra.begin(), state.editing_deck.extra.end(), code));
        const std::wstring label = utf8_to_wide(def.name) + L" (" + std::to_wstring(inDeck) + L"/" + std::to_wstring(owned) + L")";
        draw_text(dc, labelRect, label.c_str(), scale.points(10), theme::textPrimary, DT_CENTER | DT_WORDBREAK);
    }
    if (pool.empty()) draw_text(dc, L.pool_area.win32(), L"No owned cards match this search/filter.", scale.points(13), theme::textSecondary, DT_CENTER | DT_VCENTER);

    draw_button(dc, L.pool_prev, L"‹ PREV", nullptr, scale, state.deck_pool_page > 0 && L.pool_prev.contains(mouseX, mouseY));
    const std::wstring poolPageLabel = L"PAGE " + std::to_wstring(state.deck_pool_page + 1) + L"/" + std::to_wstring(L.pool_grid.page_count);
    draw_text(dc, L.pool_page_label.win32(), poolPageLabel.c_str(), scale.points(11), theme::textSecondary, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    draw_button(dc, L.pool_next, L"NEXT ›", nullptr, scale, state.deck_pool_page + 1 < L.pool_grid.page_count && L.pool_next.contains(mouseX, mouseY));

    draw_panel(dc, L.detail, theme::panel, true, theme::panelBorder);
    draw_card_detail(dc, state, inset(L.detail, scale.px(12)), state.browse_selected_code, scale);

    draw_button(dc, L.buttons[0], state.deck_save_as_active ? L"CANCEL" : L"SAVE", nullptr, scale, L.buttons[0].contains(mouseX, mouseY));
    draw_button(dc, L.buttons[1], state.deck_save_as_active ? L"CONFIRM" : L"SAVE AS", state.deck_save_as_active ? L"Type a name, then confirm" : nullptr, scale, L.buttons[1].contains(mouseX, mouseY));
    draw_button(dc, L.buttons[2], L"LOAD", L"Cycle saved decks", scale, L.buttons[2].contains(mouseX, mouseY));
    draw_button(dc, L.buttons[3], L"SET ACTIVE", L"Use for player duels", scale, L.buttons[3].contains(mouseX, mouseY));
    draw_button(dc, L.buttons[4], L"VIEW DECKLIST", L"See main + extra deck", scale, L.buttons[4].contains(mouseX, mouseY));
    draw_button(dc, L.buttons[5], L"BACK", L"Campaign hub", scale, L.buttons[5].contains(mouseX, mouseY));

    draw_text(dc, L.status.win32(), (state.deck_status + L"   —   " + state.status).c_str(), scale.points(13), theme::textSecondary, DT_LEFT | DT_WORDBREAK);
}

void handle_deck_editor_click(AppState& state, const Rect& client, const UiScale& scale, int x, int y) {
    const auto& collection = state.progression.profile().collection;
    const std::vector<uint32_t> pool = filtered_collection(*state.card_database, collection, state.search_text,
                                                             state.deck_filter_monster, state.deck_filter_spell, state.deck_filter_trap);
    const auto L = compute_deck_editor_layout(client, scale, pool.size());

    if (L.filters[0].contains(x, y)) { state.deck_filter_monster = !state.deck_filter_monster; return; }
    if (L.filters[1].contains(x, y)) { state.deck_filter_spell = !state.deck_filter_spell; return; }
    if (L.filters[2].contains(x, y)) { state.deck_filter_trap = !state.deck_filter_trap; return; }

    if (handle_deck_action_buttons(state, L.buttons, x, y)) return;
    if (L.buttons[4].contains(x, y)) { state.screen = Screen::DeckList; return; } // View Decklist

    if (L.pool_prev.contains(x, y)) { if (state.deck_pool_page > 0) --state.deck_pool_page; return; }
    if (L.pool_next.contains(x, y)) { if (state.deck_pool_page + 1 < L.pool_grid.page_count) ++state.deck_pool_page; return; }

    if (state.deck_save_as_active) return; // typing a name — pool clicks are suspended

    const size_t first = state.deck_pool_page * L.pool_grid.per_page;
    for (size_t slot = 0; slot < L.pool_grid.cards.size() && first + slot < pool.size(); ++slot) {
        if (!L.pool_grid.cards[slot].contains(x, y)) continue;
        const uint32_t code = pool[first + slot];
        state.browse_selected_code = code;
        const int owned = collection.at(code);
        const int inDeck = static_cast<int>(std::count(state.editing_deck.main.begin(), state.editing_deck.main.end(), code)) +
                            static_cast<int>(std::count(state.editing_deck.extra.begin(), state.editing_deck.extra.end(), code));
        if (inDeck >= owned) { state.status = L"You don't own another copy of that card."; return; }
        const auto& def = state.card_database->resolve(code);
        const bool isExtraCard = (def.type & (TYPE_FUSION | TYPE_RITUAL)) != 0;
        auto& list = isExtraCard ? state.editing_deck.extra : state.editing_deck.main;
        const size_t cap = isExtraCard ? 15 : 60;
        if (list.size() >= cap) { state.status = isExtraCard ? L"Extra deck is full (15)." : L"Main deck is full (60)."; return; }
        list.push_back(code);
        state.deck_editor_dirty = true;
        recompute_deck_status(state);
        return;
    }
}

void paint_deck_list(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, client, theme::background, false, 0);
    draw_app_header(dc, client, scale);
    const auto mainRows = deck_rows(state.editing_deck.main, false);
    const auto extraRows = deck_rows(state.editing_deck.extra, true);
    const auto L = compute_deck_list_layout(client, scale, mainRows.size(), extraRows.size());

    draw_text(dc, L.header.win32(), L"DECK LIST", scale.points(20), theme::gold, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    draw_deck_list_panel(dc, state, L.main_panel, L.main_list, L.main_columns, mainRows,
                          L"MAIN DECK (" + std::to_wstring(state.editing_deck.main.size()) + L"/60)", scale, mouseX, mouseY);
    draw_deck_list_panel(dc, state, L.extra_panel, L.extra_list, L.extra_columns, extraRows,
                          L"EXTRA DECK (" + std::to_wstring(state.editing_deck.extra.size()) + L"/15)", scale, mouseX, mouseY);

    draw_button(dc, L.buttons[0], state.deck_save_as_active ? L"CANCEL" : L"SAVE", nullptr, scale, L.buttons[0].contains(mouseX, mouseY));
    draw_button(dc, L.buttons[1], state.deck_save_as_active ? L"CONFIRM" : L"SAVE AS", state.deck_save_as_active ? L"Type a name, then confirm" : nullptr, scale, L.buttons[1].contains(mouseX, mouseY));
    draw_button(dc, L.buttons[2], L"LOAD", L"Cycle saved decks", scale, L.buttons[2].contains(mouseX, mouseY));
    draw_button(dc, L.buttons[3], L"SET ACTIVE", L"Use for player duels", scale, L.buttons[3].contains(mouseX, mouseY));
    draw_button(dc, L.buttons[4], L"ADD CARDS", L"Browse your collection", scale, L.buttons[4].contains(mouseX, mouseY));
    draw_button(dc, L.buttons[5], L"BACK", L"Campaign hub", scale, L.buttons[5].contains(mouseX, mouseY));

    draw_text(dc, L.status.win32(), (state.deck_status + L"   —   " + state.status).c_str(), scale.points(13), theme::textSecondary, DT_LEFT | DT_WORDBREAK);
}

void handle_deck_list_click(AppState& state, const Rect& client, const UiScale& scale, int x, int y) {
    const auto mainRows = deck_rows(state.editing_deck.main, false);
    const auto extraRows = deck_rows(state.editing_deck.extra, true);
    const auto L = compute_deck_list_layout(client, scale, mainRows.size(), extraRows.size());

    if (handle_deck_action_buttons(state, L.buttons, x, y)) return;
    if (L.buttons[4].contains(x, y)) { state.screen = Screen::DeckEditor; return; } // Add Cards

    if (state.deck_save_as_active) return; // typing a name — list clicks are suspended

    // Clicking a row in either panel removes one copy of that card.
    auto try_remove = [&](std::vector<uint32_t>& list, const std::vector<DeckRow>& rows, const ColumnListLayout& columns) -> bool {
        for (size_t i = 0; i < rows.size() && i < columns.rows.size(); ++i) {
            if (columns.rows[i].empty() || !columns.rows[i].contains(x, y)) continue;
            const auto it = std::find(list.begin(), list.end(), rows[i].code);
            if (it != list.end()) list.erase(it);
            state.browse_selected_code = rows[i].code;
            state.deck_editor_dirty = true;
            recompute_deck_status(state);
            return true;
        }
        return false;
    };
    if (try_remove(state.editing_deck.main, mainRows, L.main_columns)) return;
    try_remove(state.editing_deck.extra, extraRows, L.extra_columns);
}

void paint_duel(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY) {
    draw_panel(dc, client, theme::background, false, 0);
    const DuelLayout L = compute_duel_layout(client, scale);
    const DuelHover hover = compute_duel_hover(state, L, mouseX, mouseY);
    if (hover.inspect_code) { state.last_inspected_code = hover.inspect_code; state.last_inspected_is_back = false; }
    else if (hover.inspect_is_back) { state.last_inspected_is_back = true; }

    draw_inspector(dc, state, L.inspector, hover, scale, mouseX, mouseY);

    const Rect boardBackdrop{L.opponent_hud.left, L.opponent_hud.top, L.opponent_hud.right, L.player_hud.bottom};
    draw_panel(dc, boardBackdrop, theme::field, true, theme::panelBorder);

    for (int i = 0; i < 5; ++i) {
        // Placement targets only ever make sense on an empty zone and attack
        // targets only on an occupied one; enforcing that here means a stale
        // or unexpected action_layout can never glow the wrong zone kind.
        const bool zoneOccupied = state.monsters[0][static_cast<size_t>(i)].occupied;
        const bool legalMonster = (state.action_layout.all_zone_placement && !state.action_layout.zone_placement_is_spell)
            ? (!zoneOccupied && state.action_layout.zone_to_action[static_cast<size_t>(i)] >= 0)
            : (!state.action_layout.all_zone_placement && zoneOccupied && (state.action_layout.attack_zone_to_action.count(i) > 0 || state.action_layout.monster_board_actions.count(i) > 0));
        const bool selectedMonster = state.selected_monster_zone == i;
        const bool spellZoneOccupied = state.spells[0][static_cast<size_t>(i)].occupied;
        const bool legalSpell = (state.action_layout.all_zone_placement && state.action_layout.zone_placement_is_spell)
            ? (!spellZoneOccupied && state.action_layout.zone_to_action[static_cast<size_t>(i)] >= 0)
            : (!state.action_layout.all_zone_placement && spellZoneOccupied && state.action_layout.spell_board_actions.count(i) > 0);
        const bool selectedSpell = state.selected_spell_zone == i;
        draw_card_slot(dc, state, L.player_monsters[static_cast<size_t>(i)], state.monsters[0][static_cast<size_t>(i)], true, L"MONSTER", scale, hover.player_zone == i || selectedMonster, legalMonster);
        draw_card_slot(dc, state, L.player_spells[static_cast<size_t>(i)], state.spells[0][static_cast<size_t>(i)], false, L"SPELL/TRAP", scale, selectedSpell, legalSpell);
        draw_card_slot(dc, state, L.opponent_monsters[static_cast<size_t>(i)], state.monsters[1][static_cast<size_t>(i)], true, L"MONSTER", scale, false, false);
        draw_card_slot(dc, state, L.opponent_spells[static_cast<size_t>(i)], state.spells[1][static_cast<size_t>(i)], false, L"SPELL/TRAP", scale, false, false);
    }
    draw_card_slot(dc, state, L.player_spells[5], state.spells[0][5], false, L"FIELD", scale, state.selected_spell_zone == 5,
                   state.spells[0][5].occupied && state.action_layout.spell_board_actions.count(5) > 0);
    draw_card_slot(dc, state, L.opponent_spells[5], state.spells[1][5], false, L"FIELD", scale, false, false);
    draw_pile_zone(dc, L.player_deck, L"DECK", state.deck_count[0], scale);
    draw_pile_zone(dc, L.player_extra, L"EXTRA", state.extra_count[0], scale);
    draw_pile_zone(dc, L.player_grave, L"GRAVE", state.grave_count[0], scale);
    draw_pile_zone(dc, L.player_banished, L"BANISH", state.banished_count[0], scale);
    draw_pile_zone(dc, L.opponent_deck, L"DECK", state.deck_count[1], scale);
    draw_pile_zone(dc, L.opponent_extra, L"EXTRA", state.extra_count[1], scale);
    draw_pile_zone(dc, L.opponent_grave, L"GRAVE", state.grave_count[1], scale);
    draw_pile_zone(dc, L.opponent_banished, L"BANISH", state.banished_count[1], scale);

    draw_hud_bar(dc, L.opponent_hud, state, true, scale);
    draw_hud_bar(dc, L.player_hud, state, false, scale);
    draw_turn_indicator(dc, L.turn_indicator, state, scale, mouseX, mouseY);
    draw_hand_row(dc, state, L.opponent_hand, false, hover, scale);
    draw_hand_row(dc, state, L.player_hand, true, hover, scale);
    draw_prompt_panel(dc, state, L.prompt_panel, scale, mouseX, mouseY);
    draw_hand_popup(dc, state, L, scale, mouseX, mouseY);
}

void paint_app(HDC dc, const Rect& client, AppState& state, const UiScale& scale, int mouseX, int mouseY) {
    // Collection and Deck Editor position/show the shared search box
    // themselves (they know their own layout); every other screen hides it.
    if (state.screen != Screen::Collection && state.screen != Screen::DeckEditor) hide_search_edit(state);
    switch (state.screen) {
        case Screen::Title: paint_title(dc, client, state, scale, mouseX, mouseY); break;
        case Screen::Hub: paint_hub(dc, client, state, scale, mouseX, mouseY); break;
        case Screen::Shop: paint_shop(dc, client, state, scale, mouseX, mouseY); break;
        case Screen::PackOpening: paint_pack_opening(dc, client, state, scale, mouseX, mouseY); break;
        case Screen::Collection: paint_collection(dc, client, state, scale, mouseX, mouseY); break;
        case Screen::DeckEditor: paint_deck_editor(dc, client, state, scale, mouseX, mouseY); break;
        case Screen::DeckList: paint_deck_list(dc, client, state, scale, mouseX, mouseY); break;
        case Screen::CpuSelect: paint_cpu_select(dc, client, state, scale, mouseX, mouseY); break;
        case Screen::Duel: paint_duel(dc, client, state, scale, mouseX, mouseY); break;
    }
}

// ---------- hit-testing (mirrors the paint-time layout exactly) ----------

void handle_duel_click(AppState& state, const DuelLayout& L, const UiScale& scale, int x, int y) {
    if (!state.player_process.hProcess) {
        if (state.legal_actions.empty()) state.screen = Screen::Hub;
        return;
    }
    if (state.legal_actions.empty()) return;

    // Debounce: ignore clicks for a short window after any submission. Two
    // Windows click events can land in quick succession (an accidental
    // double-click, or just clicking fast through a run of back-to-back
    // "Confirm" prompts) — without this, the second click can arrive *after*
    // the first one's response already resolved to a brand-new, unrelated
    // prompt (e.g. a fresh Main Phase list), landing on whatever row happens
    // to now occupy that same screen position and submitting it — which is
    // exactly what "clicking confirm summons a random card" looks like.
    constexpr DWORD kClickDebounceMs = 300;
    if (GetTickCount() - state.last_submit_tick < kClickDebounceMs) return;

    // A selected board card's inspector buttons take top priority: they're
    // always visible in a fixed spot, independent of everything else on screen.
    {
        const BoardSelection selection = resolve_board_selection(state);
        if (selection.actions && !selection.actions->empty()) {
            Rect inner = inset(L.inspector, scale.px(12));
            const auto actionLayout = compute_inspector_action_buttons(inner, selection.actions->size(), scale);
            for (size_t i = 0; i < actionLayout.buttons.size(); ++i) {
                if (actionLayout.buttons[i].contains(x, y)) { submit_action(state, (*selection.actions)[i]); return; }
            }
        }
    }

    // The per-card hand popup takes priority: its buttons float above the
    // board, and clicking a hand card should never also trigger whatever is
    // underneath it. A click on the currently-open card toggles it closed; a
    // click on a different hand card switches (or closes, if that card has
    // no actions); a click anywhere else closes it and falls through so the
    // same click can still act on the board/panel underneath.
    const auto handRects = layout_hand(L.player_hand, state.hand_cards.size());
    if (state.open_hand_card >= 0 && static_cast<size_t>(state.open_hand_card) < state.hand_cards.size()) {
        const uint32_t openCode = state.hand_cards[static_cast<size_t>(state.open_hand_card)];
        const auto it = state.action_layout.hand_actions.find(openCode);
        if (it != state.action_layout.hand_actions.end() && !it->second.empty() && static_cast<size_t>(state.open_hand_card) < handRects.size()) {
            const auto popup = compute_hand_popup_layout(handRects[static_cast<size_t>(state.open_hand_card)], it->second.size(), scale, L.opponent_hud);
            for (size_t i = 0; i < popup.buttons.size(); ++i) {
                if (popup.buttons[i].contains(x, y)) { submit_action(state, it->second[i]); return; }
            }
        }
    }
    for (int i = static_cast<int>(handRects.size()) - 1; i >= 0; --i) {
        if (!handRects[static_cast<size_t>(i)].contains(x, y)) continue;
        if (state.open_hand_card == i) { state.open_hand_card = -1; return; }
        const uint32_t code = state.hand_cards[static_cast<size_t>(i)];
        state.open_hand_card = state.action_layout.hand_actions.count(code) ? i : -1;
        return;
    }
    state.open_hand_card = -1;

    const auto pb = compute_phase_bar_layout(L.turn_indicator, scale);
    if (state.action_layout.battle_phase_action >= 0 && pb.battle.contains(x, y)) { submit_action(state, static_cast<size_t>(state.action_layout.battle_phase_action)); return; }
    if (state.action_layout.main_phase2_action >= 0 && pb.main2.contains(x, y)) { submit_action(state, static_cast<size_t>(state.action_layout.main_phase2_action)); return; }
    if (state.action_layout.end_phase_action >= 0 && pb.end.contains(x, y)) { submit_action(state, static_cast<size_t>(state.action_layout.end_phase_action)); return; }

    if (state.action_layout.all_zone_placement) {
        const bool toSpell = state.action_layout.zone_placement_is_spell;
        for (int i = 0; i < 5; ++i) {
            const int actionIndex = state.action_layout.zone_to_action[static_cast<size_t>(i)];
            if (actionIndex < 0) continue;
            const Rect& zoneRect = toSpell ? L.player_spells[static_cast<size_t>(i)] : L.player_monsters[static_cast<size_t>(i)];
            const bool zoneOccupied = toSpell ? state.spells[0][static_cast<size_t>(i)].occupied : state.monsters[0][static_cast<size_t>(i)].occupied;
            if (!zoneOccupied && zoneRect.contains(x, y)) { submit_action(state, static_cast<size_t>(actionIndex)); return; }
        }
        return;
    }

    if (state.action_layout.has_attacks) {
        for (const auto& [zone, actionIndex] : state.action_layout.attack_zone_to_action) {
            if (L.player_monsters[static_cast<size_t>(zone)].contains(x, y)) { submit_action(state, static_cast<size_t>(actionIndex)); return; }
        }
    }

    // Clicking one of your own occupied zones selects it (toggling closed on
    // a second click) when it has Change Position / Activate options; a click
    // there is always consumed, even when that specific card has nothing to do.
    for (int i = 0; i < 5; ++i) {
        if (L.player_monsters[static_cast<size_t>(i)].contains(x, y)) {
            if (state.monsters[0][static_cast<size_t>(i)].occupied && state.action_layout.monster_board_actions.count(i)) {
                state.selected_spell_zone = -1;
                state.selected_monster_zone = (state.selected_monster_zone == i) ? -1 : i;
            }
            return;
        }
        if (L.player_spells[static_cast<size_t>(i)].contains(x, y)) {
            if (state.spells[0][static_cast<size_t>(i)].occupied && state.action_layout.spell_board_actions.count(i)) {
                state.selected_monster_zone = -1;
                state.selected_spell_zone = (state.selected_spell_zone == i) ? -1 : i;
            }
            return;
        }
    }
    if (L.player_spells[5].contains(x, y)) {
        if (state.spells[0][5].occupied && state.action_layout.spell_board_actions.count(5)) {
            state.selected_monster_zone = -1;
            state.selected_spell_zone = (state.selected_spell_zone == 5) ? -1 : 5;
        }
        return;
    }

    const auto pl = compute_prompt_layout(L.prompt_panel, state.action_layout.panel_indices.size(), scale);
    for (size_t i = 0; i < pl.rows.size(); ++i) {
        const size_t globalIndex = state.action_page * kPromptRowsPerPage + i;
        if (globalIndex >= state.action_layout.panel_indices.size()) break;
        if (pl.rows[i].contains(x, y)) { submit_action(state, state.action_layout.panel_indices[globalIndex]); return; }
    }
    if (pl.has_pager) {
        if (pl.prev_button.contains(x, y) && state.action_page > 0) { --state.action_page; return; }
        if (pl.next_button.contains(x, y) && state.action_page + 1 < pl.page_count) { ++state.action_page; return; }
    }
}

int get_window_dpi(HWND window) {
    HDC dc = GetDC(window);
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(window, dc);
    return dpi;
}

HDC ensure_buffer(AppState& state, HDC windowDc, int width, int height) {
    width = std::max(1, width); height = std::max(1, height);
    if (state.buffer_dc && state.buffer_width == width && state.buffer_height == height) return state.buffer_dc;
    if (state.buffer_bitmap) { DeleteObject(state.buffer_bitmap); state.buffer_bitmap = nullptr; }
    if (!state.buffer_dc) state.buffer_dc = CreateCompatibleDC(windowDc);
    state.buffer_bitmap = CreateCompatibleBitmap(windowDc, width, height);
    SelectObject(state.buffer_dc, state.buffer_bitmap);
    state.buffer_width = width; state.buffer_height = height;
    return state.buffer_dc;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w_param, LPARAM l_param) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        auto* created = new AppState;
        created->search_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
                                                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchEditId)),
                                                GetModuleHandleW(nullptr), nullptr);
        if (created->search_edit) {
            static HFONT searchFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                                   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            SendMessageW(created->search_edit, WM_SETFONT, reinterpret_cast<WPARAM>(searchFont), TRUE);
        }
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
        return TRUE;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(l_param);
        info->ptMinTrackSize.x = kMinWindowWidth;
        info->ptMinTrackSize.y = kMinWindowHeight;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // painted fully via the off-screen buffer; avoids a flicker-causing extra erase.
    case WM_SIZE:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        // Coalesce rapid mousemove floods into at most one repaint per frame:
        // WM_PAINT clears the flag, so a burst of moves before the next paint
        // only ever schedules a single InvalidateRect.
        if (state && !state->paint_pending) { state->paint_pending = true; InvalidateRect(window, nullptr, FALSE); }
        return 0;
    case WM_LBUTTONUP: {
        if (!state) break;
        RECT clientRc{}; GetClientRect(window, &clientRc);
        const Rect client{0, 0, static_cast<int>(clientRc.right), static_cast<int>(clientRc.bottom)};
        const UiScale scale = compute_scale(client.width(), client.height(), get_window_dpi(window));
        const int x = static_cast<short>(LOWORD(l_param));
        const int y = static_cast<short>(HIWORD(l_param));

        switch (state->screen) {
        case Screen::Title: {
            const auto L = compute_title_layout(client, scale);
            if (L.cta.contains(x, y)) { state->screen = Screen::Hub; state->status = L"Choose a duel, visit the shop, or view your collection."; }
            break;
        }
        case Screen::Hub: {
            const auto L = compute_hub_layout(client, scale);
            if (L.nav[0].contains(x, y)) state->status = run_automatic_duel();
            else if (L.nav[1].contains(x, y)) { state->screen = Screen::CpuSelect; state->cpu_select_test_mode = false; state->cpu_select_page = 0; }
            else if (L.nav[2].contains(x, y)) state->screen = Screen::Shop;
            else if (L.nav[3].contains(x, y)) {
                state->screen = Screen::Collection; state->collection_page = 0; state->search_text.clear();
                if (state->search_edit) SetWindowTextW(state->search_edit, L"");
            }
            else if (L.select[0].contains(x, y)) { open_deck_editor(*state, state->progression.profile().selected_deck); }
            else if (L.select[1].contains(x, y)) { state->screen = Screen::CpuSelect; state->cpu_select_test_mode = true; state->cpu_select_page = 0; }
            break;
        }
        case Screen::CpuSelect: {
            const auto L = compute_cpu_select_layout(client, scale, state->catalog.npcs.size());
            const int unlockedTier = highest_unlocked_tier(state->catalog, state->progression.profile());
            if (L.buttons[0].contains(x, y)) { if (state->cpu_select_page > 0) --state->cpu_select_page; break; }
            if (L.buttons[1].contains(x, y)) { if (state->cpu_select_page + 1 < L.page_count) ++state->cpu_select_page; break; }
            if (L.buttons[2].contains(x, y)) { state->screen = Screen::Hub; break; }
            const size_t first = state->cpu_select_page * L.per_page;
            for (size_t slot = 0; slot < L.rows.size() && first + slot < state->catalog.npcs.size(); ++slot) {
                if (!L.rows[slot].contains(x, y)) continue;
                const size_t i = first + slot;
                const auto& npc = state->catalog.npcs[i];
                if (!state->cpu_select_test_mode && npc.tier > unlockedTier) break; // locked: click does nothing
                state->selected_npc = i;
                state->screen = Screen::Duel;
                start_player_duel(*state, state->cpu_select_test_mode);
                break;
            }
            break;
        }
        case Screen::Shop: {
            const auto& packs = state->catalog.packs;
            const auto L = compute_shop_layout(client, scale, packs.size());
            bool handled = false;
            for (size_t i = 0; i < packs.size() && i < L.grid.cards.size(); ++i) {
                if (!L.grid.cards[i].contains(x, y)) continue;
                state->selected_pack = i;
                handled = true;
                break;
            }
            if (!handled && !packs.empty() && L.buttons[0].contains(x, y)) {
                const auto& pack = packs[state->selected_pack % packs.size()];
                const int unlockedTier = highest_unlocked_tier(state->catalog, state->progression.profile());
                if (pack.required_tier > unlockedTier) {
                    state->status = L"Locked — clear every Tier " + std::to_wstring(pack.required_tier - 1) + L" opponent 10 times to unlock " + utf8_to_wide(pack.name) + L".";
                } else if (!state->progression.buy_pack(pack)) {
                    state->status = L"Not enough credits for " + utf8_to_wide(pack.name) + L".";
                } else {
                    state->opening_cards = state->progression.open_pack(pack, state->random);
                    goat::game::ProfileStore::save(state->progression.profile(), "saves/default.sav");
                    state->opening_revealed = 0;
                    state->opening_reveal_tick = GetTickCount();
                    state->opening_flip_start_tick = state->opening_reveal_tick;
                    state->opening_selected = -1;
                    state->status = L"Opened " + utf8_to_wide(pack.name) + L".";
                    state->screen = Screen::PackOpening;
                }
            } else if (!handled && L.buttons[1].contains(x, y)) state->screen = Screen::Hub;
            break;
        }
        case Screen::PackOpening: {
            const auto L = compute_pack_opening_layout(client, scale, state->opening_cards.size());
            if (L.reveal_all.contains(x, y)) {
                state->opening_revealed = state->opening_cards.size();
            } else if (L.done.contains(x, y)) {
                state->screen = Screen::Shop;
                state->status = L"Cards added to your collection.";
            } else {
                for (size_t i = 0; i < state->opening_revealed && i < L.grid.cards.size(); ++i) {
                    if (L.grid.cards[i].contains(x, y)) { state->opening_selected = static_cast<int>(i); break; }
                }
            }
            break;
        }
        case Screen::Collection: {
            const auto& collection = state->progression.profile().collection;
            std::vector<uint32_t> visible = filtered_collection(*state->card_database, collection, state->search_text,
                                                                  state->collection_filter_monster, state->collection_filter_spell, state->collection_filter_trap);
            const auto L = compute_collection_layout(client, scale, visible.size());
            position_search_edit(*state, L.search_box);
            if (L.filters[0].contains(x, y)) state->collection_filter_monster = !state->collection_filter_monster;
            else if (L.filters[1].contains(x, y)) state->collection_filter_spell = !state->collection_filter_spell;
            else if (L.filters[2].contains(x, y)) state->collection_filter_trap = !state->collection_filter_trap;
            else if (L.buttons[0].contains(x, y) && state->collection_page > 0) --state->collection_page;
            else if (L.buttons[1].contains(x, y) && state->collection_page + 1 < L.grid.page_count) ++state->collection_page;
            else if (L.buttons[2].contains(x, y)) { state->screen = Screen::Hub; hide_search_edit(*state); }
            else {
                const size_t first = state->collection_page * L.grid.per_page;
                for (size_t slot = 0; slot < L.grid.cards.size() && first + slot < visible.size(); ++slot) {
                    if (L.grid.cards[slot].contains(x, y)) { state->browse_selected_code = visible[first + slot]; break; }
                }
            }
            break;
        }
        case Screen::DeckEditor: {
            handle_deck_editor_click(*state, client, scale, x, y);
            break;
        }
        case Screen::DeckList: {
            handle_deck_list_click(*state, client, scale, x, y);
            break;
        }
        case Screen::Duel:
            handle_duel_click(*state, compute_duel_layout(client, scale), scale, x, y);
            break;
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        if (state) state->paint_pending = false;
        RECT clientRc{}; GetClientRect(window, &clientRc);
        const int width = std::max(1, static_cast<int>(clientRc.right));
        const int height = std::max(1, static_cast<int>(clientRc.bottom));
        if (state) {
            HDC buffer = ensure_buffer(*state, dc, width, height);
            POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(window, &cursor);
            const UiScale scale = compute_scale(width, height, GetDeviceCaps(dc, LOGPIXELSY));
            paint_app(buffer, Rect{0, 0, width, height}, *state, scale, cursor.x, cursor.y);
            BitBlt(dc, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_COMMAND:
        if (state && LOWORD(w_param) == kSearchEditId && HIWORD(w_param) == EN_CHANGE) {
            const int length = GetWindowTextLengthW(state->search_edit);
            std::wstring text(static_cast<size_t>(length), L'\0');
            if (length > 0) GetWindowTextW(state->search_edit, text.data(), length + 1);
            if (state->naming_deck) state->deck_new_name = text; else state->search_text = text;
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (state) {
            if (state->screen == Screen::Duel) {
                for (int i = 0; i < 2; ++i) state->life_display[static_cast<size_t>(i)] += (state->life[static_cast<size_t>(i)] - state->life_display[static_cast<size_t>(i)]) * 0.2;
            }
            if (state->screen == Screen::PackOpening) InvalidateRect(window, nullptr, FALSE);
            if (state->player_process.hProcess) {
                poll_player_duel(*state);
                poll_board_snapshot(*state);
                if (!state->legal_actions.empty()) state->action_layout = build_action_layout(*state);
                InvalidateRect(window, nullptr, FALSE);
            } else if (state->screen == Screen::Duel) {
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_DESTROY:
        if (state) {
            if (state->player_process.hProcess) { TerminateProcess(state->player_process.hProcess, 1); CloseHandle(state->player_process.hThread); CloseHandle(state->player_process.hProcess); }
            if (state->featured_card) DeleteObject(state->featured_card);
            if (state->card_back_texture) DeleteObject(state->card_back_texture);
            if (state->title_background) DeleteObject(state->title_background);
            for (auto& [code, bitmap] : state->texture_cache) if (bitmap) DeleteObject(bitmap);
            for (auto& [art, bitmap] : state->pack_texture_cache) if (bitmap) DeleteObject(bitmap);
            if (state->buffer_bitmap) DeleteObject(state->buffer_bitmap);
            if (state->buffer_dc) DeleteDC(state->buffer_dc);
            delete state;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, w_param, l_param);
}

} // namespace
} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int show) {
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const auto root = project_root();
    SetCurrentDirectoryW(root.wstring().c_str());
    WNDCLASSW wc{};
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.lpfnWndProc = window_proc;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, kClassName, L"Project GOAT", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VISIBLE,
                                   CW_USEDEFAULT, CW_USEDEFAULT, 1440, 900, nullptr, nullptr, instance, nullptr);
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (state) {
        state->featured_card = load_jpeg(fs::path(L"external/packart/goat-starter.jpg"));
        state->card_back_texture = load_jpeg(fs::path(L"external/extra/card_back.jpg"));
        // WIC auto-detects PNG despite the helper's name; flatten its
        // transparency onto the app's own background color so it matches
        // every other screen instead of showing raw (near-black) alpha data.
        constexpr COLORREF kTitleBackgroundColor = theme::background;
        state->title_background = load_jpeg(fs::path(L"external/extra/titlescreen.png"), &kTitleBackgroundColor);
    }
    SetTimer(window, 1, 150, nullptr);
    ShowWindow(window, show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0)) { TranslateMessage(&message); DispatchMessageW(&message); }
    CoUninitialize();
    return 0;
}
