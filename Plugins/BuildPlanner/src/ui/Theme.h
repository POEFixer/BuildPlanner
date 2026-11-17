#pragma once
// BuildPlanner - colours and drawing helpers. The overlays and the settings tabs both call the
// signatures below.
// BP_TESTS_EXCLUDE: ImGui; not built into tests/.
// Theme.h - the plugin's colours and the drawing helpers every overlay and tab shares:
// rarity and gem colours, chips, badges, marks and the measured card drawn on the game. Header-only.
//
// Rules: every size derives from ImGui::GetFontSize() (Em()) and measured text, never a fixed pixel
// column. Helpers that take an ImDrawList draw in the list's coordinates (game client pixels for the
// overlays) and snap to whole pixels. RENDER thread only (they call ImGui).
//
// One visual language (the three overlays, the "Build skills" window and the tabs use the same parts):
//   Card   a translucent dark panel with a 1 px light border; a small gold caption, text rows, thin
//          rules, rows of chips, a verdict row with a mark. Everything a hint says in words is a Card.
//   Chip   a rounded pill in the colour of what it names (a gem's attribute colour), optionally led by
//          a mark.
//   Badge  a dark disc outlined in a colour with one or two characters (a step number, "II", "S").
//   Mark   tick / dot / arrow / cross / plus on a dark disc: the verdict on a slot or a socket.
//
// Nothing of ours is ever drawn on a tooltip of the game (Avoid): a mark, a ring or a line under one
// is left out, a card goes beside it, and the "Build skills" window steps aside while one is on it.
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../../../POEFixer/plugin_sdk/PluginSDK.h"
#include "../../../../POEFixer/plugin_sdk/PluginUi.h"

#include "../core/SkillsReader.h"   // SkillsFrame
#include "../core/TreeReader.h"     // TreePose, and through it GearReader.h (GameTooltip, GearFrame)
#include "../core/Types.h"

namespace bp::theme {

// ---- colours (IM_COL32 layout) ----
inline constexpr ImU32 kText = IM_COL32(0xE6, 0xE6, 0xE6, 255);
inline constexpr ImU32 kTextDim = IM_COL32(0xA0, 0xA0, 0xA0, 255);
inline constexpr ImU32 kCaption = IM_COL32(0xC8, 0xAA, 0x5A, 255);   // card captions ("BUILD ITEM")
inline constexpr ImU32 kMod = IM_COL32(0x88, 0x88, 0xFF, 255);       // explicit / implicit mod text
inline constexpr ImU32 kEnchant = IM_COL32(0xB4, 0xB4, 0xFF, 255);   // enchant and rune lines
inline constexpr ImU32 kCrafted = IM_COL32(0xB4, 0xE6, 0xFF, 255);
inline constexpr ImU32 kGood = IM_COL32(0x46, 0xDC, 0x5A, 255);      // green tick: matches the build
inline constexpr ImU32 kWarn = IM_COL32(0xFF, 0xB4, 0x3C, 255);      // amber dot: differs / missing
inline constexpr ImU32 kBad = IM_COL32(0xFF, 0x46, 0x3C, 255);       // red cross: not in the build
inline constexpr ImU32 kBadgeFill = IM_COL32(12, 14, 18, 235);
inline constexpr ImU32 kDesecrated = IM_COL32(0x7C, 0xD6, 0x8E, 255);   // desecrated mod lines
inline constexpr ImU32 kFractured = IM_COL32(0xC8, 0xB4, 0x7A, 255);    // fractured mod lines
inline constexpr ImU32 kRule = IM_COL32(255, 255, 255, 40);             // the thin rule inside a card
inline constexpr ImU32 kUnderlay = IM_COL32(0, 0, 0, 150);              // dark stroke under rings and lines
inline constexpr ImU32 kSetOne = IM_COL32(0xFF, 0x9C, 0x8C, 255);       // Weapon Set I badge (the game's red, pale)
inline constexpr ImU32 kSetTwo = IM_COL32(0x9C, 0xE8, 0x8C, 255);       // Weapon Set II badge (the game's green, pale)

// Item rarity colours.
inline ImU32 RarityColor(Rarity r) {
    switch (r) {
    case Rarity::Magic: return IM_COL32(0x88, 0x88, 0xFF, 255);
    case Rarity::Rare: return IM_COL32(0xFF, 0xFF, 0x77, 255);
    case Rarity::Unique: return IM_COL32(0xAF, 0x60, 0x25, 255);
    case Rarity::Relic: return IM_COL32(0x82, 0xAD, 0x6A, 255);
    case Rarity::Normal:
    default: return IM_COL32(0xC8, 0xC8, 0xC8, 255);
    }
}

// Gem colours by attribute; None = neutral.
inline ImU32 GemColor(Attr a) {
    switch (a) {
    case Attr::Str: return IM_COL32(0xC8, 0x55, 0x3D, 255);
    case Attr::Dex: return IM_COL32(0x5B, 0xB8, 0x5B, 255);
    case Attr::Int: return IM_COL32(0x5B, 0x8D, 0xEF, 255);
    case Attr::None:
    default: return IM_COL32(0xC8, 0xC8, 0xC8, 255);
    }
}

// The letter badge colour of an attribute choice (S red, D green, I blue).
inline ImU32 AttrColor(Attr a) { return GemColor(a); }

// The colour of one mod line by its ModTag bits.
inline ImU32 ModLineColor(uint32_t tags) {
    if (tags & (kModEnchant | kModRune)) return kEnchant;
    if (tags & kModCrafted) return kCrafted;
    if (tags & kModDesecrated) return kDesecrated;
    if (tags & kModFractured) return kFractured;
    return kMod;
}

// `color` with its alpha multiplied by `mul` (0..1).
inline ImU32 WithAlpha(ImU32 color, float mul) {
    const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(mul, 0.0f, 1.0f);
    return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a + 0.5f) << IM_COL32_A_SHIFT);
}
// `color` with full alpha (text and badge outlines in a ring's colour).
inline ImU32 Opaque(ImU32 color) { return color | IM_COL32_A_MASK; }

