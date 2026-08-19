// Project GOAT — cross-platform client (raylib), Phase 1 slice.
//
// This is a from-scratch port of src/client/main.cpp (the Win32 GDI client)
// onto raylib instead of GDI/WIC/Win32 EDIT controls, so the same game can
// eventually build on macOS and Linux, not just Windows. Only the Title and
// Hub screens exist so far — see the plan this was built from for the full
// phased roadmap. The existing Win32 client is untouched and still the real,
// feature-complete game; this is being built out alongside it.
//
// Architecture note: unlike the Win32 client (which has to split "paint"
// (WM_PAINT) from "handle click" (WM_LBUTTONUP) because GDI's message loop
// delivers them as separate events), raylib's per-frame loop lets a single
// `button(...)` call both draw a button and report whether it was just
// clicked, in the same place. That's used throughout below instead of the
// Win32 client's separate compute_layout/paint/handle_click trio.

#include <raylib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "AudioManager.hpp"
#include "Layout.hpp"
#include "ProcessBridge.hpp"
#include "cards/CardDatabase.hpp"
#include "deck/Banlist.hpp"
#include "game/Catalog.hpp"
#include "game/DeckBuilder.hpp"
#include "game/Progression.hpp"

// Forward-declared rather than pulling in <windows.h> (which collides with
// raylib.h in the same translation unit — both declare e.g. `CloseWindow`,
// `Rectangle`, `LoadImage`, `ShowCursor` with incompatible signatures) —
// only this one function is needed, to locate the exe for project_root().
#ifdef _WIN32
extern "C" unsigned long __stdcall GetModuleFileNameW(void* hModule, wchar_t* lpFilename, unsigned long nSize);
#endif

namespace fs = std::filesystem;
using namespace goat::ui;

namespace {

// Local copies of the ygopro-core bit layout this client needs to interpret
// (type/attribute/race), matching src/client/main.cpp's own local copy —
// kept local rather than including the engine's C headers so the GUI stays
// decoupled from ocgapi's extern "C" block.
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
// Matches ocgapi_constants.h's LOCATION_* — used to correlate a multi-select
// candidate (LegalAction::location, sent by choose_multi_menu in
// src/main.cpp) to the on-screen zone it should highlight.
constexpr uint8_t LOCATION_HAND = 0x02, LOCATION_MZONE = 0x04, LOCATION_SZONE = 0x08;
}
using namespace cardbits;

// ---------- theme (matches src/client/main.cpp's theme:: namespace) ----------

namespace theme {
constexpr Color background    = {10, 16, 24, 255};
constexpr Color panel         = {22, 32, 44, 255};
constexpr Color panelBorder   = {58, 78, 96, 255};
constexpr Color hover         = {45, 84, 112, 255};
constexpr Color field         = {18, 46, 40, 255};
constexpr Color fieldZone     = {27, 63, 55, 255};
constexpr Color fieldZoneLine = {72, 112, 98, 255};
constexpr Color gold          = {214, 178, 94, 255};
constexpr Color legal         = {120, 196, 150, 255};
constexpr Color textPrimary   = {232, 238, 240, 255};
constexpr Color textSecondary = {158, 176, 186, 255};
constexpr Color danger        = {214, 106, 96, 255};
constexpr Color buffed        = {104, 168, 232, 255}; // Current ATK/DEF boosted above printed — a stat drop reuses `danger`.
constexpr Color cardBack      = {46, 40, 92, 255};
}

constexpr int kHeaderHeight = 76;

Rectangle to_rectangle(const Rect& r) {
    return Rectangle{static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.width()), static_cast<float>(r.height())};
}

// ---------- card text (ported from src/client/main.cpp's wide-string
// versions, UTF-8 native here per the "write it right" note in the port plan
// — raylib's text functions consume UTF-8 directly, no wide-char conversion
// needed at all) ----------

const char* attribute_name(uint32_t attribute) {
    switch (attribute) {
        case ATTRIBUTE_EARTH: return "EARTH"; case ATTRIBUTE_WATER: return "WATER";
        case ATTRIBUTE_FIRE: return "FIRE"; case ATTRIBUTE_WIND: return "WIND";
        case ATTRIBUTE_LIGHT: return "LIGHT"; case ATTRIBUTE_DARK: return "DARK";
        case ATTRIBUTE_DIVINE: return "DIVINE"; default: return "";
    }
}

const char* race_name(uint64_t race) {
    switch (race) {
        case RACE_WARRIOR: return "Warrior"; case RACE_SPELLCASTER: return "Spellcaster"; case RACE_FAIRY: return "Fairy";
        case RACE_FIEND: return "Fiend"; case RACE_ZOMBIE: return "Zombie"; case RACE_MACHINE: return "Machine";
        case RACE_AQUA: return "Aqua"; case RACE_PYRO: return "Pyro"; case RACE_ROCK: return "Rock";
        case RACE_WINGEDBEAST: return "Winged Beast"; case RACE_PLANT: return "Plant"; case RACE_INSECT: return "Insect";
        case RACE_THUNDER: return "Thunder"; case RACE_DRAGON: return "Dragon"; case RACE_BEAST: return "Beast";
        case RACE_BEASTWARRIOR: return "Beast-Warrior"; case RACE_DINOSAUR: return "Dinosaur"; case RACE_FISH: return "Fish";
        case RACE_SEASERPENT: return "Sea Serpent"; case RACE_REPTILE: return "Reptile"; case RACE_PSYCHIC: return "Psychic";
        default: return "";
    }
}

std::string describe_card_stats(const goat::CardDefinition& def) {
    if (!(def.type & TYPE_MONSTER)) return "";
    std::string line = "ATK " + std::to_string(def.attack) + " / DEF " + std::to_string(def.defense);
    const uint32_t level = def.level & 0xffu;
    if (level) line += " \xE2\x80\xA2 Level " + std::to_string(level); // U+2022 BULLET
    return line;
}

// Same content as describe_card_stats, but ATK/DEF are drawn as separate
// colored segments: blue when a monster's *current* (effect-modified) value
// on the field is above its printed baseline, red when below, the normal
// text color otherwise — including when `currentAttack`/`currentDefense`
// are -1 (not on the field, e.g. a hand card, so there's nothing to compare
// against). Manual side-by-side layout since draw_text only supports one
// color per call; mirrors draw_text's own spacing/baseline math.
void draw_card_stat_line(const Font& font, const goat::CardDefinition& def, int32_t currentAttack, int32_t currentDefense, const Rect& rect, float fontSize) {
    if (!(def.type & TYPE_MONSTER) || rect.empty()) return;
    const float spacing = fontSize / 10.0f;
    const float y = rect.top + (rect.height() - fontSize) / 2.0f;
    float x = static_cast<float>(rect.left);
    auto segment = [&](const std::string& text, Color color) {
        DrawTextEx(font, text.c_str(), Vector2{x, y}, fontSize, spacing, color);
        x += MeasureTextEx(font, text.c_str(), fontSize, spacing).x;
    };
    auto statColor = [](int32_t current, int32_t base) {
        if (current < 0) return theme::textPrimary; // Not on the field — nothing to compare against.
        if (current > base) return theme::buffed;
        if (current < base) return theme::danger;
        return theme::textPrimary;
    };
    segment("ATK ", theme::textPrimary);
    segment(std::to_string(currentAttack >= 0 ? currentAttack : def.attack), statColor(currentAttack, def.attack));
    segment(" / DEF ", theme::textPrimary);
    segment(std::to_string(currentDefense >= 0 ? currentDefense : def.defense), statColor(currentDefense, def.defense));
    const uint32_t level = def.level & 0xffu;
    if (level) segment(" \xE2\x80\xA2 Level " + std::to_string(level), theme::textSecondary);
}

std::string describe_card_type(const goat::CardDefinition& def) {
    if (def.type & TYPE_MONSTER) {
        std::vector<std::string> parts;
        const std::string race = race_name(def.race);
        if (!race.empty()) parts.push_back(race);
        if (def.type & TYPE_FUSION) parts.emplace_back("Fusion");
        if (def.type & TYPE_RITUAL) parts.emplace_back("Ritual");
        if (def.type & TYPE_EFFECT) parts.emplace_back("Effect"); else if (def.type & TYPE_NORMAL) parts.emplace_back("Normal");
        if (def.type & TYPE_TUNER) parts.emplace_back("Tuner");
        if (def.type & TYPE_UNION) parts.emplace_back("Union");
        if (def.type & TYPE_SPIRIT) parts.emplace_back("Spirit");
        if (def.type & TYPE_GEMINI) parts.emplace_back("Gemini");
        if (def.type & TYPE_FLIP) parts.emplace_back("Flip");
        if (def.type & TYPE_TOON) parts.emplace_back("Toon");
        std::string line;
        for (size_t i = 0; i < parts.size(); ++i) { if (i) line += " / "; line += parts[i]; }
        line += " Monster";
        const std::string attr = attribute_name(def.attribute);
        if (!attr.empty()) line += " \xE2\x80\x94 " + attr; // U+2014 EM DASH
        return line;
    }
    if (def.type & TYPE_SPELL) {
        if (def.type & TYPE_QUICKPLAY) return "Quick-Play Spell Card";
        if (def.type & TYPE_CONTINUOUS) return "Continuous Spell Card";
        if (def.type & TYPE_EQUIP) return "Equip Spell Card";
        if (def.type & TYPE_FIELD) return "Field Spell Card";
        if (def.type & TYPE_RITUAL) return "Ritual Spell Card";
        return "Normal Spell Card";
    }
    if (def.type & TYPE_TRAP) {
        if (def.type & TYPE_CONTINUOUS) return "Continuous Trap Card";
        if (def.type & TYPE_COUNTER) return "Counter Trap Card";
        return "Normal Trap Card";
    }
    return "";
}

// Maps the duel board's raw phase bitmask to a display string — first match
// wins, matching src/client/main.cpp's phase_name.
const char* phase_name(uint16_t phase) {
    if (phase & PHASE_DRAW) return "Draw Phase";
    if (phase & PHASE_STANDBY) return "Standby Phase";
    if (phase & PHASE_MAIN1) return "Main Phase 1";
    if (phase & (PHASE_BATTLE_START | PHASE_BATTLE_STEP | PHASE_BATTLE)) return "Battle Phase";
    if (phase & (PHASE_DAMAGE | PHASE_DAMAGE_CAL)) return "Damage Step";
    if (phase & PHASE_MAIN2) return "Main Phase 2";
    if (phase & PHASE_END) return "End Phase";
    return "";
}

// ---------- text: word-wrap + alignment + ellipsis ----------
//
// raylib's DrawTextEx places one string at one point with no wrapping or
// alignment at all — everything here replaces what GDI's DrawText(DT_CENTER
// | DT_VCENTER | DT_WORDBREAK | DT_SINGLELINE | DT_END_ELLIPSIS) gave the
// Win32 client for free. Every future screen depends on this being right.

enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Center };

struct TextStyle {
    HAlign h = HAlign::Left;
    VAlign v = VAlign::Top;
    bool wrap = false;      // multi-line word wrap (splits on spaces and literal '\n')
    bool ellipsis = false;  // single-line only: truncate with "..." if it doesn't fit
};

// Greedily packs words into lines no wider than maxWidth, and treats each
// literal '\n' in the source text as a forced line break first — matching
// how the ported screens already embed '\n' in a handful of strings (e.g.
// the title screen's hero text).
std::vector<std::string> wrap_text(const Font& font, const std::string& text, float fontSize, float spacing, float maxWidth) {
    std::vector<std::string> lines;
    size_t paragraphStart = 0;
    while (true) {
        const size_t newline = text.find('\n', paragraphStart);
        const std::string paragraph = text.substr(paragraphStart, newline == std::string::npos ? std::string::npos : newline - paragraphStart);

        std::string line;
        size_t wordStart = 0;
        while (wordStart <= paragraph.size()) {
            size_t space = paragraph.find(' ', wordStart);
            if (space == std::string::npos) space = paragraph.size();
            const std::string word = paragraph.substr(wordStart, space - wordStart);
            if (!word.empty()) {
                const std::string candidate = line.empty() ? word : line + " " + word;
                const float width = MeasureTextEx(font, candidate.c_str(), fontSize, spacing).x;
                if (width <= maxWidth || line.empty()) {
                    line = candidate;
                } else {
                    lines.push_back(line);
                    line = word;
                }
            }
            if (space >= paragraph.size()) break;
            wordStart = space + 1;
        }
        lines.push_back(line);

        if (newline == std::string::npos) break;
        paragraphStart = newline + 1;
    }
    return lines;
}

std::string truncate_ellipsis(const Font& font, const std::string& text, float fontSize, float spacing, float maxWidth) {
    if (MeasureTextEx(font, text.c_str(), fontSize, spacing).x <= maxWidth) return text;
    std::string result = text;
    while (!result.empty()) {
        result.pop_back();
        const std::string candidate = result + "...";
        if (MeasureTextEx(font, candidate.c_str(), fontSize, spacing).x <= maxWidth) return candidate;
    }
    return "...";
}

void draw_text(const Font& font, const std::string& text, const Rect& rect, float fontSize, Color color, TextStyle style) {
    if (rect.empty() || text.empty()) return;
    const float spacing = fontSize / 10.0f;
    std::vector<std::string> lines;
    if (style.wrap) {
        lines = wrap_text(font, text, fontSize, spacing, static_cast<float>(rect.width()));
    } else {
        std::string oneLine = text;
        std::replace(oneLine.begin(), oneLine.end(), '\n', ' ');
        if (style.ellipsis) oneLine = truncate_ellipsis(font, oneLine, fontSize, spacing, static_cast<float>(rect.width()));
        lines.push_back(std::move(oneLine));
    }

    const float lineHeight = fontSize * 1.2f;
    const float totalHeight = lineHeight * static_cast<float>(lines.size());
    float y = static_cast<float>(rect.top);
    if (style.v == VAlign::Center) y = rect.top + (rect.height() - totalHeight) / 2.0f;

    for (const auto& line : lines) {
        const float width = MeasureTextEx(font, line.c_str(), fontSize, spacing).x;
        float x = static_cast<float>(rect.left);
        if (style.h == HAlign::Center) x = rect.left + (rect.width() - width) / 2.0f;
        else if (style.h == HAlign::Right) x = rect.right - width;
        DrawTextEx(font, line.c_str(), Vector2{x, y}, fontSize, spacing, color);
        y += lineHeight;
    }
}

// ---------- drawing primitives ----------

void draw_panel(const Rect& r, Color fill, bool border, Color borderColor) {
    DrawRectangleRec(to_rectangle(r), fill);
    if (border) DrawRectangleLinesEx(to_rectangle(r), 1.0f, borderColor);
}

void draw_button(const Font& font, const Rect& r, const char* title, const char* subtitle, const UiScale& scale, bool hovered) {
    draw_panel(r, hovered ? theme::hover : theme::panel, true, hovered ? theme::gold : theme::panelBorder);
    Rect inner = inset(r, scale.px(10));
    const bool hasSubtitle = subtitle && *subtitle;
    Rect titleRect = hasSubtitle ? cut_top(inner, scale.px(22)) : inner;
    draw_text(font, title, titleRect, static_cast<float>(scale.points(hasSubtitle ? 14 : 13)), theme::gold, {HAlign::Left, VAlign::Center, false, true});
    if (hasSubtitle) draw_text(font, subtitle, inner, static_cast<float>(scale.points(11)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});
}

// Draws a button and reports whether it was just clicked — see the
// architecture note at the top of this file for why this can be one call
// here where the Win32 client needs a separate paint pass and click handler.
bool button(const Font& font, const Rect& r, const char* title, const char* subtitle, const UiScale& scale, Vector2 mouse, bool clicked) {
    const bool hovered = r.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
    draw_button(font, r, title, subtitle, scale, hovered);
    const bool activated = hovered && clicked;
    // Routed through a free function (see AudioManager.hpp) rather than
    // threading an AudioManager through every one of this function's ~36
    // call sites across every screen, just for one optional click sound.
    if (activated) goat::audio::play_ui_click();
    return activated;
}

// A toggleable filter chip (Collection's Monster/Spell/Trap buttons) — unlike
// `button()`, its fill/border reflect a persistent `active` state rather than
// just momentary hover, matching src/client/main.cpp's paint_collection.
bool toggle_chip(const Font& font, const Rect& r, const char* label, bool active, const UiScale& scale, Vector2 mouse, bool clicked) {
    const bool hovered = r.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
    draw_panel(r, active ? theme::panel : Color{16, 20, 26, 255}, true, active ? theme::legal : (hovered ? theme::gold : theme::panelBorder));
    draw_text(font, label, r, static_cast<float>(scale.points(11)), active ? theme::textPrimary : theme::textSecondary, {HAlign::Center, VAlign::Center, false, false});
    return hovered && clicked;
}

// A hand-rolled single-line text field: draws the box + current text (plus a
// blinking caret while focused), and updates `text` in place from this
// frame's input. Click-to-focus/unfocus is a single line — any click either
// focuses (inside the box) or defocuses (outside it while active) — since
// exactly one field is active at a time via the caller's `active` flag.
// Kept generic (not tied to Collection) so future screens (Deck Editor
// search + filters, Save As naming) can reuse it against their own
// text/active state without redesign, per the port plan's text-input phase.
void text_field(const Font& font, const Rect& r, std::string& text, bool& active, const UiScale& scale, Vector2 mouse, bool clicked) {
    const bool hovered = r.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
    if (clicked) active = hovered;

    if (active) {
        int codepoint;
        while ((codepoint = GetCharPressed()) != 0) {
            if (codepoint >= 32 && text.size() < 64) {
                int utf8Size = 0;
                const char* utf8 = CodepointToUTF8(codepoint, &utf8Size);
                text.append(utf8, static_cast<size_t>(utf8Size));
            }
        }
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !text.empty()) {
            text.pop_back();
            while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80) text.pop_back();
        }
    }

    draw_panel(r, Color{16, 20, 26, 255}, true, active ? theme::gold : theme::panelBorder);
    Rect inner = inset(r, scale.px(8), 0);
    std::string shown = text;
    if (active && std::fmod(GetTime(), 1.0) < 0.5) shown += "_";
    draw_text(font, shown, inner, static_cast<float>(scale.points(12)), text.empty() ? theme::textSecondary : theme::textPrimary, {HAlign::Left, VAlign::Center, false, false});
}

// Letterboxes `tex` into `dest`, preserving its own aspect ratio ("contain"
// fit) — for card/pack art tiles.
void draw_bitmap_fit(const Texture2D& tex, const Rect& dest) {
    if (tex.id == 0 || dest.empty()) return;
    const double srcAspect = static_cast<double>(tex.width) / tex.height;
    const double destAspect = static_cast<double>(dest.width()) / std::max(1, dest.height());
    Rect fitted = dest;
    if (srcAspect > destAspect) {
        const int height = static_cast<int>(dest.width() / srcAspect);
        fitted.top = dest.top + (dest.height() - height) / 2;
        fitted.bottom = fitted.top + height;
    } else {
        const int width = static_cast<int>(dest.height() * srcAspect);
        fitted.left = dest.left + (dest.width() - width) / 2;
        fitted.right = fitted.left + width;
    }
    const Rectangle src{0, 0, static_cast<float>(tex.width), static_cast<float>(tex.height)};
    DrawTexturePro(tex, src, to_rectangle(fitted), Vector2{0, 0}, 0.0f, WHITE);
}

// Fills `dest` edge-to-edge, cropping whichever axis overflows ("cover"
// fit) — for full-bleed backgrounds.
void draw_bitmap_cover(const Texture2D& tex, const Rect& dest) {
    if (tex.id == 0 || dest.empty()) return;
    const double srcAspect = static_cast<double>(tex.width) / tex.height;
    const double destAspect = static_cast<double>(dest.width()) / std::max(1, dest.height());
    float srcX = 0, srcY = 0, srcW = static_cast<float>(tex.width), srcH = static_cast<float>(tex.height);
    if (srcAspect > destAspect) {
        srcW = static_cast<float>(tex.height * destAspect);
        srcX = (tex.width - srcW) / 2.0f;
    } else {
        srcH = static_cast<float>(tex.width / destAspect);
        srcY = (tex.height - srcH) / 2.0f;
    }
    const Rectangle src{srcX, srcY, srcW, srcH};
    DrawTexturePro(tex, src, to_rectangle(dest), Vector2{0, 0}, 0.0f, WHITE);
}

// Outline-only rect at a given line thickness — raylib's DrawRectangleLinesEx
// already supports variable width directly, so this is just a Rect-typed
// wrapper (matches src/client/main.cpp's draw_rect_outline call sites, which
// vary thickness between 1px normal and 2px hovered/legal-target outlines).
void draw_rect_outline(const Rect& r, float thickness, Color color) {
    DrawRectangleLinesEx(to_rectangle(r), thickness, color);
}

