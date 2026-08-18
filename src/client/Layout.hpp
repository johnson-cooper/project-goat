#pragma once

// Resolution-independent layout primitives for the native GDI client.
//
// The client used to position every widget with literal pixel constants tuned
// for a single 920x690 window. That breaks the moment the window is resized or
// opened on a different-DPI monitor: cards stay tiny, zones overlap text, and
// hitboxes drift away from what's actually painted.
//
// The fix here is the classic "rect cutting" layout style: start from the full
// client rect and repeatedly slice fixed- or proportional-size pieces off one
// edge, shrinking the remainder each time. Every screen (duel, hub, shop, ...)
// derives its widget rects this way from the *current* client rect, so paint
// and hit-testing always agree and resizing just recomputes the same cuts.

#include <windows.h>

#include <algorithm>
#include <cstdint>

namespace goat::ui {

struct Rect {
    int left{};
    int top{};
    int right{};
    int bottom{};

    int width() const { return right - left; }
    int height() const { return bottom - top; }
    int center_x() const { return (left + right) / 2; }
    int center_y() const { return (top + bottom) / 2; }
    bool contains(int x, int y) const { return x >= left && x < right && y >= top && y < bottom; }
    bool empty() const { return width() <= 0 || height() <= 0; }
    RECT win32() const { return RECT{left, top, right, bottom}; }
};

inline Rect from_client(const RECT& client) { return {client.left, client.top, client.right, client.bottom}; }

inline Rect inset(Rect r, int amount) { return {r.left + amount, r.top + amount, r.right - amount, r.bottom - amount}; }
inline Rect inset(Rect r, int horizontal, int vertical) { return {r.left + horizontal, r.top + vertical, r.right - horizontal, r.bottom - vertical}; }

// Slice a fixed-size piece off one edge of `r`, shrinking `r` to the remainder.
// Clamped so a too-small window never produces an inverted (negative-size) rect.
inline Rect cut_top(Rect& r, int amount) { const int y = std::clamp(r.top + amount, r.top, r.bottom); Rect slice{r.left, r.top, r.right, y}; r.top = y; return slice; }
inline Rect cut_bottom(Rect& r, int amount) { const int y = std::clamp(r.bottom - amount, r.top, r.bottom); Rect slice{r.left, y, r.right, r.bottom}; r.bottom = y; return slice; }
inline Rect cut_left(Rect& r, int amount) { const int x = std::clamp(r.left + amount, r.left, r.right); Rect slice{r.left, r.top, x, r.bottom}; r.left = x; return slice; }
inline Rect cut_right(Rect& r, int amount) { const int x = std::clamp(r.right - amount, r.left, r.right); Rect slice{x, r.top, r.right, r.bottom}; r.right = x; return slice; }

// A width x height rect centered within `r`, never exceeding it.
inline Rect centered(const Rect& r, int width, int height) {
    width = std::min(width, r.width());
    height = std::min(height, r.height());
    const int x = r.left + (r.width() - width) / 2;
    const int y = r.top + (r.height() - height) / 2;
    return {x, y, x + width, y + height};
}

// Real Yu-Gi-Oh card stock is 59mm x 86mm. Every card/zone rect derives its width
// from this ratio so hitboxes and rendered art stay aligned at any window size
// instead of being stretched independently on X and Y.
constexpr double kCardAspect = 59.0 / 86.0;

inline Rect card_from_height(int center_x, int top, int height) {
    const int width = static_cast<int>(height * kCardAspect);
    return {center_x - width / 2, top, center_x - width / 2 + width, top + height};
}

inline Rect card_from_width(int center_x, int top, int width) {
    const int height = static_cast<int>(width / kCardAspect);
    return {center_x - width / 2, top, center_x - width / 2 + width, top + height};
}

// Combines a window-size-relative scale with monitor DPI so both card/panel
// sizing and font point sizes track the same factor. Fonts are always created
// at their true scaled point size (never bitmap-stretched), which keeps text
// crisp under Windows display scaling (100/125/150/200%).
struct UiScale {
    float layout{1.0f};
    int dpi{96};

    float dpi_factor() const { return dpi / 96.0f; }
    int points(int base) const { return std::max(8, static_cast<int>(base * layout * dpi_factor() + 0.5f)); }
    int px(int base) const { return static_cast<int>(base * layout + 0.5f); }
};

// 1600x900 is the design resolution the layout is tuned at; smaller/larger
// windows scale smoothly from it instead of snapping between fixed buckets.
inline UiScale compute_scale(int width, int height, int dpi) {
    const float sx = width / 1600.0f;
    const float sy = height / 900.0f;
    UiScale result;
    result.layout = std::clamp(std::min(sx, sy), 0.6f, 1.7f);
    result.dpi = dpi;
    return result;
}

} // namespace goat::ui