// ---- sizes ----
inline float Em() { return ImGui::GetFontSize(); }   // the unit of every size here
// The game's side panels (the inventory at the right edge of the client, the Skills panel at the left)
// are about 0.62 x the client height wide at every client size (measured on the 0.5.5 client: the
// inventory measures 369.75 of 600 and 498.55 of 809). A little more, so that nothing lands on its
// edge. Used ONLY where the panel's own rectangle is not known: the readers give the real one
// (GearFrame::panel, SkillsFrame::panel) whenever the panel was found.
inline constexpr float kSidePanelWidthPerHeight = 0.63f;
// A font size of `scale` em in whole pixels (fractional sizes blur the glyphs).
inline float FontPx(float scale = 1.0f) { return std::max(8.0f, PluginUi::SnapPx(Em() * scale)); }
inline ImVec2 ToIm(Vec2 v) { return ImVec2(v.x, v.y); }

// ---- untrusted text ----
// Names come from a pasted build: a card, a chip or a window row never grows past its place for them.
// Shortens `text` by a few characters and ends it with "..."; false when nothing is left to cut.
inline bool CutTail(std::string& text) {
    size_t n = text.size();
    if (n >= 3 && text.compare(n - 3, 3, "...") == 0) n -= 3;
    if (n <= 1) return false;
    size_t cut = n > 4 ? n - 4 : 1;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;   // a UTF-8 boundary
    if (cut == 0) return false;
    text.resize(cut);
    text += "...";
    return true;
}
// `text` cut to at most `maxBytes` on a UTF-8 character boundary, with "..." when it was cut: the cap
// on a build's own texts (names, titles, notes, mod lines) before they are measured.
inline std::string Shortened(std::string_view text, size_t maxBytes) {
    if (text.size() <= maxBytes) return std::string(text);
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80) --n;
    return std::string(text.substr(0, n)) + "...";
}
// `text` as it is when it is at most `maxWidth` wide in the current font, else cut and ended with "...".
inline std::string FitText(std::string_view text, float maxWidth) {
    std::string out(text);
    while (ImGui::CalcTextSize(out.c_str()).x > maxWidth && CutTail(out)) {
    }
    return out;
}

// ---- marks ----
// tick   matches the build (a slot's item, a socket's support)         green
// dot    differs / missing (a slot whose item is not the build's)      amber
// arrow  the build uses another tier of this support ("Other tier")       amber
// cross  this support is not in the build                              red
// plus   a support of the build the skill does not have yet ("Add")    amber
enum class Mark : uint8_t { None = 0, Tick, Dot, Arrow, Cross, Plus };

inline ImU32 MarkColor(Mark m) {
    switch (m) {
    case Mark::Tick: return kGood;
    case Mark::Dot:
    case Mark::Arrow:
    case Mark::Plus: return kWarn;
    case Mark::Cross: return kBad;
    case Mark::None:
    default: return kTextDim;
    }
}