// Stretches `tex` (a portrait card image) rotated 90 degrees to exactly fill
// `dest` — used for defense-position monsters. `dest` here is already the
// approximate landscape footprint computed by defense_footprint (same width
// as the upright card, height = width*kCardAspect — not a literal rotated
// bounding box, just a silhouette that reads as "a rotated card"), so this
// mirrors src/client/main.cpp's draw_bitmap_rotated (a PlgBlt parallelogram
// blit that force-fits the source into that same approximate footprint) by
// scaling the source into a pre-rotation box already sized to become
// `dest`'s exact width x height once rotated, rather than preserving the
// texture's own aspect ratio.
void draw_bitmap_rotated(const Texture2D& tex, const Rect& dest) {
    if (tex.id == 0 || dest.empty()) return;
    const Rectangle src{0, 0, static_cast<float>(tex.width), static_cast<float>(tex.height)};
    const float w = static_cast<float>(dest.width()), h = static_cast<float>(dest.height());
    const Rectangle box{dest.center_x() + 0.0f, dest.center_y() + 0.0f, h, w}; // pre-rotation size is swapped
    DrawTexturePro(tex, src, box, Vector2{h / 2.0f, w / 2.0f}, 90.0f, WHITE);
}

// ---------- image loading ----------
//
// raylib's LoadImage decodes JPEG/PNG (and more) internally via stb_image —
// no WIC/platform imaging API needed, unlike the Win32 client's load_jpeg.

struct TextureCache {
    std::unordered_map<std::string, Texture2D> textures;

    Texture2D get(const std::string& path) {
        const auto it = textures.find(path);
        if (it != textures.end()) return it->second;
        Texture2D tex{};
        if (FileExists(path.c_str())) {
            Image image = LoadImage(path.c_str());
            if (image.data) {
                tex = LoadTextureFromImage(image);
                UnloadImage(image);
            }
        }
        textures.emplace(path, tex);
        return tex;
    }

    void unload_all() {
        for (auto& [path, tex] : textures) if (tex.id != 0) UnloadTexture(tex);
        textures.clear();
    }
};

// ---------- pure geometry: card footprints (duel board) ----------
// Ported directly from src/client/main.cpp's own local copies (not the
// shared Layout.hpp — these are specific to the duel board's zone/hand
// rendering, matching where the Win32 client itself keeps them).

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

// Shared hand-row layout math (opponent hand, player hand, and the anchor
// rect a hand-card action popup is positioned against) — lays `count` cards
// out left-to-right, centered, compressing their spacing (never their own
// size) once they'd otherwise overflow `area`'s width.
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

// A reusable multi-row-and-column card grid for one "page" worth of tiles —
// ported from src/client/main.cpp's compute_card_grid_layout, shared there
// (and here) by the Shop pack grid, the Collection browser, the Deck
// Editor's owned-card pool, and the Pack Opening reveal grid.
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

// Lays out `count` rows in as many left-to-right columns as needed to fit
// within `area`'s height, each row `row_height` tall — used for the Deck
// List screen's Main/Extra panels so every card is always reachable however
// many unique entries there are, instead of truncating past one column's
// worth of rows the way a single scroll-less list would. Ported directly
// from src/client/main.cpp's compute_column_list_layout.
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

// The highest NPC tier currently selectable in normal (non-test) play — also
// used to gate shop pack purchases via Pack::required_tier. Ported directly
// from src/client/main.cpp's highest_unlocked_tier (pure Catalog/Profile
// logic, no Windows dependency).
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

    // The roster grew past what always fit in one screen's worth of rows, so
    // this pages the same way Collection/Deck Editor do rather than silently
    // truncating the rows that don't fit.
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

// Buttons: Save/Cancel, Save As/Confirm, Load, Set Active, View Decklist, Back.
struct DeckEditorLayout { Rect header; Rect search_box; std::array<Rect, 3> filters{}; Rect pool_area; CardGridLayout pool_grid; Rect pool_prev, pool_next, pool_page_label; Rect detail; std::array<Rect, 6> buttons{}; Rect status; };
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

    // Right column: detail panel only (Main/Extra lists live on their own
    // Screen::DeckList screen), leaving the rest of the width for a bigger
    // pool grid.
    const int detailWidth = std::clamp(scale.px(260), 200, std::max(200, area.width() / 4));
    L.detail = cut_right(area, detailWidth);
    cut_right(area, scale.px(14));

    // Local pager strip at the bottom of the pool panel (the global button
    // row above is already full).
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

