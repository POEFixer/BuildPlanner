#pragma once
// BuildPlanner - what the settings tabs share
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// SettingsCommon.h - what the settings tabs share: RAII scopes for every Begin / End and Push / Pop
// pair (so an exception thrown between them unwinds the ImGui stacks and the page's one catch sits at
// a balanced point, Ui.h), and a few measured-layout helpers in the house look of PluginUi.h.
// Header-only; RENDER thread only.
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

#include "Theme.h"
#include "Ui.h"

namespace bp::sui {

inline float Em() { return ImGui::GetFontSize(); }

// ---------------------------------------------------------------------------------------------
// Scopes
// ---------------------------------------------------------------------------------------------

struct Card {
    explicit Card(const char* id) { PluginUi::BeginCard(id); }
    ~Card() { PluginUi::EndCard(); }
    Card(const Card&) = delete;
    Card& operator=(const Card&) = delete;
};

struct Disabled {
    explicit Disabled(bool disabled) { ImGui::BeginDisabled(disabled); }
    ~Disabled() { ImGui::EndDisabled(); }
    Disabled(const Disabled&) = delete;
    Disabled& operator=(const Disabled&) = delete;
};

struct Id {
    explicit Id(const char* id) { ImGui::PushID(id); }
    explicit Id(int id) { ImGui::PushID(id); }
    ~Id() { ImGui::PopID(); }
    Id(const Id&) = delete;
    Id& operator=(const Id&) = delete;
};

struct TextColor {
    explicit TextColor(const ImVec4& c) { ImGui::PushStyleColor(ImGuiCol_Text, c); }
    explicit TextColor(ImU32 c) { ImGui::PushStyleColor(ImGuiCol_Text, c); }
    ~TextColor() { ImGui::PopStyleColor(); }
    TextColor(const TextColor&) = delete;
    TextColor& operator=(const TextColor&) = delete;
};

struct ItemSpacing {
    explicit ItemSpacing(const ImVec2& spacing) { ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing); }
    ~ItemSpacing() { ImGui::PopStyleVar(); }
    ItemSpacing(const ItemSpacing&) = delete;
    ItemSpacing& operator=(const ItemSpacing&) = delete;
};

struct Group {
    Group() { ImGui::BeginGroup(); }
    ~Group() { ImGui::EndGroup(); }
    Group(const Group&) = delete;
    Group& operator=(const Group&) = delete;
};

// A table that ends itself; `if (Table t{...})` draws the rows.
struct Table {
    Table(const char* id, int columns, ImGuiTableFlags flags) : open(ImGui::BeginTable(id, columns, flags)) {}
    ~Table() {
        if (open) ImGui::EndTable();
    }
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    explicit operator bool() const { return open; }
    bool open;
};

// The flags of every data table of the page: striped rows, padded outer edges, columns sized by
// their setup (measured fixed widths for the short ones, stretch weights for names).
inline constexpr ImGuiTableFlags kTableFlags =
    ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_BordersInnerH;

struct TabBar {
    explicit TabBar(const char* id) : open(ImGui::BeginTabBar(id)) {}
    ~TabBar() {
        if (open) ImGui::EndTabBar();
    }
    TabBar(const TabBar&) = delete;
    TabBar& operator=(const TabBar&) = delete;
    explicit operator bool() const { return open; }
    bool open;
};

struct TabItem {
    TabItem(const char* label, ImGuiTabItemFlags flags) : open(ImGui::BeginTabItem(label, nullptr, flags)) {}
    ~TabItem() {
        if (open) ImGui::EndTabItem();
    }
    TabItem(const TabItem&) = delete;
    TabItem& operator=(const TabItem&) = delete;
    explicit operator bool() const { return open; }
    bool open;
};

// The closed "Diagnostics" card of the kit.
struct Diagnostics {
    Diagnostics(const char* tip, const char* id) : open(PluginUi::BeginDiagnostics(tip, id)) {}
    ~Diagnostics() {
        if (open) PluginUi::EndDiagnostics();
    }
    Diagnostics(const Diagnostics&) = delete;
    Diagnostics& operator=(const Diagnostics&) = delete;
    explicit operator bool() const { return open; }
    bool open;
};

// ---------------------------------------------------------------------------------------------
// Colours of the page (the live theme's, PluginUi::Colors)
// ---------------------------------------------------------------------------------------------