// The mark's strokes alone, inside a circle of `radius` around `centre` (no disc): inside chips and
// card rows, where the ground is already dark.
inline void DrawMarkGlyph(ImDrawList* dl, const ImVec2& centre, float radius, Mark mark, ImU32 color) {
    if (!dl || mark == Mark::None || !(radius > 0.0f)) return;
    const ImVec2 c = centre;
    const float w = std::max(1.5f, radius * 0.28f);
    switch (mark) {
    case Mark::Tick: {
        const ImVec2 pts[3] = {ImVec2(c.x - radius * 0.50f, c.y + radius * 0.02f), ImVec2(c.x - radius * 0.12f, c.y + radius * 0.40f),
                               ImVec2(c.x + radius * 0.52f, c.y - radius * 0.36f)};
        dl->AddPolyline(pts, 3, color, w, ImDrawFlags_None);
        break;
    }
    case Mark::Dot: dl->AddCircleFilled(c, radius * 0.5f, color, 12); break;
    case Mark::Arrow: {
        dl->AddLine(ImVec2(c.x, c.y + radius * 0.50f), ImVec2(c.x, c.y - radius * 0.42f), color, w);
        const ImVec2 head[3] = {ImVec2(c.x - radius * 0.42f, c.y - radius * 0.02f), ImVec2(c.x, c.y - radius * 0.48f),
                                ImVec2(c.x + radius * 0.42f, c.y - radius * 0.02f)};
        dl->AddPolyline(head, 3, color, w, ImDrawFlags_None);
        break;
    }
    case Mark::Cross: {
        const float d = radius * 0.40f;
        dl->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), color, w);
        dl->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x + d, c.y - d), color, w);
        break;
    }
    case Mark::Plus: {
        const float d = radius * 0.48f;
        dl->AddLine(ImVec2(c.x - d, c.y), ImVec2(c.x + d, c.y), color, w);
        dl->AddLine(ImVec2(c.x, c.y - d), ImVec2(c.x, c.y + d), color, w);
        break;
    }
    case Mark::None:
    default: break;
    }
}

// The dark disc under a mark or a badge, edged in the mark's colour so it reads on a dark ground too.
inline void DrawDisc(ImDrawList* dl, const ImVec2& centre, float radius, ImU32 edge) {
    if (!dl) return;
    dl->AddCircleFilled(centre, radius, kBadgeFill, 20);
    dl->AddCircle(centre, radius, WithAlpha(edge, 0.7f), 20, 1.0f);
}

// A mark on its disc, in the mark's own colour; `radius` is the disc's radius. None draws nothing.
inline void DrawMark(ImDrawList* dl, const ImVec2& centre, float radius, Mark mark) {
    if (!dl || mark == Mark::None) return;
    const ImVec2 c = PluginUi::SnapPx(centre);
    DrawDisc(dl, c, radius, MarkColor(mark));
    DrawMarkGlyph(dl, c, radius, mark, MarkColor(mark));
}

// ---- draw-list helpers (overlays and info windows) ----

// A chip: a rounded pill with tinted fill, a 1 px border and the text in `color`.
inline ImVec2 ChipSize(const char* text) {
    const ImVec2 t = ImGui::CalcTextSize(text ? text : "");
    return ImVec2(PluginUi::SnapPx(t.x + Em() * 0.9f), PluginUi::SnapPx(t.y + Em() * 0.3f));
}
// The same chip led by a mark (the tick on a support the character already has). None = a plain chip.
inline ImVec2 MarkChipSize(const char* text, Mark mark) {
    ImVec2 size = ChipSize(text);
    if (mark != Mark::None) size.x += PluginUi::SnapPx(Em() * 0.85f);
    return size;
}
inline void DrawMarkChip(ImDrawList* dl, const ImVec2& pos, const char* text, ImU32 color, Mark mark, float fillAlpha = 0.22f) {
    if (!dl || !text) return;
    const ImVec2 size = MarkChipSize(text, mark);
    const ImVec2 a = PluginUi::SnapPx(pos), b(a.x + size.x, a.y + size.y);
    const float rounding = size.y * 0.5f;
    dl->AddRectFilled(a, b, IM_COL32(12, 14, 18, 200), rounding);
    dl->AddRectFilled(a, b, WithAlpha(color, fillAlpha), rounding);
    dl->AddRect(a, b, WithAlpha(color, 0.65f), rounding);
    const ImVec2 t = ImGui::CalcTextSize(text);
    float textLeft = a.x + Em() * 0.45f;
    if (mark != Mark::None) {
        const float lead = PluginUi::SnapPx(Em() * 0.85f);
        DrawMarkGlyph(dl, ImVec2(PluginUi::SnapPx(a.x + Em() * 0.3f + lead * 0.5f), PluginUi::SnapPx(a.y + size.y * 0.5f)), Em() * 0.42f, mark,
                      MarkColor(mark));
        textLeft += lead;
    }
    dl->AddText(ImVec2(PluginUi::SnapPx(textLeft), PluginUi::SnapPx(a.y + (size.y - t.y) * 0.5f)), color, text);
}
inline void DrawChip(ImDrawList* dl, const ImVec2& pos, const char* text, ImU32 color, float fillAlpha = 0.22f) {
    DrawMarkChip(dl, pos, text, color, Mark::None, fillAlpha);
}