// Buttons: Save/Cancel, Save As/Confirm, Load, Set Active, Add Cards, Back.
struct DeckListLayout { Rect header; Rect name_box; Rect main_panel, main_list; ColumnListLayout main_columns; Rect extra_panel, extra_list; ColumnListLayout extra_columns; std::array<Rect, 6> buttons{}; Rect status; };
DeckListLayout compute_deck_list_layout(const Rect& client, const UiScale& scale, size_t main_row_count, size_t extra_row_count) {
    DeckListLayout L;
    Rect area = inset(client, scale.px(28));
    cut_top(area, scale.px(kHeaderHeight));
    L.header = cut_top(area, scale.px(34));
    // Reserved for the Save As name field — src/client/main.cpp reuses its
    // one shared native EDIT control here too, but since it only ever
    // *positions* that control on the Collection and Deck Editor screens,
    // Save As is actually unreachable from its Deck List screen (there's
    // nowhere visible to type the name). Reserving real space for a
    // `text_field` here fixes that rather than reproducing it.
    L.name_box = cut_top(area, scale.px(30));
    cut_top(area, scale.px(14));
    L.status = cut_bottom(area, scale.px(40));
    Rect buttonRow = cut_bottom(area, scale.px(44));
    cut_bottom(area, scale.px(14));
    const int gap = scale.px(8);
    const int w = (buttonRow.width() - gap * 5) / 6;
    for (int i = 0; i < 6; ++i) { const int x = buttonRow.left + i * (w + gap); L.buttons[static_cast<size_t>(i)] = {x, buttonRow.top, x + w, buttonRow.bottom}; }

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

// ---------- duel board: pure layout/data structs ----------
// Ported from src/client/main.cpp's own duel-only structs/layout functions.

// `attack`/`defense` are the monster's current (possibly effect-modified)
// stats from state.txt's extended monster= line, sentinel -1 when not
// applicable (spell/trap zones never populate these) or not yet known.
struct FieldCard { bool occupied{}; uint32_t code{}; uint8_t position{}; int32_t attack{-1}; int32_t defense{-1}; };

// `controller`/`location`/`sequence` are only ever populated for a
// multi-select prompt's candidates (see poll_player_duel's "#SELECT"
// branch) — every other action leaves them at their defaults, unused.
// LOCATION_MZONE/LOCATION_SZONE/LOCATION_HAND let a multi-select candidate
// be correlated to the physical zone it's rendered in for click-to-select;
// anything else (graveyard, banished, deck, extra) has no on-screen zone
// and falls back to a plain list row.
struct LegalAction { std::string label; std::string image; uint32_t code{}; uint8_t controller{}; uint8_t location{}; uint32_t sequence{}; };

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

constexpr size_t kPromptRowsPerPage = 3;

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
            monsterZones[static_cast<size_t>(i)] = inset(Rect{x, monsterRow.top, x + zoneWidth, monsterRow.bottom}, 0, scale.px(2));
            spellZones[static_cast<size_t>(i)] = inset(Rect{x, spellRow.top, x + zoneWidth, spellRow.bottom}, 0, scale.px(2));
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

// The engine's action text repeats whichever card the action applies to
// ("Summon Protector of the Throne", "Change position Giant Soldier of
// Stone") which is redundant once that specific card's info is already
// showing (the hand popup is anchored to it; the inspector is already
// displaying it) — this strips the verb down to just that. Unifies
// src/client/main.cpp's two near-identical tables (hand_action_short_label
// and short_action_label, used for the hand popup and inspector board-
// selection buttons respectively) into one, since short_action_label's own
// table there is already a strict superset (it additionally handles "Change
// position ", which never applies to a hand card anyway, so sharing one
// function changes nothing observable in either use).
std::string short_action_label(const std::string& text) {
    static const std::vector<std::pair<std::string, std::string>> verbs = {
        {"Change position ", "Change Position"}, {"Special summon ", "Special Summon"},
        {"Summon ", "Summon"}, {"Set monster ", "Set (Defense)"},
        {"Set spell/trap ", "Set"}, {"Activate ", "Activate"},
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

// Shared conceptually with src/client/main.cpp's filtered_collection: codes
// from `collection` whose name contains `search` (case-insensitive) and
// whose type matches at least one enabled filter, sorted by name.
std::vector<uint32_t> filtered_collection(goat::CardDatabase& database, const std::map<uint32_t, int>& collection,
                                           const std::string& search, bool includeMonster, bool includeSpell, bool includeTrap) {
    std::string needle = search;
    std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::vector<uint32_t> result;
    for (const auto& [code, copies] : collection) {
        const auto& def = database.resolve(code);
        if ((def.type & TYPE_MONSTER) && !includeMonster) continue;
        if ((def.type & TYPE_SPELL) && !includeSpell) continue;
        if ((def.type & TYPE_TRAP) && !includeTrap) continue;
        if (!needle.empty()) {
            std::string name = def.name;
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (name.find(needle) == std::string::npos) continue;
        }
        result.push_back(code);
    }
    std::sort(result.begin(), result.end(), [&](uint32_t a, uint32_t b) { return database.resolve(a).name < database.resolve(b).name; });
    return result;
}

// ---------- app state / screens ----------

enum class Screen { Title, Hub, Collection, Shop, PackOpening, DeckEditor, DeckList, CpuSelect, Duel, Options };

// Which pile the duel board's Graveyard/Banished viewer overlay (see
// draw_pile_viewer) is currently showing — None means the overlay is
// closed. Not a Screen: it's a modal drawn on top of Screen::Duel, not a
// navigation target of its own (see AppState::viewing_pile_kind).
// Extra deck is player-only (see AppState::extra_cards) — unlike Grave/
// Banished, there's no "opponent's Extra" viewer at all; opening one always
// implies viewing_pile_player == 0.
enum class PileKind { None, Grave, Banished, Extra };

// Menu screens (everything but Duel) all share one continuous music
// context — see desired_music_for below — but Options is reachable from any
// of them, so BACK needs to know which one to return to rather than
// hardcoding Hub. Set right before switching into Screen::Options wherever
// that happens (draw_app_header, paint_title); read only by paint_options.
struct AppState {
    Screen screen = Screen::Title;
    Screen options_return_screen = Screen::Hub;
    Font font{};
    goat::audio::AudioManager audio;
    TextureCache textures;
    std::string status = "Choose a mode to begin your GOAT Format campaign.";

    goat::CardDatabase card_database{"external/BabelCDB/cards.cdb", "external/BabelCDB/goat-entries.cdb"};
    goat::game::Progression progression{goat::game::ProfileStore::load("saves/default.sav")};
    goat::game::Catalog catalog{goat::game::load_catalog("data/npcs.json", "data/packs.json")};
    std::mt19937 random{std::random_device{}()};

    // ---- Collection browser (see paint_collection) ----
    size_t collection_page{};
    bool collection_filter_monster = true, collection_filter_spell = true, collection_filter_trap = true;
    uint32_t browse_selected_code{};
    std::string search_text;
    bool search_active{};

    // ---- Shop / Pack Opening (see paint_shop / paint_pack_opening) ----
    size_t selected_pack{};
    std::vector<uint32_t> opening_cards;
    size_t opening_revealed{};
    double opening_reveal_start_time{};
    int opening_selected = -1;

    // ---- Deck Editor / Deck List (see paint_deck_editor / paint_deck_list) ----
    goat::game::DeckContents editing_deck;
    std::string editing_deck_path;
    bool deck_editor_dirty{};
    size_t deck_pool_page{};
    bool deck_filter_monster = true, deck_filter_spell = true, deck_filter_trap = true;
    bool deck_save_as_active{};
    std::string deck_new_name;
    bool deck_naming_active{};
    std::string deck_status; // legality banner, recomputed after every deck edit

    // ---- CPU Select (see paint_cpu_select) ----
    size_t selected_npc{};
    bool cpu_select_test_mode{};
    size_t cpu_select_page{};

    // ---- Duel board / engine IPC (see paint_duel) ----
    // Duel-board fields here mirror only what the engine bridge publishes in
    // state.txt; interaction state (hover/last-inspected) is kept alongside
    // but never fed back into anything the engine reads, keeping the UI/
    // engine boundary one-directional — matches src/client/main.cpp's own
    // AppState doc-comment for this section.
    goat::process::Process player_process{};
    // Unique per running instance (not just "active-duel" — src/client/
    // main.cpp's own fixed name, which this matched originally) so a stray
    // leftover process from an earlier test run can never write into the
    // same request.txt/response.txt/state.txt this instance is using. Cross-
    // instance leftover processes have been observed more than once in this
    // project's own development, and would otherwise corrupt the IPC
    // protocol in ways that look exactly like a hung duel from the client's
    // side (a response mysteriously vanishes, or a stale request.txt from
    // the *other* process's session gets picked up).
    fs::path session_directory = fs::path("sessions") / ("active-duel-" + std::to_string(goat::process::current_pid()));
    std::vector<LegalAction> legal_actions;
    ActionLayout action_layout;
    size_t action_page{};
    // "Select between min and max cards" prompts (choose_multi_menu in
    // src/main.cpp — request.txt's reserved "#SELECT <min> <max>" first
    // line) render as click-the-card-on-the-board instead of the normal
    // legal_actions list; the candidates themselves still live in
    // legal_actions (each entry's controller/location/sequence identify its
    // zone), just interpreted differently — see poll_player_duel,
    // resolve_duel_click, and draw_prompt_panel's multi-select branches.
    // `multi_select_toggled` always matches legal_actions' length; toggled[i]
    // true means that candidate is currently selected.
    bool multi_select_active{};
    uint32_t multi_select_min{}, multi_select_max{};
    std::vector<bool> multi_select_toggled;
    bool duel_is_test_mode{};
    double last_submit_time{}; // GetTime()-based click-debounce window (seconds)
    double last_poll_time{};   // GetTime() the file-based IPC was last polled — throttled independently of render framerate, see paint_duel
    // Detects "this request.txt is a genuinely new prompt, not the one we
    // already answered" — see poll_player_duel for why this pair of signals
    // is used instead of a filesystem timestamp (this toolchain's
    // fs::last_write_time() truncates to ~1-second granularity, so any two
    // prompts published within the same wall-clock second were previously
    // indistinguishable and the second one got silently dropped — a
    // deterministic bug, not a rare race, since back-to-back decisions
    // within the same second are routine during normal play).
    bool request_txt_seen_missing = true; // true once request.txt has been observed absent since the last prompt we parsed; starts true so the very first prompt is always fresh
    std::string last_request_text;        // raw content of the last prompt we parsed — a second, independent signal alongside the flag above
    std::string prompt_title;
    std::string last_board_snapshot;
    double last_board_change_time{}; // GetTime() poll_board_snapshot last saw state.txt actually change — used to tell "busy" (board still updating) apart from "stuck" (nothing changing at all), see kStuckDuelSeconds
    double waiting_since{};    // GetTime() we started showing "Waiting for the rules engine…" for the current gap, or 0 when not waiting — see paint_duel's stuck-duel fallback

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
    uint8_t turn_player{};
    uint32_t turn_number{};
    uint16_t phase{};

    // ---- Graveyard/Banished pile viewer (see draw_pile_viewer) ----
    // Populated from state.txt's gravecards=/banishedcards= lines the same
    // way hand_cards is from handcards=; unlike hand_cards, both players'
    // piles are (mostly — see the engine's own write_board_state comment on
    // face-down banished cards) public information, so both indices are
    // always populated, not just the local human player's.
    std::array<std::vector<uint32_t>, 2> grave_cards{};
    std::array<std::vector<uint32_t>, 2> banished_cards{};
    // Extra deck is private information (only its owner knows what's left in
    // it), same as hand_cards — a flat vector for the same reason hand_cards
    // is: the engine only ever writes the local human player's own extracards=
    // line, never the opponent's (see write_board_state).
    std::vector<uint32_t> extra_cards;
    PileKind viewing_pile_kind = PileKind::None;
    int viewing_pile_player = -1;   // 0 = you, 1 = opponent; meaningless when viewing_pile_kind == None or Extra (always "you")
    int viewing_pile_selected = -1; // index into the relevant pile vector, or -1
    size_t viewing_pile_page{};

    uint32_t last_inspected_code{};
    bool last_inspected_is_back{};
};

// The persistent brand header shown atop every screen except Title and Duel
// — ported from src/client/main.cpp's draw_app_header. Every screen's own
// layout reserves `kHeaderHeight` at the top for this (see e.g.
// compute_collection_layout) and calls this to paint into it. Also owns the
// OPTIONS button in its top-right corner (see AppState::options_return_screen)
// so every menu screen gets Options for free instead of each screen wiring
// its own; Duel never calls this at all, so Options is never exposed mid-duel.
void draw_app_header(AppState& state, const Rect& client, const UiScale& scale, Vector2 mouse, bool clicked) {
    Rect area = inset(client, scale.px(28));
    Rect header = cut_top(area, scale.px(50));
    Rect optionsButton = cut_right(header, scale.px(96));
    draw_text(state.font, "PROJECT GOAT", header, static_cast<float>(scale.points(26)), theme::gold, {HAlign::Left, VAlign::Center, false, false});
    Rect sub = cut_top(area, scale.px(26));
    draw_text(state.font, "Project Ignis rules \xE2\x80\xA2 2005 GOAT Format", sub, static_cast<float>(scale.points(12)), theme::textSecondary, {HAlign::Left, VAlign::Center, false, false});
    if (button(state.font, inset(optionsButton, 0, scale.px(8)), "OPTIONS", nullptr, scale, mouse, clicked)) {
        state.options_return_screen = state.screen;
        state.screen = Screen::Options;
    }
}

// Looks up (and caches) the card-art texture for `code`, falling back to the
// card's alias id — cards resolved through goat-entries.cdb (a GOAT-accurate
// ruling variant) live on the board under a synthetic id distinct from the
// real card's id, but art is only ever filed under the real id.
Texture2D get_card_texture(AppState& state, uint32_t code) {
    if (code == 0) return Texture2D{};
    Texture2D tex = state.textures.get("external/card_images/" + std::to_string(code) + ".jpg");
    if (tex.id == 0) {
        const auto& def = state.card_database.resolve(code);
        if (def.alias != 0 && def.alias != code) tex = state.textures.get("external/card_images/" + std::to_string(def.alias) + ".jpg");
    }
    return tex;
}

// Draws a name/stats/type/text detail panel for `code` into `area` — shared
// conceptually with src/client/main.cpp's draw_card_detail, and reused here
// by Collection, Pack Opening, and (below) the Deck Editor's pool.
void draw_card_detail(AppState& state, const Rect& area, uint32_t code, const UiScale& scale) {
    if (code == 0) { draw_text(state.font, "Click a card to inspect it.", area, static_cast<float>(scale.points(11)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false}); return; }
    Rect inner = area;
    const auto& def = state.card_database.resolve(code);
    Rect nameRect = cut_top(inner, scale.px(24));
    draw_text(state.font, def.name, nameRect, static_cast<float>(scale.points(14)), theme::gold, {HAlign::Left, VAlign::Center, true, false});
    Rect statRect = cut_top(inner, scale.px(20));
    draw_text(state.font, describe_card_stats(def), statRect, static_cast<float>(scale.points(11)), theme::textPrimary, {HAlign::Left, VAlign::Center, false, false});
    Rect typeRect = cut_top(inner, scale.px(34));
    draw_text(state.font, describe_card_type(def), typeRect, static_cast<float>(scale.points(10)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});
    if (!def.text.empty()) { cut_top(inner, scale.px(6)); draw_text(state.font, def.text, inner, static_cast<float>(scale.points(10)), theme::textPrimary, {HAlign::Left, VAlign::Top, true, false}); }
}

// True for decks shipped under decks/starter/ — read-only in the editor
// (Save always redirects to Save As for these, matching src/client/main.cpp's
// is_shipped_deck_path).
bool is_shipped_deck_path(const std::string& path) {
    return fs::path(path).parent_path() == fs::path("decks") / "starter";
}

// Keeps a typed deck name usable as a filename: strips path separators and
// other characters the shell would otherwise choke on, then trims
// surrounding spaces. Ported from src/client/main.cpp's sanitize_filename
// (UTF-8 std::string here instead of wstring — the forbidden-character set
// is a superset of what any target OS forbids, so no behavior change).
std::string sanitize_filename(std::string name) {
    static const std::string forbidden = "\\/:*?\"<>|";
    for (auto& ch : name) if (forbidden.find(ch) != std::string::npos) ch = '_';
    while (!name.empty() && (name.front() == ' ' || name.back() == ' ')) {
        if (name.front() == ' ') name.erase(name.begin());
        if (!name.empty() && name.back() == ' ') name.pop_back();
    }
    return name;
}

// Reloads the banlist and re-validates `editing_deck` after every add/remove,
// mirroring src/client/main.cpp's recompute_deck_status.
void recompute_deck_status(AppState& state) {
    try {
        const auto banlist = goat::Banlist::load("external/LFLists/GOAT.lflist.conf");
        goat::game::validate_player_deck(state.editing_deck, state.progression.profile(), state.card_database, banlist);
        state.deck_status = "Legal \xE2\x80\x94 " + std::to_string(state.editing_deck.main.size()) + " main / " + std::to_string(state.editing_deck.extra.size()) + " extra";
    } catch (const std::exception& error) {
        state.deck_status = error.what();
    }
}

void open_deck_editor(AppState& state, const std::string& deck_path) {
    try {
        state.editing_deck = goat::game::read_deck(deck_path);
    } catch (const std::exception& error) {
        state.status = std::string("Cannot open deck: ") + error.what();
        return;
    }
    state.editing_deck_path = deck_path;
    state.deck_editor_dirty = false;
    state.deck_pool_page = 0;
    state.browse_selected_code = 0;
    state.deck_save_as_active = false;
    state.deck_naming_active = false;
    state.deck_new_name.clear();
    state.search_text.clear();
    state.search_active = false;
    recompute_deck_status(state);
    state.screen = Screen::DeckEditor;
}

// Cycles alphabetically through every *.ydk directly under decks/player/,
// advancing from whichever one is currently open (or starting at the first
// if the current deck isn't a saved player deck). Ported directly from
// src/client/main.cpp's load_next_player_deck, including its habit of
// unconditionally overwriting `status` with "Loaded ..." right after
// open_deck_editor — so a read failure's more specific error message gets
// immediately replaced by this line. That's a pre-existing cosmetic quirk in
// the original (only reachable if a file in decks/player/ is unreadable);
// kept as-is for parity rather than "fixed" here.
void load_next_player_deck(AppState& state) {
    std::vector<fs::path> files;
    std::error_code ignored;
    if (fs::exists("decks/player", ignored)) {
        for (const auto& entry : fs::directory_iterator("decks/player", ignored)) {
            if (entry.path().extension() == ".ydk") files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) { state.status = "No saved decks in decks/player/ yet \xE2\x80\x94 use Save As first."; return; }
    const auto it = std::find(files.begin(), files.end(), fs::path(state.editing_deck_path));
    const size_t nextIndex = it != files.end() ? (static_cast<size_t>(it - files.begin()) + 1) % files.size() : 0;
    open_deck_editor(state, files[nextIndex].string());
    state.status = "Loaded " + files[nextIndex].stem().string();
}

// The Save/Cancel, Save As/Confirm, Load, Set Active, and Back buttons are
// identical on both the Deck Editor (pool/add-cards) screen and the Deck
// List screen, since both act on the same in-progress `editing_deck` — only
// the 5th button differs (View Decklist vs. Add Cards) and is drawn/handled
// by each screen's own paint function. Ported from src/client/main.cpp's
// handle_deck_action_buttons, but since this client's button() draws *and*
// reports a click in one call (see the architecture note at the top of the
// file), this draws the buttons too rather than being click-only.
void deck_action_buttons(AppState& state, const std::array<Rect, 6>& buttons, const UiScale& scale, Vector2 mouse, bool clicked) {
    if (button(state.font, buttons[0], state.deck_save_as_active ? "CANCEL" : "SAVE", nullptr, scale, mouse, clicked)) {
        if (state.deck_save_as_active) {
            state.deck_save_as_active = false;
            state.deck_naming_active = false;
        } else if (is_shipped_deck_path(state.editing_deck_path)) {
            state.deck_save_as_active = true;
            state.deck_naming_active = true;
            state.deck_new_name.clear();
            state.status = "This is a shipped deck \xE2\x80\x94 type a name and click CONFIRM to save your own copy.";
        } else {
            try {
                goat::game::write_deck(state.editing_deck_path, state.editing_deck);
                state.deck_editor_dirty = false;
                state.status = "Saved.";
            } catch (const std::exception& error) { state.status = error.what(); }
        }
    }
    if (button(state.font, buttons[1], state.deck_save_as_active ? "CONFIRM" : "SAVE AS", state.deck_save_as_active ? "Type a name, then confirm" : nullptr, scale, mouse, clicked)) {
        if (!state.deck_save_as_active) {
            state.deck_save_as_active = true;
            state.deck_naming_active = true;
            state.deck_new_name.clear();
        } else {
            const std::string name = sanitize_filename(state.deck_new_name);
            if (name.empty()) {
                state.status = "Type a deck name first.";
            } else {
                const std::string path = "decks/player/" + name + ".ydk";
                try {
                    goat::game::write_deck(path, state.editing_deck);
                    state.editing_deck_path = path;
                    state.deck_editor_dirty = false;
                    state.status = "Saved as " + name + ".";
                } catch (const std::exception& error) { state.status = error.what(); }
                state.deck_save_as_active = false;
                state.deck_naming_active = false;
            }
        }
    }
    if (button(state.font, buttons[2], "LOAD", "Cycle saved decks", scale, mouse, clicked)) load_next_player_deck(state);
    if (button(state.font, buttons[3], "SET ACTIVE", "Use for player duels", scale, mouse, clicked)) {
        bool autoSaveFailed = false;
        if (state.deck_editor_dirty && !is_shipped_deck_path(state.editing_deck_path)) {
            try {
                goat::game::write_deck(state.editing_deck_path, state.editing_deck);
                state.deck_editor_dirty = false;
            } catch (const std::exception& error) {
                state.status = error.what();
                autoSaveFailed = true;
            }
        }
        if (!autoSaveFailed) {
            if (state.deck_editor_dirty) {
                state.status = "Save your changes first \xE2\x80\x94 SAVE AS if this is a shipped deck.";
            } else {
                try {
                    state.progression.select_starter_deck(state.editing_deck_path);
                    goat::game::ProfileStore::save(state.progression.profile(), "saves/default.sav");
                    state.status = "Set as your active duel deck.";
                } catch (const std::exception& error) { state.status = error.what(); }
            }
        }
    }
    if (button(state.font, buttons[5], "BACK", "Campaign hub", scale, mouse, clicked)) state.screen = Screen::Hub;
}

// ---------- duel board: hover/selection resolution ----------

// `inspect_monster_player`/`inspect_monster_sequence` identify exactly which
// monster-zone slot is being inspected (when applicable), so the inspector
// can look up its *current* ATK/DEF alongside the printed stats — a hand
// card or spell/trap zone never sets these, since only monsters have stats.
struct DuelHover { uint32_t inspect_code = 0; bool inspect_is_back = false; int hand_index = -1; int player_zone = -1; int inspect_monster_player = -1; int inspect_monster_sequence = -1; };

// Ported directly from src/client/main.cpp's compute_duel_hover.
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
            if (card.occupied) { if (card.code == 0) hover.inspect_is_back = true; else { hover.inspect_code = card.code; hover.inspect_monster_player = 0; hover.inspect_monster_sequence = i; } }
            return hover;
        }
        if (L.player_spells[si].contains(x, y)) {
            const auto& card = state.spells[0][si];
            if (card.occupied) { if (card.code == 0) hover.inspect_is_back = true; else hover.inspect_code = card.code; }
            return hover;
        }
        if (L.opponent_monsters[si].contains(x, y)) {
            const auto& card = state.monsters[1][si];
            if (card.occupied) { if (card.code == 0) hover.inspect_is_back = true; else { hover.inspect_code = card.code; hover.inspect_monster_player = 1; hover.inspect_monster_sequence = i; } }
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

// Clicking an occupied own monster/spell zone (outside zone-placement/attack
// mode) selects it; the inspector then shows buttons for just that card's
// legal actions (Change Position, Activate) instead of a flat list entry.
// An explicit selection like this always outranks mere hover.
// `monster_sequence` is set alongside `code` whenever the selection is a
// monster-zone slot (always the player's own — see below), so the inspector
// can look up its current ATK/DEF; -1 for a spell/trap selection or no
// selection at all.
struct BoardSelection { uint32_t code = 0; const std::vector<size_t>* actions = nullptr; int monster_sequence = -1; };

BoardSelection resolve_board_selection(AppState& state) {
    BoardSelection info;
    if (state.selected_monster_zone >= 0 && static_cast<size_t>(state.selected_monster_zone) < 5) {
        const auto& card = state.monsters[0][static_cast<size_t>(state.selected_monster_zone)];
        if (card.occupied) { info.code = card.code; info.monster_sequence = state.selected_monster_zone; }
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

// Maps state.legal_actions (the flat list the engine published) onto zones/
// hand cards/phase buttons/the fallback panel — ported directly from
// src/client/main.cpp's build_action_layout.
ActionLayout build_action_layout(AppState& state) {
    ActionLayout layout;
    bool everyEntryIsZonePlacement = !state.legal_actions.empty();
    std::unordered_set<size_t> mappedAttacks;
    std::unordered_set<size_t> mappedHandActions;
    std::unordered_set<size_t> mappedBoardActions;
    // MSG_SELECT_PLACE can ask for either a monster zone or a spell/trap zone
    // (e.g. Set Spell/Trap) — both use this same "Place in <kind> zone N" text.
    static const std::string monsterZonePrefix = "Place in monster zone ";
    static const std::string spellZonePrefix = "Place in spell/trap zone ";
    static const std::string attackPrefix = "Attack with ";
    static const std::array<std::string, 4> handVerbs = {"Summon ", "Special summon ", "Set monster ", "Set spell/trap "};
    static const std::string changePositionPrefix = "Change position ";
    static const std::string activatePrefix = "Activate ";

    for (size_t i = 0; i < state.legal_actions.size(); ++i) {
        const auto& action = state.legal_actions[i];
        const std::string& text = action.label;
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

        if (text == "Enter Battle Phase") { layout.battle_phase_action = static_cast<int>(i); continue; }
        if (text == "Main Phase 2") { layout.main_phase2_action = static_cast<int>(i); continue; }
        if (text == "End Phase") { layout.end_phase_action = static_cast<int>(i); continue; }

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
                // without this dedupe, N copies of a card produced N
                // identical "Summon"/"Set"/"Activate" buttons in its popup.
                auto& actions = layout.hand_actions[action.code];
                const bool alreadyHasVerb = std::any_of(actions.begin(), actions.end(), [&](size_t existing) {
                    return short_action_label(state.legal_actions[existing].label) == short_action_label(text);
                });
                if (!alreadyHasVerb) actions.push_back(i);
                mappedHandActions.insert(i);
            } else if (isChangePosition || isActivate) {
                // Two or more copies of the same monster/set card each publish
                // their own "Change position"/"Activate" legal action with
                // identical text — the engine's wire format for these only
                // ever names the *card*, never which physical zone. Without
                // skipping a zone that already has this exact action's text,
                // every copy's action matched the same first zone with that
                // code, showing N duplicate entries in that one zone's popup
                // while every other copy's zone showed none at all. The
                // engine publishes these lists (repositionable_cards for
                // Change Position; select_chains for Activate) in the same
                // order the matching zones are walked here, so skipping an
                // already-claimed zone naturally pairs the Nth published
                // action with the Nth zone that still needs one.
                auto alreadyHasVerb = [&](const std::vector<size_t>& actions) {
                    return std::any_of(actions.begin(), actions.end(), [&](size_t existing) {
                        return short_action_label(state.legal_actions[existing].label) == short_action_label(text);
                    });
                };
                bool matched = false;
                for (int slot = 0; slot < 5 && !matched; ++slot) {
                    const auto& card = state.monsters[0][static_cast<size_t>(slot)];
                    if (!card.occupied || card.code != action.code) continue;
                    auto& actions = layout.monster_board_actions[slot];
                    if (alreadyHasVerb(actions)) continue;
                    actions.push_back(i);
                    mappedBoardActions.insert(i);
                    matched = true;
                }
                for (int slot = 0; slot < 6 && !matched && isActivate; ++slot) {
                    const auto& card = state.spells[0][static_cast<size_t>(slot)];
                    if (!card.occupied || card.code != action.code) continue;
                    auto& actions = layout.spell_board_actions[slot];
                    if (alreadyHasVerb(actions)) continue;
                    actions.push_back(i);
                    mappedBoardActions.insert(i);
                    matched = true;
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

// ---------- engine IPC (unchanged protocol — see ProcessBridge.hpp for the
// cross-platform process-spawn abstraction this uses in place of
// src/client/main.cpp's direct CreateProcessW calls) ----------

#ifdef _WIN32
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

// A fresh random seed per duel so the shuffle (and therefore every opening
// hand and topdeck) differs game to game — the engine otherwise defaults to
// a fixed seed, which made every duel replay an identical deck order.
uint64_t random_duel_seed() {
    std::random_device rd;
    return (static_cast<uint64_t>(rd()) << 32) ^ static_cast<uint64_t>(rd());
}

// Maps an NPC's 1-5 difficulty rating (data/npcs.json's "difficulty" field,
// otherwise only used for the CPU-select screen's subtitle) onto the CPU
// agent's normal/hard tiers (src/ai/GoatAgent.hpp). Every NPC uses the
// "heuristic-goat" agent now, and deliberately never maps to "easy" — Easy
// is currently a full delegate to the old passive RandomAgent baseline
// (zero heuristics), which is exactly the "very dumb" behavior the roster
// moved away from; 4-5 gets Hard, everything else gets Normal.
const char* difficulty_flag_for(int difficulty) {
    return difficulty >= 4 ? "hard" : "normal";
}

// Strips the trailing " [image/path.jpg]" the engine appends to a choice
// label purely so the client can recover the card code (see
// parse_code_from_image_path) — never meant to be shown.
std::string action_caption(std::string text) {
    const auto image = text.find(" [");
    if (image != std::string::npos) text.erase(image);
    return text;
}

uint32_t parse_code_from_image_path(const std::string& path) {
    const auto stem = fs::path(path).stem().string();
    if (stem.empty()) return 0;
    for (char c : stem) if (!std::isdigit(static_cast<unsigned char>(c))) return 0;
    try { return static_cast<uint32_t>(std::stoul(stem)); } catch (...) { return 0; }
}

std::string run_automatic_duel() {
    const auto root = fs::current_path();
    const auto executable = root / "build" / (std::string("goat-sim") + kExeSuffix);
    if (!fs::exists(executable)) return "Engine executable is missing. Run scripts/build-smoke.sh first.";
    const std::vector<std::string> args = {
        "duel", "decks/vanilla-a.ydk", "decks/vanilla-b.ydk",
        "--seed", std::to_string(random_duel_seed()), "--max-turns", "100", "--quiet",
    };
    // Blocks the calling thread until the duel ends — same characteristic as
    // src/client/main.cpp's own WaitForSingleObject(..., INFINITE) here (see
    // ProcessBridge.hpp's run_and_capture doc-comment); not fixed, since
    // that's accepted behavior for this one smoke-test button, not a bug.
    const auto result = goat::process::run_and_capture(executable.string(), args, root.string());
    if (!result.started) return "Could not start the Project Ignis duel process.";
    if (result.exit_code != 0) return "Automatic duel failed:\n" + result.output.substr(result.output.rfind('\n') + 1);
    // "wins (reason" for a real winner, "draw (reason" for ygopro-core's
    // PLAYER_NONE case (see src/main.cpp's log_message) — a duel only ever
    // produces one or the other, never both, so whichever is found (if any)
    // is the final result line.
    auto winner = result.output.rfind("wins (reason");
    if (winner == std::string::npos) winner = result.output.rfind("draw (reason");
    if (winner == std::string::npos) return "Automatic duel finished without a final result message.";
    const auto line_start = result.output.rfind('\n', winner);
    const auto line_end = result.output.find('\n', winner);
    return "Automatic duel complete \xE2\x80\x94 " + result.output.substr(line_start == std::string::npos ? 0 : line_start + 1, line_end - line_start - 1);
}

void start_player_duel(AppState& state, bool test_mode) {
    if (state.player_process.valid()) return;
    const auto& npc = state.catalog.npcs.at(state.selected_npc % state.catalog.npcs.size());
    if (!test_mode) {
        try {
            // Re-validates deck legality client-side before even trying to
            // launch the engine (test_mode skips this).
            goat::CardDatabase database("external/BabelCDB/cards.cdb", "external/BabelCDB/goat-entries.cdb");
            const auto banlist = goat::Banlist::load("external/LFLists/GOAT.lflist.conf");
            goat::game::validate_player_deck(goat::game::read_deck(state.progression.profile().selected_deck), state.progression.profile(), database, banlist);
            goat::game::validate_npc_deck(goat::game::read_deck(npc.deck_path), database, banlist);
        } catch (const std::exception& error) {
            state.status = std::string("Deck cannot be used: ") + error.what();
            return;
        }
    }
    std::error_code ignored;
    fs::remove_all(state.session_directory, ignored); // wipe any stale files from a previous duel
    fs::create_directories(state.session_directory);
    const auto root = fs::current_path();
    const auto executable = root / "build" / (std::string("goat-sim") + kExeSuffix);
    if (!fs::exists(executable)) { state.status = "Duel engine is missing: " + executable.string(); return; }

    const std::vector<std::string> baseArgs = {
        "duel", state.progression.profile().selected_deck, npc.deck_path,
        "--human-player", "1",
        "--decision-dir", state.session_directory.string(),
        "--result-file", (state.session_directory / "result.txt").string(),
        "--seed", std::to_string(random_duel_seed()), "--quiet",
        // Honors this NPC's data-driven strength (data/npcs.json's "agent"
        // field — "random" or "goat", and its "difficulty" 1-5 rating mapped
        // to an AI difficulty tier); the human seat (player 1) is untouched
        // by either flag.
        "--agent2", npc.agent,
        "--difficulty", difficulty_flag_for(npc.difficulty),
    };
    std::vector<std::string> args = baseArgs;
    if (test_mode) args.push_back("--allow-illegal-deck");

    // The engine reports its actual failure reason (an uncaught exception's
    // .what(), e.g. "empty option-selection prompt" or "turn limit reached")
    // to stderr before exiting. Redirecting that to a log file here means
    // poll_player_duel can surface it instead of leaving the player with
    // only a generic "ended before reporting a result" and no way to tell
    // what went wrong (this is a windowed app with no console to see it on).
    const auto logPath = state.session_directory / "engine-log.txt";
    state.player_process = goat::process::spawn(executable.string(), args, root.string(), logPath.string());
    if (!state.player_process.valid()) { state.status = "Could not start the player-vs-CPU duel."; return; }

    state.duel_is_test_mode = test_mode;
    state.life = {8000, 8000}; state.life_display = {8000.0, 8000.0};
    state.hand_count = {}; state.deck_count = {}; state.grave_count = {}; state.extra_count = {}; state.banished_count = {};
    state.monsters = {}; state.spells = {}; state.hand_cards.clear();
    state.grave_cards = {}; state.banished_cards = {}; state.extra_cards.clear();
    state.viewing_pile_kind = PileKind::None; state.viewing_pile_player = -1; state.viewing_pile_selected = -1; state.viewing_pile_page = 0;
    state.turn_player = 0; state.turn_number = 0; state.phase = 0;
    state.legal_actions.clear(); state.action_layout = ActionLayout{}; state.action_page = 0; state.prompt_title.clear();
    state.open_hand_card = -1; state.selected_monster_zone = -1; state.selected_spell_zone = -1;
    state.multi_select_active = false; state.multi_select_toggled.clear();
    state.last_board_snapshot.clear();
    state.request_txt_seen_missing = true;
    state.last_request_text.clear();
    // Explicit resets for clarity/safety even though every path that ends a
    // duel already zeroes these before this point can be reached again
    // (paint_duel's tracking block resets waiting_since every frame
    // player_process is invalid; last_poll_time and last_board_change_time
    // are GetTime()-based and only ever compared as "how long since," so a
    // stale value from a previous duel can't cause a false reading here) —
    // but "explicitly reset all duel state at duel start" is the pattern
    // every other field on this list already follows.
    state.last_poll_time = 0.0;
    state.last_board_change_time = GetTime();
    state.waiting_since = 0.0;
    state.status = (test_mode ? "Test duel started against " : "Duel started against ") + npc.name +
        (test_mode ? " \xE2\x80\x94 no rewards, restrictions bypassed." : ". Waiting for the engine's legal actions.");
}

void poll_player_duel(AppState& state) {
    if (!state.player_process.valid()) return;
    const auto request = state.session_directory / "request.txt";
    // The engine (see choose_menu in src/main.cpp) writes request.txt once
    // per decision and only deletes it once it has consumed our response.txt
    // for that decision — a step that happens on its own poll, not
    // synchronously with our submit. Without a freshness check, a poll
    // landing in that gap would see the *same* already-answered request.txt
    // still on disk (legal_actions was already cleared by submit_action) and
    // re-ingest it as if it were new.
    //
    // Freshness used to be tracked via fs::last_write_time, but this
    // toolchain's implementation truncates to ~1-second granularity (verified
    // directly: several rewrites milliseconds apart all reported the
    // identical timestamp) — so any second prompt published within the same
    // wall-clock second as the previous one was silently and permanently
    // dropped. That's not a rare race; back-to-back decisions inside one
    // second are routine (right after Set-ing a card, or any time the CPU's
    // turn needs a human response with no real "thinking" delay in between).
    //
    // Instead: the engine always deletes request.txt before it can ever
    // publish a genuinely new one (see above), so "have we observed the file
    // absent since the prompt we last parsed" is a timestamp-free freshness
    // signal on its own. Content comparison is kept alongside it only to
    // catch the (much narrower) case of a poll landing exactly in the
    // sub-100ms gap between the engine deleting the old file and publishing
    // an identical-text new one, missing the transient "absent" moment
    // entirely.
    if (!fs::exists(request)) {
        state.request_txt_seen_missing = true;
    } else if (state.legal_actions.empty()) {
        std::ifstream input(request);
        const std::string text{std::istreambuf_iterator<char>(input), {}};
        const bool fresh = state.request_txt_seen_missing || text != state.last_request_text;
        if (fresh && !text.empty()) {
            state.request_txt_seen_missing = false;
            state.last_request_text = text;
            std::istringstream lines(text);
            std::string line;
            std::getline(lines, line);
            state.legal_actions.clear();
            state.multi_select_active = line.rfind("#SELECT ", 0) == 0;
            if (state.multi_select_active) {
                // "#SELECT <min> <max>" header, then the real title on its
                // own line, then one candidate per line (see choose_multi_menu
                // in src/main.cpp) — "<label> [<image>][<controller>,<location>,<sequence>]".
                std::istringstream header(line.substr(8));
                header >> state.multi_select_min >> state.multi_select_max;
                std::getline(lines, line);
                state.prompt_title = line;
                while (std::getline(lines, line)) {
                    const auto split = line.find('|');
                    if (split == std::string::npos) continue;
                    const auto raw = line.substr(split + 1);
                    const auto zoneOpen = raw.rfind('['), zoneClose = raw.rfind(']');
                    if (zoneOpen == std::string::npos || zoneClose == std::string::npos || zoneOpen >= zoneClose) continue;
                    const auto beforeZone = raw.substr(0, zoneOpen);
                    const auto imgOpen = beforeZone.rfind('['), imgClose = beforeZone.rfind(']');
                    LegalAction action;
                    if (imgOpen != std::string::npos && imgClose != std::string::npos && imgOpen < imgClose) {
                        action.image = beforeZone.substr(imgOpen + 1, imgClose - imgOpen - 1);
                        action.code = parse_code_from_image_path(action.image);
                        action.label = action_caption(beforeZone.substr(0, imgOpen));
                    } else {
                        action.label = action_caption(beforeZone);
                    }
                    const auto locationField = raw.substr(zoneOpen + 1, zoneClose - zoneOpen - 1);
                    uint32_t parts[3] = {0, 0, 0}; int partIndex = 0; size_t cursor = 0;
                    while (cursor <= locationField.size() && partIndex < 3) {
                        const auto comma = locationField.find(',', cursor);
                        const auto token = locationField.substr(cursor, comma == std::string::npos ? std::string::npos : comma - cursor);
                        try { parts[partIndex] = static_cast<uint32_t>(std::stoul(token)); } catch (...) {}
                        ++partIndex;
                        if (comma == std::string::npos) break;
                        cursor = comma + 1;
                    }
                    action.controller = static_cast<uint8_t>(parts[0]);
                    action.location = static_cast<uint8_t>(parts[1]);
                    action.sequence = parts[2];
                    state.legal_actions.push_back(std::move(action));
                }
                state.multi_select_toggled.assign(state.legal_actions.size(), false);
                state.action_layout = ActionLayout{};
            } else {
                state.prompt_title = line;
                while (std::getline(lines, line)) {
                    const auto split = line.find('|');
                    if (split == std::string::npos) continue;
                    const auto raw = line.substr(split + 1);
                    const auto begin = raw.find('['), end = raw.rfind(']');
                    LegalAction action;
                    if (begin != std::string::npos && end != std::string::npos && begin < end) {
                        action.image = raw.substr(begin + 1, end - begin - 1);
                        action.code = parse_code_from_image_path(action.image);
                    }
                    action.label = action_caption(raw);
                    state.legal_actions.push_back(std::move(action));
                }
            }
            state.action_page = 0;
        }
    }
    const auto result = state.session_directory / "result.txt";
    if (fs::exists(result)) {
        std::ifstream input(result); std::string winner; std::getline(input, winner);
        const bool player_won = winner == "winner=0";
        // ygopro-core's own PLAYER_NONE (see field::check_win_lose in
        // processor.cpp, e.g. both players deck out on the same draw, or a
        // simultaneous LP-to-zero neither side is immune to) writes MSG_WIN
        // with a winner byte of 2, not 0 or 1 — a genuine draw, distinct from
        // either player losing. Treating "not winner=0" as an automatic
        // defeat (the naive reading of this file) silently told the player
        // they lost a duel that was actually a draw; check for it explicitly.
        const bool is_draw = winner == "winner=2";
        const auto& npc = state.catalog.npcs.at(state.selected_npc % state.catalog.npcs.size());
        if (state.duel_is_test_mode) {
            state.status = player_won ? "Test duel complete \xE2\x80\x94 Victory! (no rewards, test mode)"
                : is_draw ? "Test duel complete \xE2\x80\x94 Draw. (no rewards, test mode)"
                : "Test duel complete \xE2\x80\x94 Defeat. (no penalty, test mode)";
        } else if (player_won) {
            state.progression.award_npc_victory(npc);
            goat::game::ProfileStore::save(state.progression.profile(), "saves/default.sav");
            state.status = "Victory! Earned " + std::to_string(npc.reward.credits) + " credits and a sealed pack.";
        } else if (is_draw) {
            state.status = "The duel ended in a draw. No rewards or penalty.";
        } else {
            state.status = "Defeat. Try a different line or strengthen your collection in the shop.";
        }
        goat::process::close_handles(state.player_process);
        state.legal_actions.clear(); state.action_layout = ActionLayout{}; state.prompt_title.clear();
        state.open_hand_card = -1; state.selected_monster_zone = -1; state.selected_spell_zone = -1;
        state.multi_select_active = false; state.multi_select_toggled.clear();
        return;
    }
    int exit_code = 0;
    if (goat::process::poll(state.player_process, exit_code) != goat::process::Status::Running) {
        state.status = "The duel process ended before reporting a result.";
        // Surface whatever the engine actually reported (its uncaught
        // exception's .what(), captured to engine-log.txt) instead of
        // leaving the player with only the generic message above.
        std::ifstream log(state.session_directory / "engine-log.txt");
        if (log) {
            std::string logText{std::istreambuf_iterator<char>(log), {}};
            while (!logText.empty() && (logText.back() == '\n' || logText.back() == '\r')) logText.pop_back();
            const auto lastLine = logText.find_last_of("\r\n");
            const auto reason = lastLine == std::string::npos ? logText : logText.substr(lastLine + 1);
            if (!reason.empty()) state.status += " (" + reason + ")";
        }
        goat::process::close_handles(state.player_process);
        state.legal_actions.clear(); state.action_layout = ActionLayout{}; state.prompt_title.clear();
        state.open_hand_card = -1; state.selected_monster_zone = -1; state.selected_spell_zone = -1;
    }
}

// Purely presentational, engine-originated audio for state changes that
// submit_action's own play_action_sfx can't see coming (mainly the CPU
// opponent's turn) — see the top-level task's board-diff section. Restricted
// to the *opponent's* zones (index 1) for summon/set specifically, so this
// can never double up with play_action_sfx's already-fired sound for the
// local player's own submitted action (index 0 is deliberately left
// untouched by those two branches). Destroy and LP-damage are symmetric
// across both players since neither is already covered by any
// play_action_sfx branch. Only ever called with a snapshot that's already
// confirmed to differ from the previous one (see poll_board_snapshot's own
// early-return above), so this never fires from re-polling unchanged state.
void emit_board_diff_sfx(AppState& state, const std::array<int, 2>& prevLife,
                          const std::array<std::array<FieldCard, 5>, 2>& prevMonsters,
                          const std::array<std::array<FieldCard, 6>, 2>& prevSpells) {
    using goat::audio::SoundEffect;
    for (int p = 0; p < 2; ++p) {
        if (state.life[p] < prevLife[p]) { state.audio.playSound(SoundEffect::LifePointDamage); break; }
    }
    for (int p = 0; p < 2; ++p) {
        for (size_t z = 0; z < prevMonsters[p].size(); ++z) {
            const bool was = prevMonsters[p][z].occupied, is = state.monsters[p][z].occupied;
            if (was && !is) state.audio.playSound(SoundEffect::Destroy);
            else if (!was && is && p == 1) {
                const bool faceDown = (state.monsters[p][z].position & POS_FACEDOWN) != 0;
                state.audio.playSound(faceDown ? SoundEffect::SetCard : SoundEffect::SummonMonster);
            }
        }
        for (size_t z = 0; z < prevSpells[p].size(); ++z) {
            const bool was = prevSpells[p][z].occupied, is = state.spells[p][z].occupied;
            if (was && !is) state.audio.playSound(SoundEffect::Destroy);
            else if (!was && is && p == 1) state.audio.playSound(SoundEffect::SetCard);
        }
    }
}

void poll_board_snapshot(AppState& state) {
    const auto filename = state.session_directory / "state.txt";
    std::ifstream input(filename); if (!input) return;
    const std::string text{std::istreambuf_iterator<char>(input), {}};
    if (text.empty() || text == state.last_board_snapshot) return;
    state.last_board_snapshot = text;
    state.last_board_change_time = GetTime();
    const std::array<int, 2> prevLife = state.life;
    const auto prevMonsters = state.monsters;
    const auto prevSpells = state.spells;
    for (auto& player : state.monsters) for (auto& cell : player) cell = {};
    for (auto& player : state.spells) for (auto& cell : player) cell = {};
    state.hand_cards.clear();
    for (auto& cards : state.grave_cards) cards.clear();
    for (auto& cards : state.banished_cards) cards.clear();
    state.extra_cards.clear();
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
        else if (key == "monster" && fields.size() == 6 && fields[0] < 2 && fields[1] < 5)
            state.monsters[fields[0]][fields[1]] = {true, fields[2], static_cast<uint8_t>(fields[3]), static_cast<int32_t>(fields[4]), static_cast<int32_t>(fields[5])};
        else if (key == "spell" && fields.size() == 4 && fields[0] < 2 && fields[1] < 6) state.spells[fields[0]][fields[1]] = {true, fields[2], static_cast<uint8_t>(fields[3])};
        else if (key == "handcards" && !fields.empty() && fields[0] < 2) state.hand_cards.assign(fields.begin() + 1, fields.end());
        else if (key == "gravecards" && !fields.empty() && fields[0] < 2) state.grave_cards[fields[0]].assign(fields.begin() + 1, fields.end());
        else if (key == "banishedcards" && !fields.empty() && fields[0] < 2) state.banished_cards[fields[0]].assign(fields.begin() + 1, fields.end());
        else if (key == "extracards" && !fields.empty() && fields[0] < 2) state.extra_cards.assign(fields.begin() + 1, fields.end());
    }
    emit_board_diff_sfx(state, prevLife, prevMonsters, prevSpells);
}

// Maps a just-submitted LegalAction's raw label (e.g. "Summon Protector of
// the Throne", "Set spell/trap Sakuretsu Armor") to the matching one-shot
// SFX — called from submit_action only after its response.txt write has
// actually gone through, so a click that never resolves into a committed
// choice never makes a sound (see submit_action's own early-return for an
// out-of-range index, and the top-level task's "don't play on debounce/
// failed clicks" requirement). No branch here for a verb this project has no
// distinct asset for (Activate, Change Position, Attack, ...) — see
// AudioManager.cpp's sound_path() for exactly which verbs are backed by a
// file; correctness (staying silent) beats guessing at a close-enough sound.
void play_action_sfx(AppState& state, const std::string& label) {
    using goat::audio::SoundEffect;
    auto starts_with = [&](const char* prefix) { return label.rfind(prefix, 0) == 0; };
    // Special Summon reuses the normal summon sting — external/sound/fx has
    // no dedicated special-summon asset, and "a monster arrived" reads as
    // the same underlying event either way.
    if (starts_with("Summon ") || starts_with("Special summon ")) state.audio.playSound(SoundEffect::SummonMonster);
    else if (starts_with("Set monster ") || starts_with("Set spell/trap ")) state.audio.playSound(SoundEffect::SetCard);
}

void submit_action(AppState& state, size_t action) {
    if (action >= state.legal_actions.size()) return;
    state.last_submit_time = GetTime();
    if (state.legal_actions[action].code) state.last_inspected_code = state.legal_actions[action].code;
    const std::string actionLabel = state.legal_actions[action].label; // legal_actions gets cleared below
    // The engine (choose_menu in src/main.cpp) polls for this file's
    // existence and reads it the instant it sees it. Writing directly to
    // response.txt left a window where the engine could observe the file
    // mid-write and read it empty, defaulting to an always-out-of-range
    // choice and killing the duel. Writing to a temp file and renaming
    // (matching how the engine itself publishes request.txt) makes the
    // response only ever appear on disk fully formed.
    const auto responsePath = state.session_directory / "response.txt";
    const auto tempPath = state.session_directory / "response.tmp";
    { std::ofstream output(tempPath, std::ios::trunc); output << action << '\n'; }
    std::error_code renameError;
    fs::rename(tempPath, responsePath, renameError);
    if (renameError) {
        // A transient rename failure here (this project's own history
        // documents antivirus scan-locking on rapid temp-file-rename
        // patterns as a real, observed cause in this exact environment)
        // would otherwise silently drop the player's response: everything
        // below still clears legal_actions as if it succeeded, so a lost
        // rename left the engine's choose_menu polling for a response.txt
        // that was never actually written — hanging for up to its own
        // 60-second timeout while the client shows "Waiting for the rules
        // engine…" the whole time. Retrying once after clearing whatever's
        // at the destination first mirrors the engine's own
        // write_board_state rename-fallback (src/main.cpp) and recovers
        // from exactly that.
        std::error_code removeError;
        fs::remove(responsePath, removeError);
        fs::rename(tempPath, responsePath, renameError);
    }
    if (!renameError) play_action_sfx(state, actionLabel);
    state.legal_actions.clear();
    state.action_layout = ActionLayout{};
    state.action_page = 0;
    state.prompt_title.clear();
    state.open_hand_card = -1; state.selected_monster_zone = -1; state.selected_spell_zone = -1;
    state.multi_select_active = false;
    state.multi_select_toggled.clear();
}

// Submits the toggled subset of a "#SELECT <min> <max>" prompt (see
// AppState::multi_select_active). Mirrors submit_action's atomic
// temp-file-then-rename write, but the payload is a comma-separated list of
// candidate indices (src/main.cpp's choose_multi_menu expects exactly this
// shape) instead of a single index.
void submit_multi_selection(AppState& state) {
    if (!state.multi_select_active) return;
    const size_t count = std::count(state.multi_select_toggled.begin(), state.multi_select_toggled.end(), true);
    if (count < state.multi_select_min || count > state.multi_select_max) return;
    state.last_submit_time = GetTime();
    const auto responsePath = state.session_directory / "response.txt";
    const auto tempPath = state.session_directory / "response.tmp";
    {
        std::ofstream output(tempPath, std::ios::trunc);
        bool first = true;
        for (size_t i = 0; i < state.multi_select_toggled.size(); ++i) {
            if (!state.multi_select_toggled[i]) continue;
            if (!first) output << ',';
            output << i;
            first = false;
        }
        output << '\n';
    }
    std::error_code renameError;
    fs::rename(tempPath, responsePath, renameError);
    if (renameError) {
        // See submit_action's comment on the same retry pattern.
        std::error_code removeError;
        fs::remove(responsePath, removeError);
        fs::rename(tempPath, responsePath, renameError);
    }
    state.legal_actions.clear();
    state.multi_select_active = false;
    state.multi_select_toggled.clear();
    state.action_layout = ActionLayout{};
    state.action_page = 0;
    state.prompt_title.clear();
    state.open_hand_card = -1; state.selected_monster_zone = -1; state.selected_spell_zone = -1;
}

void paint_title(AppState& state, const Rect& client, const UiScale& scale, Vector2 mouse, bool clicked) {
    const Texture2D background = state.textures.get("external/extra/titlescreen.png");
    if (background.id != 0) draw_bitmap_cover(background, client);

    // Big hero title + intro paragraph, cut from the top of the client area.
    Rect textArea = inset(client, scale.px(28));
    cut_top(textArea, scale.px(kHeaderHeight));
    Rect titleRect = cut_top(textArea, scale.px(120));
    draw_text(state.font, "PROJECT GOAT", titleRect, static_cast<float>(scale.points(40)), theme::gold, {HAlign::Center, VAlign::Center, true, false});
    cut_top(textArea, scale.px(10));
    Rect introRect = cut_top(textArea, scale.px(70));
    draw_text(state.font, "Build a collection, challenge GOAT-format NPCs, and duel with Project Ignis rules.",
              introRect, static_cast<float>(scale.points(15)), theme::textPrimary, {HAlign::Center, VAlign::Top, true, false});

    // The CTA button lives in the bottom 35% of a *separate* cut sequence
    // from the same client rect (matching src/client/main.cpp's
    // compute_title_layout, which does the same two-independent-cuts thing)
    // rather than continuing to cut the area already used for the text above.
    Rect ctaArea = inset(client, scale.px(28));
    cut_top(ctaArea, scale.px(kHeaderHeight));
    Rect bottom = cut_bottom(ctaArea, static_cast<int>(ctaArea.height() * 0.35));
    Rect cta = centered(bottom, std::min(bottom.width(), scale.px(320)), scale.px(76));
    if (button(state.font, cta, "ENTER CAMPAIGN", "Start your journey", scale, mouse, clicked)) {
        state.screen = Screen::Hub;
        state.status = "Choose a duel, visit the shop, or view your collection.";
    }

    // Title doesn't call draw_app_header (it has its own hero layout, no
    // brand-header band) so its OPTIONS entry point is its own small button
    // in the same top-right corner draw_app_header would otherwise put one,
    // built from a separate independent cut of `client` (matching this
    // function's own cta/textArea split above) rather than disturbing the
    // hero text layout.
    Rect cornerArea = inset(client, scale.px(28));
    Rect cornerRow = cut_top(cornerArea, scale.px(34));
    Rect optionsButton = cut_right(cornerRow, scale.px(96));
    if (button(state.font, optionsButton, "OPTIONS", nullptr, scale, mouse, clicked)) {
        state.options_return_screen = Screen::Title;
        state.screen = Screen::Options;
    }
}

void paint_hub(AppState& state, const Rect& client, const UiScale& scale, Vector2 mouse, bool clicked) {
    draw_app_header(state, client, scale, mouse, clicked);
    Rect area = inset(client, scale.px(28));
    cut_top(area, scale.px(kHeaderHeight));
    cut_top(area, scale.px(14));

    Rect status = cut_bottom(area, scale.px(40));
    Rect navRow = cut_top(area, scale.px(84));
    cut_top(area, scale.px(14));
    Rect selectRow = cut_top(area, scale.px(84));

    static const char* navTitles[4] = {"AUTO DUEL", "PLAY CPU", "SHOP", "COLLECTION"};
    static const char* navSubs[4] = {"Watch CPU vs CPU", "Choose an opponent", "Buy GOAT packs", "Browse owned cards"};
    const int navGap = scale.px(14);
    const int navWidth = (navRow.width() - navGap * 3) / 4;
    for (int i = 0; i < 4; ++i) {
        const int x = navRow.left + i * (navWidth + navGap);
        const Rect navButton{x, navRow.top, x + navWidth, navRow.bottom};
        if (button(state.font, navButton, navTitles[i], navSubs[i], scale, mouse, clicked)) {
            if (i == 0) {
                // Blocks this thread until the CPU-vs-CPU duel finishes —
                // see run_automatic_duel's own doc-comment for why that's
                // accepted here rather than fixed. It has no board of its
                // own; the whole result is just this status-line string,
                // matching the Win32 client's own Hub-only smoke-test button.
                state.status = run_automatic_duel();
            } else if (i == 1) {
                state.screen = Screen::CpuSelect;
                state.cpu_select_test_mode = false;
                state.cpu_select_page = 0;
            } else if (i == 2) {
                state.screen = Screen::Shop;
            } else if (i == 3) {
                state.screen = Screen::Collection;
                state.collection_page = 0;
                state.search_text.clear();
                state.search_active = false;
            }
        }
    }

    const int selGap = scale.px(14);
    const int selWidth = (selectRow.width() - selGap) / 2;
    const Rect editDeckButton{selectRow.left, selectRow.top, selectRow.left + selWidth, selectRow.bottom};
    const Rect testDuelButton{selectRow.right - selWidth, selectRow.top, selectRow.right, selectRow.bottom};
    const std::string deckName = fs::path(state.progression.profile().selected_deck).stem().string();
    if (button(state.font, editDeckButton, "EDIT DECK", deckName.c_str(), scale, mouse, clicked)) {
        open_deck_editor(state, state.progression.profile().selected_deck);
    }
    if (button(state.font, testDuelButton, "TEST DUEL", "Any opponent, no rewards", scale, mouse, clicked)) {
        state.screen = Screen::CpuSelect;
        state.cpu_select_test_mode = true;
        state.cpu_select_page = 0;
    }

    draw_text(state.font, state.status, status, static_cast<float>(scale.points(13)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});
}

void paint_collection(AppState& state, const Rect& client, const UiScale& scale, Vector2 mouse, bool clicked) {
    draw_app_header(state, client, scale, mouse, clicked);
    const auto& collection = state.progression.profile().collection;
    const std::vector<uint32_t> visible = filtered_collection(state.card_database, collection, state.search_text,
                                                                state.collection_filter_monster, state.collection_filter_spell, state.collection_filter_trap);
    const auto L = compute_collection_layout(client, scale, visible.size());
    state.collection_page = std::min(state.collection_page, L.grid.page_count - 1);

    draw_text(state.font, "YOUR COLLECTION", L.header, static_cast<float>(scale.points(20)), theme::gold, {HAlign::Left, VAlign::Center, false, false});

    text_field(state.font, L.search_box, state.search_text, state.search_active, scale, mouse, clicked);
    if (toggle_chip(state.font, L.filters[0], "Monster", state.collection_filter_monster, scale, mouse, clicked)) state.collection_filter_monster = !state.collection_filter_monster;
    if (toggle_chip(state.font, L.filters[1], "Spell", state.collection_filter_spell, scale, mouse, clicked)) state.collection_filter_spell = !state.collection_filter_spell;
    if (toggle_chip(state.font, L.filters[2], "Trap", state.collection_filter_trap, scale, mouse, clicked)) state.collection_filter_trap = !state.collection_filter_trap;

    const size_t first = state.collection_page * L.grid.per_page;
    for (size_t slot = 0; slot < L.grid.cards.size() && first + slot < visible.size(); ++slot) {
        const uint32_t code = visible[first + slot];
        const Rect& r = L.grid.cards[slot];
        const bool selected = state.browse_selected_code == code;
        const bool hovered = r.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
        draw_panel(r, Color{24, 24, 30, 255}, true, selected ? theme::gold : (hovered ? theme::legal : theme::panelBorder));
        draw_bitmap_fit(get_card_texture(state, code), r);
        if (hovered && clicked) state.browse_selected_code = code;
        const Rect labelRect{r.left, r.bottom + scale.px(4), r.right, r.bottom + scale.px(28)};
        const auto& card = state.card_database.resolve(code);
        const std::string label = card.name + " x" + std::to_string(collection.at(code));
        draw_text(state.font, label, labelRect, static_cast<float>(scale.points(11)), theme::textPrimary, {HAlign::Center, VAlign::Top, true, false});
    }
    if (visible.empty()) {
        draw_text(state.font, collection.empty() ? "Your collection is empty." : "No cards match this search/filter.",
                   L.grid_area, static_cast<float>(scale.points(13)), theme::textSecondary, {HAlign::Center, VAlign::Center, false, false});
    }

    draw_panel(L.detail, theme::panel, true, theme::panelBorder);
    draw_card_detail(state, inset(L.detail, scale.px(12)), state.browse_selected_code, scale);

    // Passing an off-screen mouse position when a page button is disabled
    // suppresses its hover highlight too (not just the click), matching
    // src/client/main.cpp's `state.collection_page > 0 && rect.contains(...)`
    // hover condition for the same buttons.
    const Vector2 offscreen{-1, -1};
    if (button(state.font, L.buttons[0], "\xE2\x80\xB9 PREV", nullptr, scale, state.collection_page > 0 ? mouse : offscreen, clicked)) --state.collection_page;
    const std::string pageLabel = "PAGE " + std::to_string(state.collection_page + 1) + "/" + std::to_string(L.grid.page_count);
    if (button(state.font, L.buttons[1], pageLabel.c_str(), "Next page", scale, state.collection_page + 1 < L.grid.page_count ? mouse : offscreen, clicked)) ++state.collection_page;
    if (button(state.font, L.buttons[2], "BACK", "Campaign hub", scale, mouse, clicked)) { state.screen = Screen::Hub; state.search_active = false; }

    draw_text(state.font, state.status, L.status, static_cast<float>(scale.points(13)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});
}

void paint_shop(AppState& state, const Rect& client, const UiScale& scale, Vector2 mouse, bool clicked) {
    draw_app_header(state, client, scale, mouse, clicked);
    const auto& packs = state.catalog.packs;
    const auto L = compute_shop_layout(client, scale, packs.size());
    const std::string headerText = "CARD SHOP \xE2\x80\x94 " + std::to_string(state.progression.profile().credits) + " credits";
    draw_text(state.font, headerText, L.header, static_cast<float>(scale.points(20)), theme::gold, {HAlign::Left, VAlign::Center, false, false});

    const auto& profile = state.progression.profile();
    const int unlockedTier = highest_unlocked_tier(state.catalog, profile);
    for (size_t i = 0; i < packs.size() && i < L.grid.cards.size(); ++i) {
        const auto& pack = packs[i];
        const Rect& r = L.grid.cards[i];
        const bool locked = pack.required_tier > unlockedTier;
        const bool selected = state.selected_pack == i;
        const bool hovered = r.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
        draw_panel(r, Color{24, 24, 30, 255}, true, selected ? theme::gold : (hovered ? theme::legal : theme::panelBorder));
        draw_bitmap_fit(state.textures.get("external/packart/" + pack.art), r);
        if (locked) draw_text(state.font, "LOCKED", r, static_cast<float>(scale.points(12)), theme::danger, {HAlign::Center, VAlign::Center, false, false});
        if (hovered && clicked) state.selected_pack = i;
        const Rect labelRect{r.left, r.bottom + scale.px(4), r.right, r.bottom + scale.px(30)};
        const auto ownedIt = profile.sealed_packs.find(pack.id);
        const int owned = ownedIt != profile.sealed_packs.end() ? ownedIt->second : 0;
        std::string label = pack.name;
        if (owned > 0) label += " (" + std::to_string(owned) + " sealed)";
        draw_text(state.font, label, labelRect, static_cast<float>(scale.points(11)), theme::textPrimary, {HAlign::Center, VAlign::Top, true, false});
    }
    if (packs.empty()) {
        draw_text(state.font, "No packs available.", L.grid_area, static_cast<float>(scale.points(13)), theme::textSecondary, {HAlign::Center, VAlign::Center, false, false});
    }

    draw_panel(L.detail, theme::panel, true, theme::panelBorder);
    if (!packs.empty()) {
        const auto& pack = packs[state.selected_pack % packs.size()];
        const bool packLocked = pack.required_tier > unlockedTier;
        Rect inner = inset(L.detail, scale.px(12));
        std::string info = pack.name + "\n\n" + std::to_string(pack.cards_per_pack) + " cards per pack\n" + std::to_string(pack.price) + " credits";
        if (packLocked) info += "\n\nLocked \xE2\x80\x94 clear every Tier " + std::to_string(pack.required_tier - 1) + " opponent 10 times to unlock purchase.";
        draw_text(state.font, info, inner, static_cast<float>(scale.points(14)), theme::textPrimary, {HAlign::Left, VAlign::Top, true, false});

        // Unlike button()'s usual "one hovered flag drives both the draw and
        // the click", a locked Buy button still needs its click to register
        // (to report *why* it's locked) even though its hover highlight is
        // suppressed — so this is drawn and hit-tested separately rather
        // than through button(), matching src/client/main.cpp's own split
        // between draw_button's hover flag and the click handler's own,
        // unconditional L.buttons[0].contains(x, y) check.
        const bool buyHovered = L.buttons[0].contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
        draw_button(state.font, L.buttons[0], packLocked ? "LOCKED" : "BUY PACK", packLocked ? "Requires a higher tier" : "Opens immediately", scale, !packLocked && buyHovered);
        if (buyHovered && clicked) {
            if (packLocked) {
                state.status = "Locked \xE2\x80\x94 clear every Tier " + std::to_string(pack.required_tier - 1) + " opponent 10 times to unlock " + pack.name + ".";
            } else if (!state.progression.buy_pack(pack)) {
                state.status = "Not enough credits for " + pack.name + ".";
            } else {
                state.opening_cards = state.progression.open_pack(pack, state.random);
                goat::game::ProfileStore::save(state.progression.profile(), "saves/default.sav");
                state.opening_revealed = 0;
                state.opening_reveal_start_time = GetTime();
                state.opening_selected = -1;
                state.status = "Opened " + pack.name + ".";
                state.screen = Screen::PackOpening;
                state.audio.playSound(goat::audio::SoundEffect::PackOpen);
            }
        }
    }
    if (button(state.font, L.buttons[1], "BACK", "Campaign hub", scale, mouse, clicked)) state.screen = Screen::Hub;
    draw_text(state.font, state.status, L.status, static_cast<float>(scale.points(13)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});
}

// Seconds between each card starting its reveal flip, and how long that flip
// itself takes — tuned to roughly match src/client/main.cpp's 200ms flip
// duration. Unlike that client (whose WM_TIMER-driven `opening_reveal_tick`
// is set on every pack purchase but never actually read anywhere, so
// `opening_revealed` there only ever jumps straight from 0 to "all" via the
// REVEAL ALL button — the intended one-at-a-time animation is effectively
// unreachable), this client's per-frame loop can just check elapsed time
// every frame, so the progressive reveal here actually runs.
constexpr double kRevealIntervalSeconds = 0.5;
constexpr double kFlipDurationSeconds = 0.2;

void paint_pack_opening(AppState& state, const Rect& client, const UiScale& scale, Vector2 mouse, bool clicked) {
    draw_app_header(state, client, scale, mouse, clicked);
    const auto L = compute_pack_opening_layout(client, scale, state.opening_cards.size());
    draw_text(state.font, "PACK OPENING", L.header, static_cast<float>(scale.points(20)), theme::gold, {HAlign::Left, VAlign::Center, false, false});

    const double now = GetTime();
    if (state.opening_revealed < state.opening_cards.size() && now - state.opening_reveal_start_time >= kRevealIntervalSeconds) {
        ++state.opening_revealed;
        state.opening_reveal_start_time = now;
        // Only the natural one-at-a-time reveal plays this — the REVEAL ALL
        // button below jumps opening_revealed straight to the end in one
        // step, and firing this once per skipped card there would be an
        // instant burst of overlapping sound, not a reveal.
        state.audio.playSound(goat::audio::SoundEffect::CardReveal);
    }

    const Texture2D cardBack = state.textures.get("external/extra/card_back.jpg");
    for (size_t i = 0; i < state.opening_cards.size() && i < L.grid.cards.size(); ++i) {
        const Rect& r = L.grid.cards[i];
        const bool revealed = i < state.opening_revealed;
        const bool selected = revealed && state.opening_selected == static_cast<int>(i);
        const bool hovered = revealed && r.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
        draw_panel(r, Color{24, 24, 30, 255}, true, selected ? theme::gold : (hovered ? theme::legal : theme::panelBorder));
        if (!revealed) { draw_bitmap_fit(cardBack, r); continue; }

        // The just-revealed card (index == opening_revealed-1) plays a short
        // width-collapse "flip": its tile narrows from full width to 0 and
        // back out, swapping from card-back to face art at the midpoint —
        // every earlier card is simply drawn at full width already revealed.
        Rect drawRect = r;
        bool showBack = false;
        if (i + 1 == state.opening_revealed) {
            const double elapsed = now - state.opening_reveal_start_time;
            if (elapsed < kFlipDurationSeconds) {
                const double t = elapsed / kFlipDurationSeconds; // 0..1
                const double widthFactor = std::abs(1.0 - 2.0 * t); // 1 -> 0 -> 1
                const int width = std::max(1, static_cast<int>(r.width() * widthFactor));
                drawRect = {r.center_x() - width / 2, r.top, r.center_x() - width / 2 + width, r.bottom};
                showBack = t < 0.5;
            }
        }
        draw_bitmap_fit(showBack ? cardBack : get_card_texture(state, state.opening_cards[i]), drawRect);
        if (hovered && clicked) state.opening_selected = static_cast<int>(i);
    }

    draw_panel(L.detail, theme::panel, true, theme::panelBorder);
    Rect inner = inset(L.detail, scale.px(12));
    if (state.opening_selected >= 0 && static_cast<size_t>(state.opening_selected) < state.opening_cards.size()) {
        draw_card_detail(state, inner, state.opening_cards[static_cast<size_t>(state.opening_selected)], scale);
    } else {
        draw_text(state.font, "Click a revealed card to inspect it.", inner, static_cast<float>(scale.points(11)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});
    }

    const bool allRevealed = state.opening_revealed >= state.opening_cards.size();
    const Vector2 offscreen{-1, -1};
    if (button(state.font, L.reveal_all, allRevealed ? "ALL REVEALED" : "REVEAL ALL", allRevealed ? "" : "Skip the animation", scale, allRevealed ? offscreen : mouse, clicked)) {
        state.opening_revealed = state.opening_cards.size();
    }
    if (button(state.font, L.done, "DONE", "Back to the shop", scale, mouse, clicked)) {
        state.screen = Screen::Shop;
        state.status = "Cards added to your collection.";
    }
    draw_text(state.font, state.status, L.status, static_cast<float>(scale.points(13)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});
}

void paint_deck_editor(AppState& state, const Rect& client, const UiScale& scale, Vector2 mouse, bool clicked) {
    const bool rightClicked = IsMouseButtonReleased(MOUSE_BUTTON_RIGHT);
    draw_app_header(state, client, scale, mouse, clicked);
    const auto& collection = state.progression.profile().collection;
    const std::vector<uint32_t> pool = filtered_collection(state.card_database, collection, state.search_text,
                                                             state.deck_filter_monster, state.deck_filter_spell, state.deck_filter_trap);
    const auto L = compute_deck_editor_layout(client, scale, pool.size());
    state.deck_pool_page = std::min(state.deck_pool_page, L.pool_grid.page_count - 1);

    draw_text(state.font, "DECK EDITOR \xE2\x80\x94 ADD CARDS   (left-click: inspect \xE2\x80\xA2 right-click: add)", L.header, static_cast<float>(scale.points(20)), theme::gold, {HAlign::Left, VAlign::Center, false, false});

    // The search box doubles as the deck-naming field while Save As is in
    // progress — same physical box, different backing text/focus state, one
    // frame's worth of clicks can only ever land on whichever is currently
    // drawn there. Mirrors src/client/main.cpp's single shared native EDIT
    // control, just without its Deck-List-screen blind spot (see
    // compute_deck_list_layout's name_box comment).
    if (state.deck_save_as_active) {
        text_field(state.font, L.search_box, state.deck_new_name, state.deck_naming_active, scale, mouse, clicked);
    } else {
        text_field(state.font, L.search_box, state.search_text, state.search_active, scale, mouse, clicked);
    }
    if (toggle_chip(state.font, L.filters[0], "Monster", state.deck_filter_monster, scale, mouse, clicked)) state.deck_filter_monster = !state.deck_filter_monster;
    if (toggle_chip(state.font, L.filters[1], "Spell", state.deck_filter_spell, scale, mouse, clicked)) state.deck_filter_spell = !state.deck_filter_spell;
    if (toggle_chip(state.font, L.filters[2], "Trap", state.deck_filter_trap, scale, mouse, clicked)) state.deck_filter_trap = !state.deck_filter_trap;

    const size_t first = state.deck_pool_page * L.pool_grid.per_page;
    for (size_t slot = 0; slot < L.pool_grid.cards.size() && first + slot < pool.size(); ++slot) {
        const uint32_t code = pool[first + slot];
        const Rect& r = L.pool_grid.cards[slot];
        const bool selected = state.browse_selected_code == code;
        const bool hovered = r.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
        draw_panel(r, Color{24, 24, 30, 255}, true, selected ? theme::gold : (hovered ? theme::legal : theme::panelBorder));
        draw_bitmap_fit(get_card_texture(state, code), r);
        const int owned = collection.at(code);
        const int inDeck = static_cast<int>(std::count(state.editing_deck.main.begin(), state.editing_deck.main.end(), code)) +
                            static_cast<int>(std::count(state.editing_deck.extra.begin(), state.editing_deck.extra.end(), code));
        const Rect labelRect{r.left, r.bottom + scale.px(4), r.right, r.bottom + scale.px(28)};
        const auto& card = state.card_database.resolve(code);
        const std::string label = card.name + " (" + std::to_string(inDeck) + "/" + std::to_string(owned) + ")";
        draw_text(state.font, label, labelRect, static_cast<float>(scale.points(11)), theme::textPrimary, {HAlign::Center, VAlign::Top, true, false});

        // Left-click only selects the card for the detail panel — it used to
        // also add a copy to the deck, but that made simply browsing/
        // inspecting the pool add cards as a side effect. Adding is now a
        // separate, deliberate right-click, gated the same way (suspended
        // while naming a Save-As copy — every other control on this screen,
        // filters/pager/the six action buttons, stays live either way,
        // matching src/client/main.cpp's own gating for the equivalent
        // single-click case).
        if (hovered && clicked) {
            state.browse_selected_code = code;
        }
        if (hovered && rightClicked && !state.deck_save_as_active) {
            state.browse_selected_code = code;
            if (inDeck >= owned) {
                state.status = "You don't own another copy of that card.";
            } else {
                const auto& def = state.card_database.resolve(code);
                const bool isExtraCard = (def.type & (TYPE_FUSION | TYPE_RITUAL)) != 0;
                auto& list = isExtraCard ? state.editing_deck.extra : state.editing_deck.main;
                const size_t cap = isExtraCard ? 15 : 60;
                if (list.size() >= cap) {
                    state.status = isExtraCard ? "Extra deck is full (15)." : "Main deck is full (60).";
                } else {
                    list.push_back(code);
                    state.deck_editor_dirty = true;
                    recompute_deck_status(state);
                }
            }
        }
    }
    if (pool.empty()) {
        draw_text(state.font, "No owned cards match this search/filter.", L.pool_area, static_cast<float>(scale.points(13)), theme::textSecondary, {HAlign::Center, VAlign::Center, false, false});
    }

    const Vector2 offscreen{-1, -1};
    if (button(state.font, L.pool_prev, "\xE2\x80\xB9 PREV", nullptr, scale, state.deck_pool_page > 0 ? mouse : offscreen, clicked)) --state.deck_pool_page;
    const std::string pageLabel = "PAGE " + std::to_string(state.deck_pool_page + 1) + "/" + std::to_string(L.pool_grid.page_count);
    draw_text(state.font, pageLabel, L.pool_page_label, static_cast<float>(scale.points(12)), theme::textSecondary, {HAlign::Center, VAlign::Center, false, false});
    if (button(state.font, L.pool_next, "NEXT \xE2\x80\xBA", nullptr, scale, state.deck_pool_page + 1 < L.pool_grid.page_count ? mouse : offscreen, clicked)) ++state.deck_pool_page;

    draw_panel(L.detail, theme::panel, true, theme::panelBorder);
    draw_card_detail(state, inset(L.detail, scale.px(12)), state.browse_selected_code, scale);

    deck_action_buttons(state, L.buttons, scale, mouse, clicked);
    if (button(state.font, L.buttons[4], "VIEW DECKLIST", "See main + extra deck", scale, mouse, clicked)) state.screen = Screen::DeckList;

    draw_text(state.font, state.deck_status + "   \xE2\x80\x94   " + state.status, L.status, static_cast<float>(scale.points(13)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});
}

// One unique-code+count row in the Deck List's Main or Extra panel.
struct DeckRow { uint32_t code; int count; };
std::vector<DeckRow> deck_rows(const std::vector<uint32_t>& list) {
    std::map<uint32_t, int> counts;
    for (const auto code : list) ++counts[code];
    std::vector<DeckRow> rows;
    rows.reserve(counts.size());
    for (const auto& [code, count] : counts) rows.push_back({code, count});
    return rows;
}

// Draws one Main- or Extra-deck panel and, on a row click (while not naming
// a Save-As copy), removes exactly one copy of that row's card. Shared by
// both panels in paint_deck_list, mirroring src/client/main.cpp's
// draw_deck_list_panel + the removal half of handle_deck_list_click's
// try_remove (folded together here since this client's per-frame loop lets
// a click be detected and acted on in the same pass that draws the row).
void deck_list_panel(AppState& state, const Rect& panel, const ColumnListLayout& columns, const std::vector<DeckRow>& rows,
                      const std::string& headerText, const UiScale& scale, Vector2 mouse, bool clicked, std::vector<uint32_t>& list) {
    draw_panel(panel, theme::panel, true, theme::panelBorder);
    Rect header = inset(panel, scale.px(12));
    header.bottom = header.top + scale.px(22);
    draw_text(state.font, headerText, header, static_cast<float>(scale.points(13)), theme::gold, {HAlign::Left, VAlign::Center, false, false});
    for (size_t i = 0; i < rows.size() && i < columns.rows.size(); ++i) {
        const Rect& r = columns.rows[i];
        if (r.empty()) continue;
        const bool hovered = r.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
        if (hovered) draw_panel(r, Color{45, 84, 112, 255}, false, {});
        const auto& def = state.card_database.resolve(rows[i].code);
        const std::string label = def.name + " x" + std::to_string(rows[i].count);
        draw_text(state.font, label, r, static_cast<float>(scale.points(12)), theme::textPrimary, {HAlign::Left, VAlign::Center, false, true});
        if (hovered && clicked && !state.deck_save_as_active) {
            const auto it = std::find(list.begin(), list.end(), rows[i].code);
            if (it != list.end()) list.erase(it);
            state.browse_selected_code = rows[i].code;
            state.deck_editor_dirty = true;
            recompute_deck_status(state);
        }
    }
    if (rows.empty()) draw_text(state.font, "(empty)", panel, static_cast<float>(scale.points(11)), theme::textSecondary, {HAlign::Left, VAlign::Top, false, false});
}

void paint_deck_list(AppState& state, const Rect& client, const UiScale& scale, Vector2 mouse, bool clicked) {
    draw_app_header(state, client, scale, mouse, clicked);
    const auto mainRows = deck_rows(state.editing_deck.main);
    const auto extraRows = deck_rows(state.editing_deck.extra);
    const auto L = compute_deck_list_layout(client, scale, mainRows.size(), extraRows.size());

    draw_text(state.font, "DECK LIST", L.header, static_cast<float>(scale.points(20)), theme::gold, {HAlign::Left, VAlign::Center, false, false});
    if (state.deck_save_as_active) {
        text_field(state.font, L.name_box, state.deck_new_name, state.deck_naming_active, scale, mouse, clicked);
    }

    deck_list_panel(state, L.main_panel, L.main_columns, mainRows, "MAIN DECK (" + std::to_string(state.editing_deck.main.size()) + "/60)", scale, mouse, clicked, state.editing_deck.main);
    deck_list_panel(state, L.extra_panel, L.extra_columns, extraRows, "EXTRA DECK (" + std::to_string(state.editing_deck.extra.size()) + "/15)", scale, mouse, clicked, state.editing_deck.extra);

    deck_action_buttons(state, L.buttons, scale, mouse, clicked);
    if (button(state.font, L.buttons[4], "ADD CARDS", "Browse your collection", scale, mouse, clicked)) state.screen = Screen::DeckEditor;

    draw_text(state.font, state.deck_status + "   \xE2\x80\x94   " + state.status, L.status, static_cast<float>(scale.points(13)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});
}

void paint_cpu_select(AppState& state, const Rect& client, const UiScale& scale, Vector2 mouse, bool clicked) {
    draw_app_header(state, client, scale, mouse, clicked);
    const auto L = compute_cpu_select_layout(client, scale, state.catalog.npcs.size());
    state.cpu_select_page = std::min(state.cpu_select_page, L.page_count - 1);
    draw_text(state.font, state.cpu_select_test_mode ? "TEST DUEL \xE2\x80\x94 choose any opponent" : "PLAY CPU \xE2\x80\x94 choose an opponent",
              L.header, static_cast<float>(scale.points(20)), theme::gold, {HAlign::Left, VAlign::Center, false, false});

    const int unlockedTier = highest_unlocked_tier(state.catalog, state.progression.profile());
    const auto& profile = state.progression.profile();
    const size_t first = state.cpu_select_page * L.per_page;
    for (size_t slot = 0; slot < L.rows.size() && first + slot < state.catalog.npcs.size(); ++slot) {
        const size_t i = first + slot;
        const auto& npc = state.catalog.npcs[i];
        const bool locked = !state.cpu_select_test_mode && npc.tier > unlockedTier;
        const Rect& row = L.rows[slot];
        const bool hovered = !locked && row.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
        draw_panel(row, locked ? Color{16, 20, 26, 255} : (hovered ? Color{45, 84, 112, 255} : theme::panel), true,
                   locked ? theme::panelBorder : (hovered ? theme::gold : theme::panelBorder));
        Rect inner = inset(row, scale.px(14), scale.px(8));
        Rect nameRect = cut_top(inner, scale.px(24));
        const std::string nameLine = npc.name + (locked ? "  \xE2\x80\x94  LOCKED" : "");
        draw_text(state.font, nameLine, nameRect, static_cast<float>(scale.points(16)), locked ? theme::textSecondary : theme::gold, {HAlign::Left, VAlign::Center, false, false});
        std::string subtitle;
        if (locked) {
            subtitle = "Clear every Tier " + std::to_string(npc.tier - 1) + " opponent 10 times to unlock this opponent.";
        } else {
            const auto winsIt = profile.npc_wins.find(npc.id);
            const int wins = winsIt != profile.npc_wins.end() ? winsIt->second : 0;
            subtitle = "Difficulty " + std::to_string(npc.difficulty) + " \xE2\x80\xA2 Tier " + std::to_string(npc.tier) + " \xE2\x80\xA2 " +
                fs::path(npc.deck_path).stem().string();
            subtitle += state.cpu_select_test_mode ? " \xE2\x80\xA2 test mode: no rewards" : (" \xE2\x80\xA2 " + std::to_string(std::min(wins, 10)) + "/10 wins");
        }
        draw_text(state.font, subtitle, inner, static_cast<float>(scale.points(11)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});

        if (hovered && clicked) {
            state.selected_npc = i;
            state.screen = Screen::Duel;
            start_player_duel(state, state.cpu_select_test_mode);
        }
    }

    const Vector2 offscreen{-1, -1};
    if (button(state.font, L.buttons[0], "\xE2\x80\xB9 PREV", nullptr, scale, state.cpu_select_page > 0 ? mouse : offscreen, clicked)) --state.cpu_select_page;
    const std::string pageLabel = "PAGE " + std::to_string(state.cpu_select_page + 1) + "/" + std::to_string(L.page_count);
    if (button(state.font, L.buttons[1], pageLabel.c_str(), "Next page", scale, state.cpu_select_page + 1 < L.page_count ? mouse : offscreen, clicked)) ++state.cpu_select_page;
    if (button(state.font, L.buttons[2], "BACK", "Campaign hub", scale, mouse, clicked)) state.screen = Screen::Hub;
    draw_text(state.font, state.status, L.status, static_cast<float>(scale.points(13)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});
}

// ---------- Options ----------

// One [-] value% [+] row, shared by the Music/SFX volume controls below.
// `apply` receives the already-clamped new 0..1 value and is expected to
// write it straight back through AudioManager's own setter (which persists
// it — see AudioManager::setMusicVolume/setSfxVolume), not into a copy.
void draw_volume_row(const Font& font, const Rect& row, const char* label, float volume, const UiScale& scale,
                      Vector2 mouse, bool clicked, const std::function<void(float)>& apply) {
    Rect r = row;
    Rect labelRect = cut_left(r, scale.px(240));
    draw_text(font, label, labelRect, static_cast<float>(scale.points(13)), theme::textPrimary, {HAlign::Left, VAlign::Center, false, false});

    Rect plusRect = cut_right(r, scale.px(40));
    cut_right(r, scale.px(6));
    Rect valueRect = cut_right(r, scale.px(64));
    cut_right(r, scale.px(6));
    Rect minusRect = cut_right(r, scale.px(40));

    const std::string pct = std::to_string(static_cast<int>(std::lround(volume * 100.0f))) + "%";
    draw_text(font, pct, valueRect, static_cast<float>(scale.points(13)), theme::gold, {HAlign::Center, VAlign::Center, false, false});
    // 5% steps (the top-level task's own suggested increment) — clamped so
    // repeated clicks at either end just hold at 0%/100% instead of erroring.
    if (button(font, minusRect, "-", nullptr, scale, mouse, clicked)) apply(std::clamp(volume - 0.05f, 0.0f, 1.0f));
    if (button(font, plusRect, "+", nullptr, scale, mouse, clicked)) apply(std::clamp(volume + 0.05f, 0.0f, 1.0f));
}

// Reachable from every menu screen via draw_app_header's OPTIONS button (or
// Title's own copy of it) — never from Duel, which doesn't call
// draw_app_header at all, so this is never exposed mid-duel (see the
// top-level task's own note that Options during a duel needs either a proper
// overlay or to stay unexposed; this ships the latter). BACK returns to
// whichever screen actually opened it (AppState::options_return_screen)
// rather than hardcoding Hub, since Options has more than one entry point.
void paint_options(AppState& state, const Rect& client, const UiScale& scale, Vector2 mouse, bool clicked) {
    Rect area = inset(client, scale.px(28));
    Rect header = cut_top(area, scale.px(50));
    draw_text(state.font, "OPTIONS", header, static_cast<float>(scale.points(26)), theme::gold, {HAlign::Left, VAlign::Center, false, false});
    cut_top(area, scale.px(20));

    Rect panel = centered(area, std::min(area.width(), scale.px(560)), scale.px(220));
    draw_panel(panel, theme::panel, true, theme::panelBorder);
    Rect inner = inset(panel, scale.px(24));

    Rect musicRow = cut_top(inner, scale.px(44));
    draw_volume_row(state.font, musicRow, "MUSIC VOLUME", state.audio.musicVolume(), scale, mouse, clicked,
                     [&state](float v) { state.audio.setMusicVolume(v); });
    cut_top(inner, scale.px(18));
    Rect sfxRow = cut_top(inner, scale.px(44));
    draw_volume_row(state.font, sfxRow, "SOUND EFFECTS VOLUME", state.audio.sfxVolume(), scale, mouse, clicked,
                     [&state](float v) { state.audio.setSfxVolume(v); });

    cut_top(inner, scale.px(30));
    Rect backRow = cut_top(inner, scale.px(48));
    Rect backButton = centered(backRow, scale.px(180), scale.px(44));
    if (button(state.font, backButton, "BACK", nullptr, scale, mouse, clicked)) state.screen = state.options_return_screen;
}

// ---------- duel board: drawing ----------

// Finds the legal_actions index of the multi-select candidate (see
// AppState::multi_select_active) occupying the given zone, or -1 if none —
// used both to decide whether a zone should render toggled and to know which
// index a click on that zone should toggle.
int find_multi_select_candidate(const AppState& state, uint8_t controller, uint8_t location, uint32_t sequence) {
    if (!state.multi_select_active) return -1;
    for (size_t i = 0; i < state.legal_actions.size(); ++i) {
        const auto& a = state.legal_actions[i];
        if (a.controller == controller && a.location == location && a.sequence == sequence) return static_cast<int>(i);
    }
    return -1;
}

// Ported from src/client/main.cpp's draw_card_slot. `empty_label` names the
// zone kind ("MONSTER"/"SPELL/TRAP"/"FIELD") shown only while unoccupied.
void draw_card_slot(AppState& state, const Rect& cell, const FieldCard& card, bool is_monster_zone,
                     const char* empty_label, const UiScale& scale, bool hovered, bool legal_target, bool toggled = false) {
    draw_panel(cell, theme::fieldZone, false, {});
    const Color outline = toggled ? theme::buffed : (legal_target ? theme::legal : (hovered ? theme::gold : theme::fieldZoneLine));
    draw_rect_outline(cell, toggled || legal_target || hovered ? 3.0f : 1.0f, outline);

    if (!card.occupied) {
        if (empty_label && *empty_label) {
            const Rect labelRect{cell.left, cell.bottom - scale.px(30), cell.right, cell.bottom};
            draw_text(state.font, empty_label, labelRect, static_cast<float>(scale.points(9)), theme::textSecondary, {HAlign::Center, VAlign::Center, true, false});
        }
        return;
    }

    const bool faceDown = (card.position & POS_FACEDOWN) != 0;
    const bool defense = is_monster_zone && (card.position & POS_DEFENSE) != 0;
    const Rect footprint = defense ? defense_footprint(cell) : fit_card_in_cell(cell);

    if (faceDown) {
        // Always render a face-down card as a card back on the board — even
        // your own — matching a real table. The engine only redacts the
        // opponent's code (never our own), so hovering/clicking still
        // reveals the true identity in the inspector for our own cards
        // specifically; see compute_duel_hover. The board art itself never
        // leaks it.
        const Texture2D back = state.textures.get("external/extra/card_back.jpg");
        if (back.id != 0) {
            if (defense) draw_bitmap_rotated(back, footprint); else draw_bitmap_fit(back, footprint);
        } else {
            draw_panel(footprint, theme::cardBack, false, {});
        }
        draw_rect_outline(footprint, 1.0f, theme::gold);
        if (toggled) draw_rect_outline(inset(footprint, -2), 3.0f, theme::buffed);
        else if (hovered) draw_rect_outline(inset(footprint, -2), 2.0f, theme::gold);
        return;
    }

    const Texture2D texture = get_card_texture(state, card.code);
    if (texture.id != 0) {
        if (defense) draw_bitmap_rotated(texture, footprint); else draw_bitmap_fit(texture, footprint);
    } else {
        draw_panel(footprint, Color{30, 30, 38, 255}, false, {});
        const auto& def = state.card_database.resolve(card.code);
        draw_text(state.font, def.name + "\n(image unavailable)", footprint, static_cast<float>(scale.points(9)), theme::textSecondary, {HAlign::Center, VAlign::Center, true, false});
    }
    if (toggled) draw_rect_outline(inset(footprint, -2), 3.0f, theme::buffed);
    else if (hovered) draw_rect_outline(inset(footprint, -2), 2.0f, theme::gold);
}

// `hovered` only ever means something for Grave/Banished (see
// resolve_duel_click's pile-viewer-opening branch and draw_pile_viewer) —
// Deck/Extra aren't clickable (their contents are hidden or, for Extra,
// already fully visible in the Deck Editor), so their call sites just leave
// it at the default.
void draw_pile_zone(const Font& font, const Rect& cell, const char* label, uint32_t count, const UiScale& scale, bool hovered = false) {
    draw_panel(cell, hovered ? theme::hover : theme::panel, true, hovered ? theme::gold : theme::panelBorder);
    const Rect labelRect{cell.left, cell.top + scale.px(2), cell.right, cell.top + cell.height() / 2};
    draw_text(font, label, labelRect, static_cast<float>(scale.points(8)), theme::textSecondary, {HAlign::Center, VAlign::Center, false, false});
    const Rect countRect{cell.left, cell.top + cell.height() / 2, cell.right, cell.bottom - scale.px(2)};
    draw_text(font, std::to_string(count), countRect, static_cast<float>(scale.points(13)), theme::textPrimary, {HAlign::Center, VAlign::Center, false, false});
}

void draw_hud_bar(AppState& state, const Rect& r, bool opponent, const UiScale& scale) {
    draw_panel(r, theme::panel, true, theme::panelBorder);
    Rect inner = inset(r, scale.px(12), scale.px(4));
    Rect nameRect = cut_left(inner, inner.width() / 2);
    const std::string name = opponent ? state.catalog.npcs.at(state.selected_npc % state.catalog.npcs.size()).name : "YOU";
    draw_text(state.font, name, nameRect, static_cast<float>(scale.points(15)), theme::gold, {HAlign::Left, VAlign::Center, false, true});
    const size_t idx = opponent ? 1u : 0u;
    const int lp = static_cast<int>(std::lround(state.life_display[idx]));
    std::string info = "LP " + std::to_string(lp) + "   Hand " + std::to_string(opponent ? state.hand_count[1] : state.hand_cards.size()) +
        "   Deck " + std::to_string(state.deck_count[idx]) + "   GY " + std::to_string(state.grave_count[idx]);
    if (state.banished_count[idx]) info += "   Banished " + std::to_string(state.banished_count[idx]);
    draw_text(state.font, info, inner, static_cast<float>(scale.points(13)), lp <= 1000 ? theme::danger : theme::textPrimary, {HAlign::Right, VAlign::Center, false, false});
}

void draw_phase_button(const Font& font, const Rect& box, const char* title, const UiScale& scale, bool enabled, bool hovered) {
    const Color fill = !enabled ? Color{16, 24, 32, 255} : (hovered ? Color{45, 84, 112, 255} : theme::panel);
    const Color border = !enabled ? theme::panelBorder : (hovered ? theme::gold : theme::legal);
    draw_panel(box, fill, true, border);
    draw_text(font, title, box, static_cast<float>(scale.points(11)), enabled ? theme::gold : theme::textSecondary, {HAlign::Center, VAlign::Center, false, false});
}

void draw_turn_indicator(AppState& state, const Rect& r, const UiScale& scale, Vector2 mouse) {
    draw_panel(r, Color{14, 22, 30, 255}, true, theme::panelBorder);
    const auto pb = compute_phase_bar_layout(r, scale);
    std::string text = "TURN " + std::to_string(state.turn_number) + " \xE2\x80\xA2 " + (state.turn_player == 0 ? "YOUR TURN" : "OPPONENT'S TURN");
    const std::string phase = phase_name(state.phase);
    if (!phase.empty()) text += "  \xE2\x80\x94  " + phase;
    draw_text(state.font, text, pb.label, static_cast<float>(scale.points(12)), theme::textPrimary, {HAlign::Left, VAlign::Center, false, false});
    const int mx = static_cast<int>(mouse.x), my = static_cast<int>(mouse.y);
    draw_phase_button(state.font, pb.battle, "Battle Phase", scale, state.action_layout.battle_phase_action >= 0, pb.battle.contains(mx, my));
    draw_phase_button(state.font, pb.main2, "Main Phase 2", scale, state.action_layout.main_phase2_action >= 0, pb.main2.contains(mx, my));
    draw_phase_button(state.font, pb.end, "End Phase", scale, state.action_layout.end_phase_action >= 0, pb.end.contains(mx, my));
}

void draw_hand_row(AppState& state, const Rect& area, bool own, const DuelHover& hover, const UiScale& scale) {
    const size_t count = own ? state.hand_cards.size() : static_cast<size_t>(state.hand_count[1]);
    const auto rects = layout_hand(area, count);
    if (rects.empty()) {
        if (own) draw_text(state.font, "Hand empty", area, static_cast<float>(scale.points(10)), theme::textSecondary, {HAlign::Center, VAlign::Center, false, false});
        return;
    }
    const int hoveredIdx = own ? hover.hand_index : -1;
    const Texture2D cardBack = state.textures.get("external/extra/card_back.jpg");
    auto drawOne = [&](size_t i, bool isHovered) {
        const bool isOpen = own && state.open_hand_card == static_cast<int>(i);
        const bool hasActions = own && state.action_layout.hand_actions.count(state.hand_cards[i]) > 0;
        Rect r = rects[i];
        if (isHovered || isOpen) r.top -= scale.px(10);
        if (own) {
            const Texture2D texture = get_card_texture(state, state.hand_cards[i]);
            if (texture.id != 0) draw_bitmap_fit(texture, r);
            else draw_panel(r, Color{30, 30, 38, 255}, true, theme::panelBorder);
        } else if (cardBack.id != 0) {
            draw_bitmap_fit(cardBack, r);
            draw_rect_outline(r, 1.0f, theme::gold);
        } else {
            draw_panel(r, theme::cardBack, true, theme::gold);
        }
        // Toggled (blue, multi-select) beats legal (green) beats
        // hovered/open (gold) — a stronger cue always wins over a weaker one.
        const int candidate = own ? find_multi_select_candidate(state, 0, LOCATION_HAND, static_cast<uint32_t>(i)) : -1;
        const bool toggled = candidate >= 0 && state.multi_select_toggled[static_cast<size_t>(candidate)];
        if (toggled) draw_rect_outline(inset(r, -2), 3.0f, theme::buffed);
        else if (isOpen || isHovered) draw_rect_outline(inset(r, -2), 2.0f, theme::gold);
        else if (hasActions) draw_rect_outline(inset(r, -2), 2.0f, theme::legal);
    };
    const int openIdx = own ? state.open_hand_card : -1;
    for (size_t i = 0; i < rects.size(); ++i) if (static_cast<int>(i) != hoveredIdx && static_cast<int>(i) != openIdx) drawOne(i, false);
    if (hoveredIdx >= 0 && static_cast<size_t>(hoveredIdx) < rects.size() && hoveredIdx != openIdx) drawOne(static_cast<size_t>(hoveredIdx), true);
    if (openIdx >= 0 && static_cast<size_t>(openIdx) < rects.size()) drawOne(static_cast<size_t>(openIdx), hoveredIdx == openIdx);
}

void draw_inspector(AppState& state, const Rect& panel, const DuelHover& hover, const UiScale& scale, Vector2 mouse) {
    draw_panel(panel, theme::panel, true, theme::panelBorder);
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
        const Texture2D texture = get_card_texture(state, code);
        if (texture.id != 0) {
            draw_bitmap_fit(texture, artArea);
        } else {
            draw_panel(artArea, Color{24, 24, 30, 255}, true, theme::panelBorder);
            draw_text(state.font, "CARD IMAGE\nUNAVAILABLE", artArea, static_cast<float>(scale.points(11)), theme::textSecondary, {HAlign::Center, VAlign::Center, true, false});
        }
    } else {
        draw_panel(artArea, Color{24, 24, 30, 255}, true, theme::panelBorder);
        draw_text(state.font, showBack ? "FACE-DOWN CARD" : "Hover a card\nto inspect it", artArea, static_cast<float>(scale.points(11)), theme::textSecondary, {HAlign::Center, VAlign::Center, true, false});
    }
    cut_top(inner, scale.px(10));
    if (code == 0 || showBack) {
        draw_text(state.font, "Tip: monster-zone and attack choices during your turn can be clicked directly on the board. Everything else appears in the panel below the field.",
                   inner, static_cast<float>(scale.points(10)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});
        return;
    }
    const auto& def = state.card_database.resolve(code);
    Rect nameRect = cut_top(inner, scale.px(24));
    draw_text(state.font, def.name, nameRect, static_cast<float>(scale.points(15)), theme::gold, {HAlign::Left, VAlign::Center, true, false});
    Rect statRect = cut_top(inner, scale.px(20));
    // A board selection (a click) always wins over mere hover, same
    // priority `code` itself already follows above.
    const int statPlayer = selection.monster_sequence >= 0 ? 0 : hover.inspect_monster_player;
    const int statSequence = selection.monster_sequence >= 0 ? selection.monster_sequence : hover.inspect_monster_sequence;
    int32_t currentAttack = -1, currentDefense = -1;
    if (statPlayer >= 0 && statSequence >= 0) {
        const auto& fieldCard = state.monsters[static_cast<size_t>(statPlayer)][static_cast<size_t>(statSequence)];
        if (fieldCard.occupied && fieldCard.code == code) { currentAttack = fieldCard.attack; currentDefense = fieldCard.defense; }
    }
    draw_card_stat_line(state.font, def, currentAttack, currentDefense, statRect, static_cast<float>(scale.points(11)));
    Rect typeRect = cut_top(inner, scale.px(34));
    draw_text(state.font, describe_card_type(def), typeRect, static_cast<float>(scale.points(10)), theme::textSecondary, {HAlign::Left, VAlign::Top, true, false});
    if (!def.text.empty()) {
        cut_top(inner, scale.px(6));
        draw_text(state.font, def.text, inner, static_cast<float>(scale.points(10)), theme::textPrimary, {HAlign::Left, VAlign::Top, true, false});
    }
    if (hasSelectionActions) {
        for (size_t i = 0; i < actionLayout.buttons.size(); ++i) {
            const auto& label = state.legal_actions[(*selection.actions)[i]].label;
            const bool hovered = actionLayout.buttons[i].contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
            draw_button(state.font, actionLayout.buttons[i], short_action_label(label).c_str(), nullptr, scale, hovered);
        }
    }
}

// How long the "Waiting for the rules engine…" gap can last before this
// escalates — long enough that a legitimately slow engine decision (a big
// chain, a complex CPU turn) doesn't false-positive, short enough that
// nobody is stuck for the engine's own full 60-second response timeout with
// zero feedback.
constexpr double kStuckDuelSeconds = 20.0;

enum class DuelWaitState { Normal, Busy, Stuck };

// Classifies how long the current "no legal_actions yet" gap has lasted:
// Normal (still within the ordinary round-trip window), Busy (the gap is
// long, but state.txt keeps changing — the engine is actively working
// through a long CPU turn or a deep chain, not actually broken), or Stuck
// (the gap is long AND nothing has changed at all — the strongest signal
// something is really wrong). Only Stuck offers a way to abandon the duel;
// Busy is shown so a slow-but-fine duel doesn't read as identical to a
// hung one. See draw_prompt_panel (message) and resolve_duel_click (the
// actual abandon action).
DuelWaitState duel_wait_state(const AppState& state) {
    if (state.waiting_since == 0.0 || GetTime() - state.waiting_since <= kStuckDuelSeconds) return DuelWaitState::Normal;
    if (GetTime() - state.last_board_change_time <= kStuckDuelSeconds) return DuelWaitState::Busy;
    return DuelWaitState::Stuck;
}

// Multi-select candidates whose location isn't a rendered board/hand zone
// (graveyard, banished, deck, extra deck) have no on-screen tile to click —
// these fall back to a checkbox-style row list in the prompt panel, using
// the same toggle+Confirm model as the on-board click path (never the old
// combinatorial list).
std::vector<size_t> off_board_multi_select_candidates(const AppState& state) {
    std::vector<size_t> result;
    for (size_t i = 0; i < state.legal_actions.size(); ++i) {
        const auto loc = state.legal_actions[i].location;
        if (loc != LOCATION_HAND && loc != LOCATION_MZONE && loc != LOCATION_SZONE) result.push_back(i);
    }
    return result;
}

struct MultiSelectRow { Rect rect; size_t candidate_index{}; };
struct MultiSelectLayout {
    Rect title, status;
    std::vector<MultiSelectRow> rows;
    Rect prev_button, next_button, pager_label;
    bool has_pager = false;
    size_t page_count = 1;
    Rect confirm_button;
    bool confirm_enabled = false;
};

MultiSelectLayout compute_multi_select_layout(const AppState& state, const Rect& panelRect, const UiScale& scale) {
    MultiSelectLayout out;
    Rect panel = inset(panelRect, scale.px(8));
    out.title = cut_top(panel, scale.px(20));
    out.status = cut_top(panel, scale.px(18));
    out.confirm_button = cut_bottom(panel, scale.px(28));

    const auto offBoard = off_board_multi_select_candidates(state);
    out.page_count = std::max<size_t>(1, (offBoard.size() + kPromptRowsPerPage - 1) / kPromptRowsPerPage);
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
        const size_t globalIndex = state.action_page * kPromptRowsPerPage + i;
        if (globalIndex >= offBoard.size()) break;
        const int top = panel.top + static_cast<int>(i) * (rowHeight + rowGap);
        out.rows.push_back({Rect{panel.left, top, panel.right, top + rowHeight}, offBoard[globalIndex]});
    }
    const size_t selectedCount = static_cast<size_t>(std::count(state.multi_select_toggled.begin(), state.multi_select_toggled.end(), true));
    out.confirm_enabled = selectedCount >= state.multi_select_min && selectedCount <= state.multi_select_max;
    return out;
}

void draw_prompt_panel(AppState& state, const Rect& panelRect, const UiScale& scale, Vector2 mouse) {
    draw_panel(panelRect, theme::panel, true, theme::panelBorder);
    const int mx = static_cast<int>(mouse.x), my = static_cast<int>(mouse.y);
    if (!state.player_process.valid() && state.legal_actions.empty()) {
        Rect inner = inset(panelRect, scale.px(10));
        draw_text(state.font, "Duel finished. Click anywhere to return to the campaign hub.", inner, static_cast<float>(scale.points(13)), theme::textPrimary, {HAlign::Center, VAlign::Center, true, false});
        return;
    }
    if (state.legal_actions.empty()) {
        Rect inner = inset(panelRect, scale.px(10));
        const DuelWaitState waitState = duel_wait_state(state);
        std::string message = "Waiting for the rules engine\xE2\x80\xA6";
        Color color = theme::textSecondary;
        if (waitState == DuelWaitState::Busy) {
            message = "The engine is deep in a long turn or chain \xE2\x80\x94 still working, just taking a while.";
        } else if (waitState == DuelWaitState::Stuck) {
            message = "Still waiting on the duel engine after a while \xE2\x80\x94 it may be stuck. Click here to abandon this duel and return to the hub.";
            color = theme::danger;
        }
        draw_text(state.font, message, inner, static_cast<float>(scale.points(13)), color, {HAlign::Center, VAlign::Center, true, false});
        return;
    }

    if (state.multi_select_active) {
        const auto ms = compute_multi_select_layout(state, panelRect, scale);
        draw_text(state.font, state.prompt_title + " \xE2\x80\x94 click cards to select them", ms.title, static_cast<float>(scale.points(12)), theme::gold, {HAlign::Left, VAlign::Center, false, true});
        const size_t selectedCount = static_cast<size_t>(std::count(state.multi_select_toggled.begin(), state.multi_select_toggled.end(), true));
        const std::string statusText = "Selected " + std::to_string(selectedCount) + " (need " +
            (state.multi_select_min == state.multi_select_max ? std::to_string(state.multi_select_min) : std::to_string(state.multi_select_min) + "\xE2\x80\x93" + std::to_string(state.multi_select_max)) + ")";
        draw_text(state.font, statusText, ms.status, static_cast<float>(scale.points(11)), theme::textSecondary, {HAlign::Left, VAlign::Center, false, false});
        for (const auto& row : ms.rows) {
            const auto& candidate = state.legal_actions[row.candidate_index];
            const bool toggled = state.multi_select_toggled[row.candidate_index];
            const std::string label = (toggled ? "[x] " : "[ ] ") + candidate.label;
            draw_button(state.font, row.rect, label.c_str(), nullptr, scale, row.rect.contains(mx, my));
        }
        if (ms.has_pager) {
            draw_button(state.font, ms.prev_button, "\xE2\x80\xB9 Prev", nullptr, scale, state.action_page > 0 && ms.prev_button.contains(mx, my));
            draw_button(state.font, ms.next_button, "Next \xE2\x80\xBA", nullptr, scale, state.action_page + 1 < ms.page_count && ms.next_button.contains(mx, my));
            const std::string pageText = std::to_string(state.action_page + 1) + " / " + std::to_string(ms.page_count);
            draw_text(state.font, pageText, ms.pager_label, static_cast<float>(scale.points(11)), theme::textSecondary, {HAlign::Center, VAlign::Center, false, false});
        }
        draw_phase_button(state.font, ms.confirm_button, "Confirm", scale, ms.confirm_enabled, ms.confirm_button.contains(mx, my));
        return;
    }

    const auto pl = compute_prompt_layout(panelRect, state.action_layout.panel_indices.size(), scale);
    std::string title = state.prompt_title;
    if (state.action_layout.all_zone_placement) title += state.action_layout.zone_placement_is_spell ? " \xE2\x80\x94 click a highlighted Spell/Trap Zone" : " \xE2\x80\x94 click a highlighted Monster Zone";
    else if (state.action_layout.has_attacks) title += " \xE2\x80\x94 click a monster on the field to attack";
    else if (!state.action_layout.hand_actions.empty()) title += " \xE2\x80\x94 also check the highlighted cards in your hand";
    draw_text(state.font, title, pl.title, static_cast<float>(scale.points(12)), theme::gold, {HAlign::Left, VAlign::Center, false, true});
    if (state.action_layout.all_zone_placement) return;

    for (size_t i = 0; i < pl.rows.size(); ++i) {
        const size_t globalIndex = state.action_page * kPromptRowsPerPage + i;
        if (globalIndex >= state.action_layout.panel_indices.size()) break;
        const auto& action = state.legal_actions[state.action_layout.panel_indices[globalIndex]];
        draw_button(state.font, pl.rows[i], action.label.c_str(), nullptr, scale, pl.rows[i].contains(mx, my));
    }
    if (pl.has_pager) {
        draw_button(state.font, pl.prev_button, "\xE2\x80\xB9 Prev", nullptr, scale, state.action_page > 0 && pl.prev_button.contains(mx, my));
        draw_button(state.font, pl.next_button, "Next \xE2\x80\xBA", nullptr, scale, state.action_page + 1 < pl.page_count && pl.next_button.contains(mx, my));
        const std::string pageText = std::to_string(state.action_page + 1) + " / " + std::to_string(pl.page_count);
        draw_text(state.font, pageText, pl.pager_label, static_cast<float>(scale.points(11)), theme::textSecondary, {HAlign::Center, VAlign::Center, false, false});
    }
}

// Drawn last (top of the z-order) so its buttons are never covered by
// anything else on the board.
void draw_hand_popup(AppState& state, const DuelLayout& L, const UiScale& scale, Vector2 mouse) {
    if (state.open_hand_card < 0 || static_cast<size_t>(state.open_hand_card) >= state.hand_cards.size()) return;
    const uint32_t code = state.hand_cards[static_cast<size_t>(state.open_hand_card)];
    const auto it = state.action_layout.hand_actions.find(code);
    if (it == state.action_layout.hand_actions.end() || it->second.empty()) return;
    const auto handRects = layout_hand(L.player_hand, state.hand_cards.size());
    if (static_cast<size_t>(state.open_hand_card) >= handRects.size()) return;

    const auto popup = compute_hand_popup_layout(handRects[static_cast<size_t>(state.open_hand_card)], it->second.size(), scale, L.opponent_hud);
    draw_panel(popup.panel, theme::panel, true, theme::gold);
    for (size_t i = 0; i < popup.buttons.size(); ++i) {
        const auto& action = state.legal_actions[it->second[i]];
        const bool hovered = popup.buttons[i].contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
        draw_button(state.font, popup.buttons[i], short_action_label(action.label).c_str(), nullptr, scale, hovered);
    }
}

// ---------- duel board: click resolution ----------
//
// Unlike every other screen in this file, the duel board doesn't decompose
// into independent button()/toggle_chip() calls: Win32's handle_duel_click
// has ~9 ordered, mutually-exclusive priority branches (inspector action
// buttons, hand popup, hand cards, phase buttons, zone-placement mode,
// attacks, zone selection, prompt panel) where an earlier match must
// prevent every later one from also firing on the same click. That doesn't
// fit the "one call draws and reports its own click" pattern, so — same as
// src/client/main.cpp's own separate handle_duel_click — this is one
// dedicated function, called once per frame before drawing, mirroring the
// original's priority order exactly.
// Opens the Graveyard/Banished pile viewer overlay (see draw_pile_viewer)
// for `player`'s `kind` pile, or closes it if that exact pile is already
// open — a toggle, like selecting/deselecting a board zone elsewhere in this
// file. Resets the page/selection either way so a stale selection from a
// previously viewed pile never leaks into the next one.
void open_pile_view(AppState& state, int player, PileKind kind) {
    if (state.viewing_pile_kind == kind && state.viewing_pile_player == player) {
        state.viewing_pile_kind = PileKind::None;
        state.viewing_pile_player = -1;
    } else {
        state.viewing_pile_kind = kind;
        state.viewing_pile_player = player;
    }
    state.viewing_pile_selected = -1;
    state.viewing_pile_page = 0;
}

const std::vector<uint32_t>& pile_cards(AppState& state, int player, PileKind kind) {
    static const std::vector<uint32_t> empty;
    if (kind == PileKind::Extra) return state.extra_cards; // player-only — `player` is ignored, always "you"
    if (player < 0 || player > 1 || kind == PileKind::None) return empty;
    return kind == PileKind::Grave ? state.grave_cards[static_cast<size_t>(player)] : state.banished_cards[static_cast<size_t>(player)];
}

void resolve_duel_click(AppState& state, const DuelLayout& L, const UiScale& scale, Vector2 mouse) {
    const int x = static_cast<int>(mouse.x), y = static_cast<int>(mouse.y);
    if (!state.player_process.valid()) {
        if (state.legal_actions.empty()) state.screen = Screen::Hub;
        return;
    }
    if (state.legal_actions.empty()) {
        // Once draw_prompt_panel has escalated to the "may be stuck" message
        // (see kStuckDuelSeconds), clicking anywhere abandons the duel
        // instead of leaving the player with no recourse but to force-quit.
        if (duel_wait_state(state) == DuelWaitState::Stuck) {
            goat::process::terminate(state.player_process);
            state.legal_actions.clear();
            state.action_layout = ActionLayout{};
            state.prompt_title.clear();
            state.open_hand_card = -1; state.selected_monster_zone = -1; state.selected_spell_zone = -1;
            state.multi_select_active = false;
            state.multi_select_toggled.clear();
            state.waiting_since = 0.0;
            state.status = "Abandoned a duel that stopped responding.";
            state.screen = Screen::Hub;
        }
        return;
    }

    // Debounce: ignore clicks for a short window after any submission — two
    // clicks can land in quick succession (an accidental double-click, or
    // clicking fast through a run of back-to-back "Confirm" prompts) and the
    // second one can otherwise land on whatever row now occupies that same
    // screen position in a brand-new, unrelated prompt.
    constexpr double kClickDebounceSeconds = 0.3;
    if (GetTime() - state.last_submit_time < kClickDebounceSeconds) return;

    // Graveyard/Banished piles aren't part of the normal action flow — a
    // click there always opens (or, on a second click on the same one,
    // closes) the pile viewer overlay, regardless of whatever prompt/zone-
    // placement mode is otherwise active. Placed before every branch below
    // so it's never swallowed by e.g. all_zone_placement's own unconditional
    // `return` once a placement prompt is in progress. paint_duel itself
    // skips calling this function at all while the viewer is already open
    // (see its own comment), so there's no risk of a click meant for the
    // viewer's own grid/buttons reopening or double-handling here.
    if (L.player_grave.contains(x, y)) { open_pile_view(state, 0, PileKind::Grave); return; }
    if (L.opponent_grave.contains(x, y)) { open_pile_view(state, 1, PileKind::Grave); return; }
    if (L.player_banished.contains(x, y)) { open_pile_view(state, 0, PileKind::Banished); return; }
    if (L.opponent_banished.contains(x, y)) { open_pile_view(state, 1, PileKind::Banished); return; }
    // Extra deck is player-only — deliberately no L.opponent_extra branch
    // here at all, so the opponent's extra deck (private information, see
    // write_board_state) never gets a click target of any kind.
    if (L.player_extra.contains(x, y)) { open_pile_view(state, 0, PileKind::Extra); return; }

    // A "#SELECT <min> <max>" prompt (see AppState::multi_select_active)
    // takes over every click while active: it isn't a normal action list, so
    // none of the branches below (which all assume state.action_layout was
    // built from verb-prefixed action labels) apply. build_action_layout is
    // never even run for this prompt shape — see paint_duel.
    if (state.multi_select_active) {
        auto toggle = [&](int index) {
            if (index < 0 || static_cast<size_t>(index) >= state.multi_select_toggled.size()) return;
            const size_t idx = static_cast<size_t>(index);
            const auto selectedCount = static_cast<size_t>(std::count(state.multi_select_toggled.begin(), state.multi_select_toggled.end(), true));
            if (!state.multi_select_toggled[idx] && selectedCount >= state.multi_select_max) return;
            state.multi_select_toggled[idx] = !state.multi_select_toggled[idx];
        };

        const auto handRects = layout_hand(L.player_hand, state.hand_cards.size());
        for (size_t i = 0; i < handRects.size(); ++i) {
            if (handRects[i].contains(x, y)) { toggle(find_multi_select_candidate(state, 0, LOCATION_HAND, static_cast<uint32_t>(i))); return; }
        }
        for (int i = 0; i < 5; ++i) {
            const size_t si = static_cast<size_t>(i);
            if (L.player_monsters[si].contains(x, y)) { toggle(find_multi_select_candidate(state, 0, LOCATION_MZONE, static_cast<uint32_t>(i))); return; }
            if (L.player_spells[si].contains(x, y)) { toggle(find_multi_select_candidate(state, 0, LOCATION_SZONE, static_cast<uint32_t>(i))); return; }
            if (L.opponent_monsters[si].contains(x, y)) { toggle(find_multi_select_candidate(state, 1, LOCATION_MZONE, static_cast<uint32_t>(i))); return; }
            if (L.opponent_spells[si].contains(x, y)) { toggle(find_multi_select_candidate(state, 1, LOCATION_SZONE, static_cast<uint32_t>(i))); return; }
        }
        if (L.player_spells[5].contains(x, y)) { toggle(find_multi_select_candidate(state, 0, LOCATION_SZONE, 5)); return; }
        if (L.opponent_spells[5].contains(x, y)) { toggle(find_multi_select_candidate(state, 1, LOCATION_SZONE, 5)); return; }

        const auto ms = compute_multi_select_layout(state, L.prompt_panel, scale);
        for (const auto& row : ms.rows) {
            if (row.rect.contains(x, y)) { toggle(static_cast<int>(row.candidate_index)); return; }
        }
        if (ms.has_pager) {
            if (ms.prev_button.contains(x, y) && state.action_page > 0) { --state.action_page; return; }
            if (ms.next_button.contains(x, y) && state.action_page + 1 < ms.page_count) { ++state.action_page; return; }
        }
        if (ms.confirm_enabled && ms.confirm_button.contains(x, y)) submit_multi_selection(state);
        return;
    }

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
    // underneath it.
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
    // a second click) when it has Change Position / Activate options; a
    // click there is always consumed, even when that specific card has
    // nothing to do.
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

// ---------- duel board: Graveyard/Banished pile viewer ----------

// A centered modal panel over the duel board — reuses the same card-grid +
// side detail panel + pager recipe as Collection/Deck Editor's pool
// (compute_card_grid_layout), just with a CLOSE button up top instead of
// living on its own Screen.
struct PileViewLayout { Rect panel, header, close_button, detail; Rect grid_area; CardGridLayout grid; Rect prev_button, next_button, page_label; };

PileViewLayout compute_pile_view_layout(const Rect& client, size_t card_count, const UiScale& scale) {
    PileViewLayout L;
    const int panelWidth = std::clamp(client.width() - scale.px(80), scale.px(360), scale.px(880));
    const int panelHeight = std::clamp(client.height() - scale.px(80), scale.px(280), scale.px(560));
    L.panel = centered(client, panelWidth, panelHeight);
    Rect inner = inset(L.panel, scale.px(20));

    Rect headerRow = cut_top(inner, scale.px(30));
    L.close_button = cut_right(headerRow, scale.px(90));
    L.header = headerRow;
    cut_top(inner, scale.px(10));

    const int detailWidth = std::clamp(scale.px(220), 160, std::max(160, inner.width() / 3));
    L.detail = cut_right(inner, detailWidth);
    cut_right(inner, scale.px(14));

    // Local pager strip at the bottom, unconditionally reserved (matches
    // compute_deck_editor_layout's own pool pager) — its buttons just sit
    // disabled (see draw_pile_viewer's offscreen-mouse trick) when there's
    // only one page.
    Rect pagerRow = cut_bottom(inner, scale.px(26));
    cut_bottom(inner, scale.px(6));
    const int pagerButtonWidth = scale.px(70);
    L.prev_button = {pagerRow.left, pagerRow.top, pagerRow.left + pagerButtonWidth, pagerRow.bottom};
    L.next_button = {pagerRow.right - pagerButtonWidth, pagerRow.top, pagerRow.right, pagerRow.bottom};
    L.page_label = {L.prev_button.right + scale.px(8), pagerRow.top, L.next_button.left - scale.px(8), pagerRow.bottom};

    L.grid_area = inner;
    L.grid = compute_card_grid_layout(inner, card_count, scale);
    return L;
}

// Draws (and, via its own button()/hit-test calls, handles clicks for) the
// Graveyard/Banished viewer overlay when one is open — a no-op otherwise.
// Deliberately draws on top of everything else paint_duel draws (called
// last there) and, while open, is the *only* thing that gets this frame's
// click at all: paint_duel skips calling resolve_duel_click entirely in
// that case, so a click meant for the grid/CLOSE/pager here can never also
// land on the board underneath it.
void draw_pile_viewer(AppState& state, const Rect& client, const UiScale& scale, Vector2 mouse, bool clicked) {
    if (state.viewing_pile_kind == PileKind::None) return;
    const auto& cards = pile_cards(state, state.viewing_pile_player, state.viewing_pile_kind);
    const auto L = compute_pile_view_layout(client, cards.size(), scale);
    state.viewing_pile_page = std::min(state.viewing_pile_page, L.grid.page_count - 1);

    // Dims the board behind the modal so it reads unambiguously as a popup
    // rather than part of the normal board.
    DrawRectangle(client.left, client.top, client.width(), client.height(), Color{0, 0, 0, 140});
    draw_panel(L.panel, theme::panel, true, theme::gold);

    const std::string owner = state.viewing_pile_player == 0 ? "YOUR " : "OPPONENT'S ";
    const std::string kindLabel = state.viewing_pile_kind == PileKind::Grave ? "GRAVEYARD"
        : state.viewing_pile_kind == PileKind::Banished ? "BANISHED" : "EXTRA DECK";
    const std::string title = owner + kindLabel + " (" + std::to_string(cards.size()) + ")";
    draw_text(state.font, title, L.header, static_cast<float>(scale.points(16)), theme::gold, {HAlign::Left, VAlign::Center, false, false});
    if (button(state.font, L.close_button, "CLOSE", nullptr, scale, mouse, clicked)) {
        state.viewing_pile_kind = PileKind::None;
        state.viewing_pile_player = -1;
        return;
    }

    draw_panel(L.detail, theme::panel, true, theme::panelBorder);
    const uint32_t selectedCode = (state.viewing_pile_selected >= 0 && static_cast<size_t>(state.viewing_pile_selected) < cards.size())
        ? cards[static_cast<size_t>(state.viewing_pile_selected)] : 0;
    draw_card_detail(state, inset(L.detail, scale.px(12)), selectedCode, scale);

    const size_t first = state.viewing_pile_page * L.grid.per_page;
    for (size_t slot = 0; slot < L.grid.cards.size() && first + slot < cards.size(); ++slot) {
        const size_t index = first + slot;
        const Rect& r = L.grid.cards[slot];
        const bool selected = state.viewing_pile_selected == static_cast<int>(index);
        const bool hovered = r.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y));
        draw_panel(r, Color{24, 24, 30, 255}, true, selected ? theme::gold : (hovered ? theme::legal : theme::panelBorder));
        draw_bitmap_fit(get_card_texture(state, cards[index]), r);
        if (hovered && clicked) state.viewing_pile_selected = static_cast<int>(index);
    }
    if (cards.empty()) {
        draw_text(state.font, "Empty.", L.grid_area, static_cast<float>(scale.points(13)), theme::textSecondary, {HAlign::Center, VAlign::Center, false, false});
    }

    const Vector2 offscreen{-1, -1};
    if (button(state.font, L.prev_button, "\xE2\x80\xB9 PREV", nullptr, scale, state.viewing_pile_page > 0 ? mouse : offscreen, clicked)) --state.viewing_pile_page;
    const std::string pageLabel = "PAGE " + std::to_string(state.viewing_pile_page + 1) + "/" + std::to_string(L.grid.page_count);
    draw_text(state.font, pageLabel, L.page_label, static_cast<float>(scale.points(11)), theme::textSecondary, {HAlign::Center, VAlign::Center, false, false});
    if (button(state.font, L.next_button, "NEXT \xE2\x80\xBA", nullptr, scale, state.viewing_pile_page + 1 < L.grid.page_count ? mouse : offscreen, clicked)) ++state.viewing_pile_page;
}

void paint_duel(AppState& state, const Rect& client, const UiScale& scale, Vector2 mouse, bool clicked) {
    // Throttled to roughly the Win32 client's own WM_TIMER(150ms) cadence
    // rather than running every frame (an earlier version of this function
    // did poll every frame, reasoning the cheap early-outs inside
    // poll_player_duel/poll_board_snapshot made it harmless) — this isn't
    // just parity, it materially cuts filesystem churn (~9x fewer
    // request.txt/state.txt opens per second at 60fps), which matters
    // because this project has already hit real antivirus scan-locking
    // interference on rapid temp-file-rename patterns in this exact
    // environment (see submit_action's own retry-on-failure fix, added for
    // the same underlying reason: a duel that hung on "Waiting for the
    // rules engine…" indefinitely).
    constexpr double kPollIntervalSeconds = 0.15;
    if (state.player_process.valid() && GetTime() - state.last_poll_time >= kPollIntervalSeconds) {
        state.last_poll_time = GetTime();
        poll_player_duel(state);
        poll_board_snapshot(state);
        // Multi-select candidates are plain card names, not the verb-prefixed
        // action labels build_action_layout pattern-matches on ("Summon ",
        // "Attack with ", …) — running it here would misclassify them
        // (or, worse, coincidentally match one) and light up zones with the
        // wrong highlight. Its own parsing branch already left action_layout
        // blank for this prompt shape; leave it that way.
        if (!state.multi_select_active && !state.legal_actions.empty()) state.action_layout = build_action_layout(state);
    }

    // Tracks how long the "Waiting for the rules engine…" gap has lasted —
    // a stuck engine (or, before the fix above, a silently-dropped
    // response) otherwise left the player with no recourse but to
    // force-quit the app. See draw_prompt_panel (shows the escalated
    // message) and resolve_duel_click (handles the click to abandon).
    if (state.player_process.valid() && state.legal_actions.empty()) {
        if (state.waiting_since == 0.0) state.waiting_since = GetTime();
    } else {
        state.waiting_since = 0.0;
    }

    // Framerate-independent version of the Win32 client's WM_TIMER-driven
    // `life_display += (life - life_display) * 0.2` (evaluated once per
    // fixed 150ms tick there) — this reaches the same ~20%-per-150ms decay
    // rate regardless of the actual frame time, so the animation speed
    // doesn't depend on framerate the way a flat per-frame lerp would.
    const float dt = GetFrameTime();
    for (int i = 0; i < 2; ++i) {
        const auto idx = static_cast<size_t>(i);
        const double alpha = 1.0 - std::pow(1.0 - 0.2, static_cast<double>(dt) / 0.15);
        state.life_display[idx] += (state.life[idx] - state.life_display[idx]) * alpha;
    }

    const DuelLayout L = compute_duel_layout(client, scale);
    // While the pile viewer overlay is open, it alone gets this frame's
    // click (handled inside draw_pile_viewer itself, called at the very end
    // of this function) — skipping resolve_duel_click here is what stops a
    // click on the viewer's own grid/CLOSE/pager from also landing on
    // whatever's underneath it on the board.
    if (clicked && state.viewing_pile_kind == PileKind::None) resolve_duel_click(state, L, scale, mouse);

    const DuelHover hover = compute_duel_hover(state, L, static_cast<int>(mouse.x), static_cast<int>(mouse.y));
    if (hover.inspect_code) { state.last_inspected_code = hover.inspect_code; state.last_inspected_is_back = false; }
    else if (hover.inspect_is_back) { state.last_inspected_is_back = true; }

    draw_inspector(state, L.inspector, hover, scale, mouse);

    const Rect boardBackdrop{L.opponent_hud.left, L.opponent_hud.top, L.opponent_hud.right, L.player_hud.bottom};
    draw_panel(boardBackdrop, theme::field, true, theme::panelBorder);

    for (int i = 0; i < 5; ++i) {
        const size_t si = static_cast<size_t>(i);
        // Placement targets only ever make sense on an empty zone and attack
        // targets only on an occupied one; enforcing that here means a stale
        // or unexpected action_layout can never glow the wrong zone kind.
        const bool zoneOccupied = state.monsters[0][si].occupied;
        const bool legalMonster = (state.action_layout.all_zone_placement && !state.action_layout.zone_placement_is_spell)
            ? (!zoneOccupied && state.action_layout.zone_to_action[si] >= 0)
            : (!state.action_layout.all_zone_placement && zoneOccupied && (state.action_layout.attack_zone_to_action.count(i) > 0 || state.action_layout.monster_board_actions.count(i) > 0));
        const bool selectedMonster = state.selected_monster_zone == i;
        const bool spellZoneOccupied = state.spells[0][si].occupied;
        const bool legalSpell = (state.action_layout.all_zone_placement && state.action_layout.zone_placement_is_spell)
            ? (!spellZoneOccupied && state.action_layout.zone_to_action[si] >= 0)
            : (!state.action_layout.all_zone_placement && spellZoneOccupied && state.action_layout.spell_board_actions.count(i) > 0);
        const bool selectedSpell = state.selected_spell_zone == i;
        const int candPlayerMonster = find_multi_select_candidate(state, 0, LOCATION_MZONE, static_cast<uint32_t>(i));
        const int candPlayerSpell = find_multi_select_candidate(state, 0, LOCATION_SZONE, static_cast<uint32_t>(i));
        const int candOpponentMonster = find_multi_select_candidate(state, 1, LOCATION_MZONE, static_cast<uint32_t>(i));
        const int candOpponentSpell = find_multi_select_candidate(state, 1, LOCATION_SZONE, static_cast<uint32_t>(i));
        const bool toggledPlayerMonster = candPlayerMonster >= 0 && state.multi_select_toggled[static_cast<size_t>(candPlayerMonster)];
        const bool toggledPlayerSpell = candPlayerSpell >= 0 && state.multi_select_toggled[static_cast<size_t>(candPlayerSpell)];
        const bool toggledOpponentMonster = candOpponentMonster >= 0 && state.multi_select_toggled[static_cast<size_t>(candOpponentMonster)];
        const bool toggledOpponentSpell = candOpponentSpell >= 0 && state.multi_select_toggled[static_cast<size_t>(candOpponentSpell)];
        draw_card_slot(state, L.player_monsters[si], state.monsters[0][si], true, "MONSTER", scale, hover.player_zone == i || selectedMonster, legalMonster, toggledPlayerMonster);
        draw_card_slot(state, L.player_spells[si], state.spells[0][si], false, "SPELL/TRAP", scale, selectedSpell, legalSpell, toggledPlayerSpell);
        draw_card_slot(state, L.opponent_monsters[si], state.monsters[1][si], true, "MONSTER", scale, false, false, toggledOpponentMonster);
        draw_card_slot(state, L.opponent_spells[si], state.spells[1][si], false, "SPELL/TRAP", scale, false, false, toggledOpponentSpell);
    }
    const int candPlayerField = find_multi_select_candidate(state, 0, LOCATION_SZONE, 5);
    const int candOpponentField = find_multi_select_candidate(state, 1, LOCATION_SZONE, 5);
    draw_card_slot(state, L.player_spells[5], state.spells[0][5], false, "FIELD", scale, state.selected_spell_zone == 5,
                   state.spells[0][5].occupied && state.action_layout.spell_board_actions.count(5) > 0,
                   candPlayerField >= 0 && state.multi_select_toggled[static_cast<size_t>(candPlayerField)]);
    draw_card_slot(state, L.opponent_spells[5], state.spells[1][5], false, "FIELD", scale, false, false,
                   candOpponentField >= 0 && state.multi_select_toggled[static_cast<size_t>(candOpponentField)]);
    draw_pile_zone(state.font, L.player_deck, "DECK", state.deck_count[0], scale);
    draw_pile_zone(state.font, L.player_extra, "EXTRA", state.extra_count[0], scale, L.player_extra.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y)));
    draw_pile_zone(state.font, L.player_grave, "GRAVE", state.grave_count[0], scale, L.player_grave.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y)));
    draw_pile_zone(state.font, L.player_banished, "BANISH", state.banished_count[0], scale, L.player_banished.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y)));
    draw_pile_zone(state.font, L.opponent_deck, "DECK", state.deck_count[1], scale);
    draw_pile_zone(state.font, L.opponent_extra, "EXTRA", state.extra_count[1], scale);
    draw_pile_zone(state.font, L.opponent_grave, "GRAVE", state.grave_count[1], scale, L.opponent_grave.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y)));
    draw_pile_zone(state.font, L.opponent_banished, "BANISH", state.banished_count[1], scale, L.opponent_banished.contains(static_cast<int>(mouse.x), static_cast<int>(mouse.y)));

    draw_hud_bar(state, L.opponent_hud, true, scale);
    draw_hud_bar(state, L.player_hud, false, scale);
    draw_turn_indicator(state, L.turn_indicator, scale, mouse);
    draw_hand_row(state, L.opponent_hand, false, hover, scale);
    draw_hand_row(state, L.player_hand, true, hover, scale);
    draw_prompt_panel(state, L.prompt_panel, scale, mouse);
    draw_hand_popup(state, L, scale, mouse);

    // Drawn last so it's on top of everything else on the board — see its
    // own doc-comment, and the resolve_duel_click gate above, for how its
    // clicks stay isolated from the board underneath while it's open.
    draw_pile_viewer(state, client, scale, mouse, clicked);
}