inline ImU32 U32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
inline ImU32 GoodColor() { return U32(PluginUi::Colors().Success); }
inline ImU32 WarnColor() { return U32(PluginUi::Colors().Warning); }
inline ImU32 BadColor() { return U32(PluginUi::Colors().Error); }
inline ImU32 AccentColor() { return U32(PluginUi::Colors().Accent); }
inline ImU32 DimColor() { return U32(PluginUi::Colors().TextSecondary); }

// A game colour (item rarity, gem colour, mod text: Theme.h, made for the dark game) as text on the
// settings page: unchanged on a dark theme, darkened on a light one, where pale yellow on white
// cannot be read.
inline ImU32 Legible(ImU32 color) {
    const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
    if (0.2126f * bg.x + 0.7152f * bg.y + 0.0722f * bg.z <= 0.5f) return color;
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    c.x *= 0.5f;
    c.y *= 0.5f;
    c.z *= 0.5f;
    return ImGui::ColorConvertFloat4ToU32(c);
}

// ---------------------------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------------------------

// Wrapped text in one colour.
inline void Wrapped(const char* text, ImU32 color) {
    const TextColor c(color);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text ? text : "");
    ImGui::PopTextWrapPos();
}
inline void Wrapped(const std::string& text, ImU32 color) { Wrapped(text.c_str(), color); }

// "<icon>  <text>", the text wrapped with a hanging indent so its lines stay clear of the icon.
inline void IconLine(const char* icon, const char* text, ImU32 color) {
    const TextColor c(color);
    ImGui::TextUnformatted(icon ? icon : "");
    ImGui::SameLine();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text ? text : "");
    ImGui::PopTextWrapPos();
}
inline void IconLine(const char* icon, const std::string& text, ImU32 color) { IconLine(icon, text.c_str(), color); }

// A warning / an error / a note line (the cause and what to do), wrapped.
inline void WarnLine(const char* text) { IconLine(ICON_FA_TRIANGLE_EXCLAMATION, text, WarnColor()); }
inline void WarnLine(const std::string& text) { WarnLine(text.c_str()); }
inline void ErrorLine(const std::string& text) { IconLine(ICON_FA_CIRCLE_XMARK, text, BadColor()); }
inline void InfoLine(const char* text) { IconLine(ICON_FA_CIRCLE_INFO, text, DimColor()); }
inline void InfoLine(const std::string& text) { InfoLine(text.c_str()); }

// The result line of a job (App::Note): green for a success, red for a failure, quiet otherwise.
// Draws nothing when the note has no text. Busy notes are drawn by the caller (pill + Cancel).
inline void NoteLine(const JobNote& note) {
    if (note.text.empty() || note.tone == JobNote::Tone::Busy) return;
    switch (note.tone) {
    case JobNote::Tone::Ok: IconLine(ICON_FA_CIRCLE_CHECK, note.text, GoodColor()); break;
    case JobNote::Tone::Error: IconLine(ICON_FA_CIRCLE_XMARK, note.text, BadColor()); break;
    default: IconLine(ICON_FA_CIRCLE_INFO, note.text, DimColor()); break;
    }
}

// One line of text that never leaves the room it has: cut with an ellipsis when it is wider than
// `maxWidth` (<= 0: the rest of the line / table cell), the whole text as its tooltip then.
// Advances the layout like TextUnformatted.
inline void Clipped(const char* text, ImU32 color = 0, float maxWidth = 0.0f) {
    if (!text) text = "";
    const float avail = maxWidth > 0.0f ? maxWidth : ImGui::GetContentRegionAvail().x;
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float lineH = ImGui::GetTextLineHeight();
    if (size.x <= avail + 0.5f) {
        if (color) {
            const TextColor c(color);
            ImGui::TextUnformatted(text);
        } else {
            ImGui::TextUnformatted(text);
        }
        return;
    }
    const float w = (std::max)(avail, Em());
    // Read before the Dummy: reserving the item starts a new line and zeroes the offset.
    const float baseline = ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;
    ImGui::Dummy(ImVec2(w, lineH));
    if (ImGui::IsItemVisible()) {
        const ImVec2 min(pos.x, pos.y + baseline), max(pos.x + w, pos.y + baseline + lineH);
        if (color) ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), min, max, max.x, text, nullptr, &size);
        if (color) ImGui::PopStyleColor();
    }
    PluginUi::Tip(text);
}
inline void Clipped(const std::string& text, ImU32 color = 0, float maxWidth = 0.0f) { Clipped(text.c_str(), color, maxWidth); }