// A badge: a dark disc outlined in `color` with a short centred text ("3", "II", "S").
// `scale` multiplies the font size (step 1 uses a larger badge).
inline float BadgeRadius(float scale = 1.0f) { return PluginUi::SnapPx(Em() * 0.62f * scale); }
inline void DrawBadge(ImDrawList* dl, const ImVec2& centre, const char* text, ImU32 color, float scale = 1.0f) {
    if (!dl || !text) return;
    const float r = BadgeRadius(scale);
    const ImVec2 c = PluginUi::SnapPx(centre);
    dl->AddCircleFilled(c, r, kBadgeFill, 20);
    dl->AddCircle(c, r, color, 20, std::max(1.0f, Em() * 0.08f));
    ImFont* font = ImGui::GetFont();
    const float fs = FontPx(0.8f * scale);
    const ImVec2 t = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
    dl->AddText(font, fs, ImVec2(PluginUi::SnapPx(c.x - t.x * 0.5f), PluginUi::SnapPx(c.y - t.y * 0.5f)), color, text);
}

// The same badge inverted: a disc filled with `fill` and a dark character. For what names a colour
// itself (the attribute to pick: S red, D green, I blue), so that it is not read as a number badge.
inline void DrawSolidBadge(ImDrawList* dl, const ImVec2& centre, const char* text, ImU32 fill, float scale = 1.0f) {
    if (!dl || !text) return;
    const float r = BadgeRadius(scale);
    const ImVec2 c = PluginUi::SnapPx(centre);
    dl->AddCircleFilled(c, r, Opaque(fill), 20);
    dl->AddCircle(c, r, kBadgeFill, 20, std::max(1.0f, Em() * 0.08f));
    ImFont* font = ImGui::GetFont();
    const float fs = FontPx(0.8f * scale);
    const ImVec2 t = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
    dl->AddText(font, fs, ImVec2(PluginUi::SnapPx(c.x - t.x * 0.5f), PluginUi::SnapPx(c.y - t.y * 0.5f)), IM_COL32(10, 12, 16, 255), text);
}

// The radius of a mark drawn on an element of the game (an equipment slot, a gem socket): `share` of
// the element's shorter side, so that it follows the game's own scale; never larger than the text
// beside it would make it, whatever the font scale.
inline float MarkRadiusOn(const Rect& element, float share) {
    return std::max(3.0f, std::min(std::min(element.w, element.h) * share, Em() * 0.45f));
}

// Marks, each on a dark disc so they read on any ground; `radius` is the disc's radius (see Mark).
// DrawRing is a plain circle outline, not a mark.
inline void DrawTick(ImDrawList* dl, const ImVec2& centre, float radius, ImU32 color = kGood) {
    if (!dl) return;
    const ImVec2 c = PluginUi::SnapPx(centre);
    DrawDisc(dl, c, radius, color);
    DrawMarkGlyph(dl, c, radius, Mark::Tick, color);
}
inline void DrawDot(ImDrawList* dl, const ImVec2& centre, float radius, ImU32 color = kWarn) {
    if (!dl) return;
    const ImVec2 c = PluginUi::SnapPx(centre);
    DrawDisc(dl, c, radius, color);
    DrawMarkGlyph(dl, c, radius, Mark::Dot, color);
}
inline void DrawArrowUp(ImDrawList* dl, const ImVec2& centre, float radius, ImU32 color = kWarn) {
    if (!dl) return;
    const ImVec2 c = PluginUi::SnapPx(centre);
    DrawDisc(dl, c, radius, color);
    DrawMarkGlyph(dl, c, radius, Mark::Arrow, color);
}
inline void DrawCross(ImDrawList* dl, const ImVec2& centre, float radius, ImU32 color = kBad) {
    if (!dl) return;
    const ImVec2 c = PluginUi::SnapPx(centre);
    DrawDisc(dl, c, radius, color);
    DrawMarkGlyph(dl, c, radius, Mark::Cross, color);
}
inline void DrawRing(ImDrawList* dl, const ImVec2& centre, float radius, ImU32 color = kBad, float thickness = 0.0f) {
    if (!dl) return;
    dl->AddCircle(PluginUi::SnapPx(centre), radius, color, 0, thickness > 0.0f ? thickness : std::max(1.5f, Em() * 0.12f));
}
// A ring on the game picture: the same circle over a dark stroke, so it reads on bright art too.
inline void DrawRingOutlined(ImDrawList* dl, const ImVec2& centre, float radius, ImU32 color, float thickness) {
    if (!dl) return;
    dl->AddCircle(centre, radius, WithAlpha(kUnderlay, static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f), 0, thickness + 2.0f);
    dl->AddCircle(centre, radius, color, 0, thickness);
}

// Keeps a box of `size` inside the client area (with a small margin); returns its top-left.
inline ImVec2 ClampToClient(ImVec2 pos, const ImVec2& size, float clientW, float clientH) {
    const float m = PluginUi::SnapPx(Em() * 0.25f);
    pos.x = std::min(pos.x, clientW - size.x - m);
    pos.y = std::min(pos.y, clientH - size.y - m);
    pos.x = std::max(pos.x, m);
    pos.y = std::max(pos.y, m);
    return PluginUi::SnapPx(pos);
}