// Centralizes the screen -> music mapping in one place instead of scattering
// "if state.screen == X, play Y" checks through paint functions (which run
// every frame and would restart the track on every one of them). Every
// campaign/menu screen — Title, Hub, Collection, Shop, PackOpening,
// DeckEditor, DeckList, CpuSelect, Options — shares one continuous
// MusicTrack::Menu context; only Duel gets its own track. Called once per
// frame from main() via state.audio.requestMusic(...), which is itself a
// no-op when the requested track is already playing/targeted — see
// AudioManager::requestMusic — so switching between menu screens never
// restarts the menu track.
goat::audio::MusicTrack desired_music_for(Screen screen) {
    switch (screen) {
        case Screen::Duel: return goat::audio::MusicTrack::Duel;
        default: return goat::audio::MusicTrack::Menu;
    }
}

// Every data/asset path in this file (card art, card databases, the title
// background, saved profiles) is relative to the repo root, which is only
// the current directory when launched exactly as `./build/goat-client-rl`
// from the repo root itself — not when double-clicked from `build/` or
// launched via a shortcut. Mirrors src/client/main.cpp's project_root(),
// which chdir's at Win32 client startup for the same reason: if the exe's
// own directory is named `build`, assume its parent is the repo root.
fs::path project_root() {
#ifdef _WIN32
    std::array<wchar_t, 32768> buffer{};
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<unsigned long>(buffer.size()));
    if (length) {
        const fs::path executable(std::wstring(buffer.data(), length));
        if (executable.parent_path().filename() == L"build") return executable.parent_path().parent_path();
    }