// A small caption above a block inside a card (quieter than a Heading: no separator).
inline void Caption(const char* text) {
    const TextColor c(PluginUi::Colors().TextSecondary);
    ImGui::TextUnformatted(text ? text : "");
}

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------

// Keeps the next item on the current line when an item `width` wide still fits right of the last
// one; otherwise the next item starts a new line. `lineRight` = theme::LineRight() taken before the
// first item of the row.
inline void SameLineIfFits(float width, float lineRight) {
    if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + width <= lineRight) ImGui::SameLine();
}

// A pill on the current table / grid row, vertically centred on a TEXT line (StatusPill centres on
// a frame-height row, which is taller than a table's text row).
inline ImVec2 PillSize(const char* text) {
    const ImVec2 t = ImGui::CalcTextSize(text ? text : "");
    return ImVec2(PluginUi::SnapPx(t.x + Em() * 0.9f), ImGui::GetTextLineHeight());
}
inline void Pill(const char* text, ImU32 color) {
    if (!text) text = "";
    const ImVec2 size = PillSize(text);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float baseline = ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;   // before the Dummy zeroes it
    ImGui::Dummy(size);
    if (!ImGui::IsItemVisible()) return;
    // A text line is not centred on its letters: the room above the capitals is larger than the room
    // under the baseline, so a pill of exactly the line's height looks bottom-heavy and its border
    // cuts descenders. The outline grows by a sliver into the row's padding on both sides and the
    // text moves up by as much: the letters sit in the middle, the layout size stays PillSize.
    const float grow = PluginUi::SnapPx(size.y * 0.06f);
    const ImVec2 a(PluginUi::SnapPx(pos.x), PluginUi::SnapPx(pos.y + baseline) - grow),
        b(PluginUi::SnapPx(pos.x + size.x), PluginUi::SnapPx(pos.y + baseline + size.y) + grow);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rounding = (b.y - a.y) * 0.5f;
    dl->AddRectFilled(a, b, theme::WithAlpha(color, 0.18f), rounding);
    dl->AddRect(a, b, theme::WithAlpha(color, 0.55f), rounding);
    const ImVec2 t = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(PluginUi::SnapPx(a.x + (size.x - t.x) * 0.5f), PluginUi::SnapPx(a.y + (b.y - a.y - t.y) * 0.5f) - grow), color, text);
}

// A chip of the settings page (a support gem, a passive to refund, an attribute count): the pill's
// look at a chip's size, on the page's own background. theme::Chip is made for the game (a dark plate
// under the tint): on a light theme it is a dark blot, and a colour made Legible() cannot be read on it.
inline ImVec2 ChipSize(const char* text) {
    const ImVec2 t = ImGui::CalcTextSize(text ? text : "");
    return ImVec2(PluginUi::SnapPx(t.x + Em() * 0.9f), PluginUi::SnapPx(ImGui::GetTextLineHeight() + Em() * 0.3f));
}
inline void Chip(const char* text, ImU32 color) {
    if (!text) text = "";
    const ImVec2 size = ChipSize(text);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    if (!ImGui::IsItemVisible()) return;
    const ImVec2 a = PluginUi::SnapPx(pos), b(a.x + size.x, a.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, theme::WithAlpha(color, 0.18f), size.y * 0.5f);
    dl->AddRect(a, b, theme::WithAlpha(color, 0.55f), size.y * 0.5f);
    const ImVec2 t = ImGui::CalcTextSize(text);
    // Up by a sliver, as in Pill: the letters, not the text line, sit in the middle.
    dl->AddText(ImVec2(PluginUi::SnapPx(a.x + (size.x - t.x) * 0.5f),
                       PluginUi::SnapPx(a.y + (size.y - t.y) * 0.5f) - PluginUi::SnapPx(t.y * 0.06f)),
                color, text);
}
// true when a chip with this text still fits right of the last item (call SameLine only then).
// `lineRight` = theme::LineRight() taken before the first chip of the row.
inline bool ChipFits(const char* text, float lineRight) {
    return ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + ChipSize(text).x <= lineRight;
}

// The widest of some texts (a measured column).
inline float MaxTextWidth(std::initializer_list<const char*> texts) {
    float w = 0.0f;
    for (const char* t : texts)
        if (t) w = (std::max)(w, ImGui::CalcTextSize(t).x);
    return w;
}