// Where to put a box next to a rectangle it must not cover (the game's tooltip, a slot, a socket):
// left of it when there is room, else right of it, else below it, else above it; then clamped.
inline ImVec2 PlaceBeside(const Rect& avoid, const ImVec2& size, float clientW, float clientH) {
    const float gap = PluginUi::SnapPx(Em() * 0.4f);
    ImVec2 pos;
    if (avoid.x - gap - size.x >= 0.0f) pos = ImVec2(avoid.x - gap - size.x, avoid.y);
    else if (avoid.Right() + gap + size.x <= clientW) pos = ImVec2(avoid.Right() + gap, avoid.y);
    else if (avoid.Bottom() + gap + size.y <= clientH) pos = ImVec2(avoid.x, avoid.Bottom() + gap);
    else pos = ImVec2(avoid.x, avoid.y - gap - size.y);
    return ClampToClient(pos, size, clientW, clientH);
}

// Does the box (pos, size) touch `r`? A rect that is not ok touches nothing.
inline bool Overlaps(const Rect& r, const ImVec2& pos, const ImVec2& size) {
    return r.ok && pos.x < r.Right() && pos.x + size.x > r.x && pos.y < r.Bottom() && pos.y + size.y > r.y;
}

// The box of the build item card the gear overlay drew in this frame's DrawUI (ok == false: none).
// OverlayGear.cpp resets it at the start of its call and sets it when it draws the card; the Skills
// panel overlay draws no socket mark and no card on top of it. One instance for the whole DLL.
inline Rect& HoverCardBox() {
    static Rect box;
    return box;
}

// The box of the "Build skills" window as it was drawn over the game in the LAST DrawUI (ok == false:
// not shown, or shown outside the game: the host then draws it in a window of its own). In game
// client pixels. SkillsWindow.cpp resets it at the start of its call and sets it when the window is
// drawn; it runs after the three overlays, which draw no ring, mark or card on top of the window.
inline Rect& SkillsWindowBox() {
    static Rect box;
    return box;
}

// ---- the Skills panel's column of cards ----
// The inventory panel's left edge: its own rectangle when the gear reader has it (GearFrame::panel),
// else where the panel is anchored (the client's right edge less kSidePanelWidthPerHeight).
inline float InventoryLeft(const GearFrame& gear, float clientW, float clientH) {
    return gear.panel.ok ? gear.panel.x : clientW - clientH * kSidePanelWidthPerHeight;
}
// The column of cards the Skills panel overlay draws right beside the game's Skills panel
// (OverlaySkills.cpp), from the panel's right edge and the x it must end before (the inventory
// panel's left edge while that panel is open, else the client's right edge). ONE rule for the
// overlay and for the "Build skills" window, whose default place keeps off the column.
struct SkillsColumn {
    float left = 0.0f;        // the cards' left edge
    float textWidth = 0.0f;   // the widest a card's text may be (Card's maxTextWidth)
    bool ok = false;          // false: no room beside the panel, no column is drawn
    // The right edge of the widest card (its text plus the panel padding on both sides).
    float Right() const { return left + textWidth + PluginUi::PanelPadding().x * 2.0f; }
};
inline SkillsColumn SkillsColumnBeside(float panelRight, float endX) {
    SkillsColumn c;
    const float margin = PluginUi::SnapPx(Em() * 0.25f), gap = PluginUi::SnapPx(Em() * 0.4f);
    c.left = PluginUi::SnapPx(panelRight + gap);
    c.textWidth = std::min(Em() * 17.0f, endX - margin - c.left - PluginUi::PanelPadding().x * 2.0f);
    c.ok = c.textWidth >= Em() * 8.0f;   // else no room (both panels open in a small window)
    return c;
}

// ---- what nothing of ours may cover ----
// A few rectangles of the client (the game's tooltips, our own window): a mark or a ring under one is
// left out, a card is placed beside them. Fixed size, no allocation.
struct Avoid {
    static constexpr int kMax = 6;
    Rect rects[kMax];
    int count = 0;

    // Adds a usable rectangle once (the three overlay frames report the same tooltip).
    void Add(const Rect& r) {
        if (!r.ok || !(r.w > 0.0f) || !(r.h > 0.0f) || count >= kMax) return;
        for (int i = 0; i < count; ++i) {
            const Rect& o = rects[i];
            if (std::fabs(o.x - r.x) < 1.0f && std::fabs(o.y - r.y) < 1.0f && std::fabs(o.w - r.w) < 1.0f && std::fabs(o.h - r.h) < 1.0f) return;
        }
        rects[count++] = r;
    }
    void Add(const GameTooltip& t) {
        if (t.ok) Add(t.rect);
    }
    bool Empty() const { return count == 0; }
    // Does the box (pos, size) touch one of them?
    bool Hits(const ImVec2& pos, const ImVec2& size) const {
        for (int i = 0; i < count; ++i)
            if (Overlaps(rects[i], pos, size)) return true;
        return false;
    }
    // Does the disc touch one of them (its bounding box: a mark never straddles an edge)?
    bool HitsDisc(const ImVec2& centre, float radius) const {
        return Hits(ImVec2(centre.x - radius, centre.y - radius), ImVec2(radius * 2.0f, radius * 2.0f));
    }
};