#endif
    return fs::current_path();
}

} // namespace

int main() {
    fs::current_path(project_root());

    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_MSAA_4X_HINT); // must be set before InitWindow to take effect
    InitWindow(1440, 900, "Project GOAT");
    SetWindowState(FLAG_WINDOW_RESIZABLE);
    SetExitKey(KEY_NULL); // Escape shouldn't quit a game window
    SetTargetFPS(60);

    AppState state;
    state.font = GetFontDefault();

    // Audio device init/shutdown happens exactly once here, not per screen
    // change (see AudioManager::initialize's own doc-comment) — every SFX/
    // music file is loaded once up front too, so a missing optional file is
    // discovered (and logged) now rather than stalling a screen transition
    // later. set_ui_click_source wires the shared button() widget's click
    // sound to this one AudioManager instance without threading it through
    // every button() call site.
    state.audio.initialize();
    goat::audio::set_ui_click_source(&state.audio);

    while (!WindowShouldClose()) {
        const int width = GetScreenWidth();
        const int height = GetScreenHeight();
        const Rect client{0, 0, width, height};
        const Vector2 dpiScale = GetWindowScaleDPI();
        const UiScale scale = compute_scale(width, height, static_cast<int>(96.0f * dpiScale.y));
        const Vector2 mouse = GetMousePosition();
        const bool clicked = IsMouseButtonReleased(MOUSE_BUTTON_LEFT);

        // Music state belongs here, in the application layer, not in any
        // paint_* function (which runs every frame and would restart the
        // track) — see desired_music_for's own doc-comment. requestMusic is
        // idempotent when the target hasn't changed, and update() advances
        // both music-stream buffering and any in-progress fade every frame
        // regardless of which screen is active.
        state.audio.requestMusic(desired_music_for(state.screen));
        state.audio.update();

        BeginDrawing();
        ClearBackground(theme::background);
        switch (state.screen) {
            case Screen::Title: paint_title(state, client, scale, mouse, clicked); break;
            case Screen::Hub: paint_hub(state, client, scale, mouse, clicked); break;
            case Screen::Collection: paint_collection(state, client, scale, mouse, clicked); break;
            case Screen::Shop: paint_shop(state, client, scale, mouse, clicked); break;
            case Screen::PackOpening: paint_pack_opening(state, client, scale, mouse, clicked); break;
            case Screen::DeckEditor: paint_deck_editor(state, client, scale, mouse, clicked); break;
            case Screen::DeckList: paint_deck_list(state, client, scale, mouse, clicked); break;
            case Screen::CpuSelect: paint_cpu_select(state, client, scale, mouse, clicked); break;
            case Screen::Duel: paint_duel(state, client, scale, mouse, clicked); break;
            case Screen::Options: paint_options(state, client, scale, mouse, clicked); break;
        }
        EndDrawing();
    }

    // Matches src/client/main.cpp's WM_DESTROY handler: a hard kill with no
    // graceful shutdown signal to goat-sim, since the app is closing anyway.
    // Normal duel completion (poll_player_duel seeing result.txt or a dead
    // process) already closes the process there instead.
    if (state.player_process.valid()) goat::process::terminate(state.player_process);

    // Sounds/music must be unloaded and the audio device closed before the
    // window does — see AudioManager::shutdown's own doc-comment; it's safe
    // to call again from AppState's destructor afterward (idempotent).
    state.audio.shutdown();
    state.textures.unload_all();
    CloseWindow();
    return 0;
}