// The width a combo needs to show the widest of its entries whole: the text, the frame's padding
// and the arrow button.
inline float ComboWidthFor(const char* const* items, int count) {
    float w = 0.0f;
    for (int i = 0; i < count; ++i)
        if (items[i]) w = (std::max)(w, ImGui::CalcTextSize(items[i]).x);
    return w + ImGui::GetStyle().FramePadding.x * 2.0f + ImGui::GetFrameHeight();
}
inline float ComboWidthFor(std::initializer_list<const char*> items) { return ComboWidthFor(items.begin(), static_cast<int>(items.size())); }

// The label of a combo `comboW` wide (ComboWidthFor) on a LabelColumn row, and the next item's width:
// beside the label while the row holds the whole combo, else the label gets its own line and the
// combo the line under it (as wide as it needs, the row's width at most). Never narrower than 10 em.
inline void ComboRow(const PluginUi::LabelColumn& col, const char* label, float comboW) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float want = (std::max)(Em() * 10.0f, comboW);
    if (avail - col.Width() >= want) {
        col.RowNoWidth(label);
        ImGui::SetNextItemWidth(want);
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label ? label : "");
        ImGui::SetNextItemWidth((std::min)(avail, want));
    }
}

// A small icon button without a frame's bulk: for the actions of a table row.
inline bool IconButton(const char* icon, const char* id, const char* tip) {
    char label[64];
    std::snprintf(label, sizeof(label), "%s###%s", icon ? icon : "", id ? id : "");
    const bool pressed = PluginUi::SubtleButton(label);
    PluginUi::Tip(tip);
    return pressed;
}
inline float IconButtonWidth(const char* icon) { return PluginUi::ButtonWidth(icon); }

// "YYYY-MM-DD" of a unix time in local time; "" for 0 or an impossible time.
inline std::string DateText(int64_t unixSeconds) {
    if (unixSeconds <= 0) return {};
    const std::time_t t = static_cast<std::time_t>(unixSeconds);
    std::tm local{};
    if (localtime_s(&local, &t) != 0) return {};
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return buf;
}

// The card every data tab shows while no build is selected.
inline void NoBuildCard(const char* icon, const char* title) {
    const Card card("##bp_empty");
    PluginUi::Heading(icon, title);
    PluginUi::Hint(I18n::Tr("common_no_build"));
}

// A tree / set title without Path of Building's loadout tag: "Level 98 {3}" is "Level 98" to the
// reader (the tag only links a tree to its sets). The rule of Stage::title (Loadouts.h): the FIRST
// brace group made of letters, digits and commas goes, the rest is trimmed.
inline std::string CleanTitle(std::string_view title) {
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

// The title to show for a tree / set: its own (without the loadout tag), or "<fallback> <n>" when
// the author gave none.
inline std::string TitleOr(const std::string& title, const char* fallbackKey, int index) {
    std::string clean = CleanTitle(title);
    if (!clean.empty()) return clean;
    return I18n::TrF(fallbackKey, {{"n", std::to_string(index + 1)}});
}

// A gem's name as shown: Path of Building names a few gems with a placeholder the game fills in
// ("Spectre: {0}" is "Spectre: Bloodthief Wasp" in the game). The placeholder goes, with the
// separator in front of it: "Spectre".
inline std::string GemName(const std::string& name) {
    const size_t at = name.find("{0}");
    if (at == std::string::npos) return name;
    std::string out = name.substr(0, at) + name.substr(at + 3);
    while (!out.empty() && (out.back() == ' ' || out.back() == ':' || out.back() == '-')) out.pop_back();
    return out.empty() ? name : out;
}

// "<n>." and a wrapped sentence with a hanging indent: one step of a short how-to. `numberW` = the
// width kept for the number (StepNumberWidth), so the sentences of every step start in one column.
inline float StepNumberWidth() { return ImGui::CalcTextSize("0.").x + ImGui::GetStyle().ItemSpacing.x; }
inline void Step(int number, const char* text, float numberW) {
    const float x = ImGui::GetCursorPosX();
    {
        const TextColor c(AccentColor());
        ImGui::Text("%d.", number);
    }
    ImGui::SameLine(x + numberW, 0.0f);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text ? text : "");
    ImGui::PopTextWrapPos();
}

} // namespace bp::sui