// The tooltips the game shows this frame, from whichever overlay frame was read (all three apply the
// same rule, GearReader.h ReadGameTooltip; a frame that was not read reports none), plus the hovered
// equipment slot's own tooltip element (GearFrame::gameTooltip).
inline Avoid GameTooltips(const TreePose& pose, const GearFrame& gear, const SkillsFrame& skills) {
    Avoid avoid;
    avoid.Add(pose.tooltip);
    avoid.Add(gear.tooltip);
    avoid.Add(gear.gameTooltip);
    avoid.Add(skills.tooltip);
    return avoid;
}

// ---- names from the build ----
// The same rules as the settings tabs (SettingsCommon.h sui::GemName / sui::CleanTitle), so that a
// name reads the same on the game and on the page.
// A gem's name as shown: Path of Building names a few gems with a placeholder the game fills in
// ("Spectre: {0}" is "Spectre: Bloodthief Wasp" in the game). The placeholder goes, with the
// separator in front of it: "Spectre".
inline std::string GemLabel(std::string_view name) {
    const size_t at = name.find("{0}");
    if (at == std::string_view::npos) return std::string(name);
    std::string out = std::string(name.substr(0, at)) + std::string(name.substr(at + 3));
    while (!out.empty() && (out.back() == ' ' || out.back() == ':' || out.back() == '-')) out.pop_back();
    return out.empty() ? std::string(name) : out;
}
// A tree's or a set's title as shown: without Path of Building's loadout tag ("Level 98 {3}" is
// "Level 98"; the rule of Stage::title, Loadouts.h: the FIRST brace group made of letters, digits and
// commas goes, the rest is trimmed). "" stays "".
inline std::string TitleLabel(std::string_view title) {
    const auto tagChar = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ','; };
    std::string out(title);
    for (size_t open = out.find('{'); open != std::string::npos; open = out.find('{', open + 1)) {
        size_t k = open + 1;
        while (k < out.size() && tagChar(out[k])) ++k;
        if (k == open + 1 || k >= out.size() || out[k] != '}') continue;
        out.erase(open, k - open + 1);
        break;
    }
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!out.empty() && blank(out.back())) out.pop_back();
    size_t lead = 0;
    while (lead < out.size() && blank(out[lead])) ++lead;
    out.erase(0, lead);
    return out;
}

// A card drawn on the game: collect its rows, ask for the size (to place it), then draw it.
// Background PluginUi::Panel, padding PluginUi::PanelPadding(), lines wrapped at the card's width.
//   Card card(Em() * 22.0f);
//   card.Caption("BUILD ITEM"); card.Line(name, RarityColor(r)); card.Separator(); card.Line(mod, kMod);
//   const ImVec2 size = card.Size();
//   card.Draw(dl, PlaceBeside(tooltip, size, w, h), opacity);
class Card {
public:
    // maxTextWidth: the widest a line may be before it wraps (pixels).
    explicit Card(float maxTextWidth) : m_maxWidth(std::max(maxTextWidth, Em() * 6.0f)) {}

    void Caption(std::string_view text) { Add(Kind::Text, text, kCaption, 0.85f); }        // small heading line
    void Line(std::string_view text, ImU32 color = kText) { Add(Kind::Text, text, color, 1.0f); }
    void Small(std::string_view text, ImU32 color = kTextDim) { Add(Kind::Text, text, color, 0.9f); }
    void Separator() { Add(Kind::Rule, {}, 0, 1.0f); }                                     // a thin rule with spacing
    bool Empty() const { return m_rows.empty(); }

    // A text row led by a mark (the verdict that ends a card: tick + "You wear this item").
    void MarkLine(Mark mark, std::string_view text, ImU32 color) {
        Add(Kind::Text, text, color, 1.0f);
        m_rows.back().mark = mark;
    }
    // Several pieces of text on one line, each in its colour, half an em apart ("Take 5  Refund 2").
    // A piece that does not fit starts the next line; a piece is never split.
    void Inline(std::initializer_list<std::pair<std::string_view, ImU32>> parts, float scale = 1.0f) {
        Add(Kind::Flow, {}, 0, scale);
        for (const auto& p : parts)
            if (!p.first.empty()) AddItem(p.first, p.second, Mark::None, false);
    }
    // A row of chips that wraps at the card's width, led by a small label ("Add"): call AddChip after it.
    void Chips(std::string_view label, ImU32 labelColor = kTextDim) {
        Add(Kind::Flow, {}, 0, 0.9f);
        m_rows.back().chips = true;
        if (!label.empty()) AddItem(label, labelColor, Mark::None, false);
    }
    void AddChip(std::string_view text, ImU32 color, Mark mark = Mark::None) {
        if (m_rows.empty() || m_rows.back().kind != Kind::Flow) Chips({});
        AddItem(text, color, mark, true);
    }
    // Plain text after the chips of the current row ("+3 more").
    void AddPiece(std::string_view text, ImU32 color = kTextDim) {
        if (m_rows.empty() || m_rows.back().kind != Kind::Flow) Chips({});
        AddItem(text, color, Mark::None, false);
    }
    // A coloured strip down the left edge (the item's rarity, the state of a passive).
    void Accent(ImU32 color) { m_accent = color; }
    // Cards of one column share a width.
    void MinTextWidth(float width) {
        m_minWidth = std::min(width, m_maxWidth);
        m_measured = false;
    }
    // The card's outer size, padding included. Measured once (rows added later re-measure).
    ImVec2 Size() {
        Measure();
        const ImVec2 pad = PluginUi::PanelPadding();
        return ImVec2(PluginUi::SnapPx(m_width + pad.x * 2.0f), PluginUi::SnapPx(m_height + pad.y * 2.0f));
    }

    // Draws the card with its top-left at `pos`. opacity = the panel's background opacity (0..1).
    void Draw(ImDrawList* dl, const ImVec2& pos, float opacity) {
        if (!dl || m_rows.empty()) return;
        const ImVec2 size = Size();
        const ImVec2 a = PluginUi::SnapPx(pos), pad = PluginUi::PanelPadding();
        PluginUi::Panel(dl, a, ImVec2(a.x + size.x, a.y + size.y), opacity);
        if (m_accent != 0) {
            const float inset = PluginUi::SnapPx(Em() * 0.35f);
            dl->AddRectFilled(ImVec2(a.x + 1.0f, a.y + inset), ImVec2(a.x + 3.0f, a.y + size.y - inset), m_accent);
        }
        // On a faint panel the game shows through: a 1 px shadow keeps the text readable.
        const bool shadow = opacity < 0.85f;
        ImFont* font = ImGui::GetFont();
        const float left = a.x + pad.x;
        float y = a.y + pad.y;
        for (const Row& r : m_rows) {
            const float fs = FontPx(r.scale);
            switch (r.kind) {
            case Kind::Rule: {
                const float ry = PluginUi::SnapPx(y + r.height * 0.5f);
                dl->AddLine(ImVec2(left, ry), ImVec2(a.x + size.x - pad.x, ry), kRule);
                break;
            }
            case Kind::Text: {
                float x = left;
                float wrap = m_maxWidth;
                if (r.mark != Mark::None) {
                    const float lead = MarkLead();
                    DrawMarkGlyph(dl, ImVec2(PluginUi::SnapPx(x + lead * 0.4f), PluginUi::SnapPx(y + fs * 0.55f)), fs * 0.45f, r.mark, MarkColor(r.mark));
                    x += lead;
                    wrap -= lead;
                }
                Text(dl, font, fs, ImVec2(x, PluginUi::SnapPx(y)), r.color, r.text.c_str(), wrap, shadow);
                break;
            }
            case Kind::Flow:
                for (const Item& it : r.items) {
                    const ImVec2 p(left + it.x, y + it.y);
                    if (it.chip) {
                        DrawMarkChip(dl, p, it.text.c_str(), it.color, it.mark);
                    } else {
                        Text(dl, font, fs, PluginUi::SnapPx(ImVec2(p.x, p.y + (it.lineH - it.h) * 0.5f)), it.color, it.text.c_str(), 0.0f, shadow);
                    }
                }
                break;
            }
            y += r.height;
        }
    }

private:
    enum class Kind : uint8_t { Text, Rule, Flow };
    struct Item {
        std::string text;
        ImU32 color = 0;
        Mark mark = Mark::None;
        bool chip = false;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f, lineH = 0.0f;
    };
    struct Row {
        Kind kind = Kind::Text;
        std::string text;
        ImU32 color = 0;
        float scale = 1.0f;
        Mark mark = Mark::None;
        bool chips = false;
        std::vector<Item> items;
        float height = 0.0f;
    };
    static float MarkLead() { return PluginUi::SnapPx(Em() * 1.15f); }
    static ImVec2 ItemSize(ImFont* font, float fs, const Item& it) {
        return it.chip ? MarkChipSize(it.text.c_str(), it.mark) : font->CalcTextSizeA(fs, FLT_MAX, 0.0f, it.text.c_str());
    }
    static void Text(ImDrawList* dl, ImFont* font, float fs, const ImVec2& pos, ImU32 color, const char* text, float wrap, bool shadow) {
        if (shadow) dl->AddText(font, fs, ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 200), text, nullptr, wrap);
        dl->AddText(font, fs, pos, color, text, nullptr, wrap);
    }
    void Add(Kind kind, std::string_view text, ImU32 color, float scale) {
        Row r;
        r.kind = kind;
        r.text.assign(text);
        r.color = color;
        r.scale = scale;
        m_rows.push_back(std::move(r));
        m_measured = false;
    }
    void AddItem(std::string_view text, ImU32 color, Mark mark, bool chip) {
        Item it;
        it.text.assign(text);
        it.color = color;
        it.mark = mark;
        it.chip = chip;
        m_rows.back().items.push_back(std::move(it));
        m_measured = false;
    }
    void Measure() {
        if (m_measured) return;
        ImFont* font = ImGui::GetFont();
        const float rowGap = PluginUi::SnapPx(Em() * 0.12f);
        m_width = m_minWidth;
        m_height = 0.0f;
        for (Row& r : m_rows) {
            const float fs = FontPx(r.scale);
            if (r.kind == Kind::Rule) {
                r.height = PluginUi::SnapPx(Em() * 0.5f);
            } else if (r.kind == Kind::Text) {
                const float lead = r.mark != Mark::None ? MarkLead() : 0.0f;
                const ImVec2 t = font->CalcTextSizeA(fs, FLT_MAX, m_maxWidth - lead, r.text.c_str());
                r.height = PluginUi::SnapPx(std::max(t.y, fs)) + rowGap;
                m_width = std::max(m_width, std::min(t.x, m_maxWidth - lead) + lead);
            } else {
                // Flow: left to right, a new line when the next item does not fit.
                const float gapX = PluginUi::SnapPx(Em() * (r.chips ? 0.3f : 0.5f));
                const float gapY = PluginUi::SnapPx(Em() * 0.2f);
                const float lineH = r.chips ? ChipSize("A").y : PluginUi::SnapPx(fs);
                float x = 0.0f, y = 0.0f;
                for (Item& it : r.items) {
                    ImVec2 size = ItemSize(font, fs, it);
                    // A name wider than the whole card (untrusted build text): cut it, never overflow.
                    while (size.x > m_maxWidth && CutTail(it.text)) size = ItemSize(font, fs, it);
                    if (x > 0.0f && x + size.x > m_maxWidth) {
                        x = 0.0f;
                        y += lineH + gapY;
                    }
                    it.x = x;
                    it.y = y;
                    it.w = size.x;
                    it.h = size.y;
                    it.lineH = lineH;
                    m_width = std::max(m_width, std::min(x + size.x, m_maxWidth));
                    x += size.x + gapX;
                }
                r.height = y + lineH + rowGap + (r.chips ? gapY : 0.0f);
            }
            m_height += r.height;
        }
        m_measured = true;
    }

    std::vector<Row> m_rows;
    float m_maxWidth = 0.0f, m_minWidth = 0.0f, m_width = 0.0f, m_height = 0.0f;
    ImU32 m_accent = 0;
    bool m_measured = false;
};

// ---- ImGui-item helpers (settings tabs and the info window: they advance the layout) ----

// A chip as an ImGui item (SameLine between chips while ChipFits says so).
inline void Chip(const char* text, ImU32 color) {
    const ImVec2 size = ChipSize(text);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    if (ImGui::IsItemVisible()) DrawChip(ImGui::GetWindowDrawList(), pos, text, color);
}
// The same, led by a mark (Mark::None = a plain chip).
inline void MarkChip(const char* text, ImU32 color, Mark mark) {
    const ImVec2 size = MarkChipSize(text, mark);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    if (ImGui::IsItemVisible()) DrawMarkChip(ImGui::GetWindowDrawList(), pos, text, color, mark);
}
// The right edge of the current line: take it BEFORE the first chip of a row, pass it to ChipFits.
inline float LineRight() { return ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x; }
// true when a chip with this text still fits right of the last item (call SameLine only then).
inline bool ChipFits(const char* text, float lineRight) {
    return ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + ChipSize(text).x <= lineRight;
}
inline bool MarkChipFits(const char* text, Mark mark, float lineRight) {
    return ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + MarkChipSize(text, mark).x <= lineRight;
}
// A small colour dot as an ImGui item, vertically centred on a text line (a gem's colour).
inline void ColorDot(ImU32 color) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(h * 0.7f, h));
    if (ImGui::IsItemVisible())
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(pos.x + h * 0.35f, pos.y + h * 0.5f), h * 0.28f, color, 12);
}
// A mark as an ImGui item of one text line's height (a tick after a skill the character has).
inline void MarkItem(Mark mark) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(h, h));
    if (ImGui::IsItemVisible())
        DrawMarkGlyph(ImGui::GetWindowDrawList(), ImVec2(PluginUi::SnapPx(pos.x + h * 0.5f), PluginUi::SnapPx(pos.y + h * 0.5f)), h * 0.5f, mark,
                      MarkColor(mark));
}

} // namespace bp::theme
