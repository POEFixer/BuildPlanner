// BuildPlanner - the "Build skills" window
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// SkillsWindow.cpp - the "Build skills" info window, through PluginSDK::InfoWindow:
// shown when App::SkillsWindowShown(); closing it through its own menu calls App::CloseSkillsWindow().
//
// One row per skill group of the build: a dot in the gem's colour, the skill's name, its level and a
// mark for "the character has it"; below it the supports as chips, each led by the mark of the Skills
// tab (tick: socketed, arrow: another tier is socketed, plus: to add). It looks like the cards the
// overlays draw (dark panel, gold caption, the same chips and marks), in game and out of it. The host
// owns its place and its gear menu; the mode (always / with the Skills panel / never) is the App's.
//
// The player cannot scroll it while playing (over the game it takes no mouse), so a list that does not
// fit the screen is shortened first (Detail): skills the character has complete keep their name row
// only, then they fold into one closing line, then the other skills show only the supports still to
// add or to upgrade.
// Over the game the window never covers a tooltip of the game: it is not drawn while one is on it.
// Its default place is beside the game's Skills panel, right of the Skills overlay's column of cards
// when it fits there (never on a side panel while the client is wide enough for both), inside the
// client at every size.
#include "Theme.h"
#include "Ui.h"

#include "../../../../POEFixer/plugin_sdk/PluginInfoWindow.h"

#include <cfloat>
#include <string>

namespace bp {
namespace {

using theme::Em;
using theme::Mark;

// Where the window was when it was last drawn over the game (client pixels; ok == false: never, or
// it is shown outside the game). Kept while the window steps aside for a tooltip of the game, so that
// it does not come back under the same tooltip on the next frame.
Rect g_lastBox;

Mark SupportMark(const SupportAdvice& s) {
    if (s.have == Have::Yes) return Mark::Tick;
    if (s.have == Have::No) return s.otherTier ? Mark::Arrow : Mark::Plus;
    return Mark::None;   // the character's gems are not known: nothing is claimed
}

// The character has the skill with every support of the build socketed.
bool Complete(const GroupAdvice& g) {
    if (g.have != Have::Yes) return false;
    for (const SupportAdvice& s : g.supports)
        if (s.have != Have::Yes) return false;
    return true;
}

// `text` as shown: a name template's placeholder dropped, cut to `maxWidth`. Allocates only when the
// text changes (`scratch` then holds the result).
const char* Shown(const std::string& text, float maxWidth, std::string& scratch) {
    const bool templated = text.find("{0}") != std::string::npos;
    if (!templated && ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text.c_str();
    scratch = theme::FitText(templated ? theme::GemLabel(text) : text, maxWidth);
    return scratch.c_str();
}

// The window's columns and sizes, from the font and the texts (never a fixed pixel size).
struct Metrics {
    float em = 0.0f, rowH = 0.0f, gapX = 0.0f, dotW = 0.0f, markW = 0.0f, levelW = 0.0f;
    float width = 0.0f;        // of the content
    float chipH = 0.0f, chipGapX = 0.0f, chipGapY = 0.0f, groupGap = 0.0f;
    float captionH = 0.0f, ruleH = 0.0f;
};

// How much of the list is shown: the most that fits the screen.
enum class Detail : uint8_t {
    Full = 0,   // every skill with its supports
    Compact,    // a skill the character has complete: its name row only
    Folded,     // those skills are one closing line ("9 skills are complete")
    Todo        // and the other skills list only the supports that are not socketed yet
};

// Lays the rows out from `origin` and returns the content's height; draws them when `dl` is not null.
// One function for both, so that what is measured is what is drawn.
float LayOut(ImDrawList* dl, const ImVec2& origin, const Metrics& m, const ActiveView& av, const SkillAdvice& advice, Detail detail) {
    ImFont* font = ImGui::GetFont();
    const float right = origin.x + m.width;
    float y = origin.y;
    std::string scratch;

    // The caption, as on every card; the skill set's name beside it when the build has several.
    {
        const char* caption = I18n::Tr("ov_win_title");
        const float fs = theme::FontPx(0.85f);
        if (dl) dl->AddText(font, fs, PluginUi::SnapPx(ImVec2(origin.x, y)), theme::kCaption, caption);
        const BuildSkillSet* set = av.SkillSet();
        if (dl && set && av.build && av.build->skillSets.size() > 1) {
            const float x = origin.x + font->CalcTextSizeA(fs, FLT_MAX, 0.0f, caption).x + m.em * 0.6f;
            std::string title = theme::TitleLabel(set->title);
            while (!title.empty() && font->CalcTextSizeA(fs, FLT_MAX, 0.0f, title.c_str()).x > right - x && theme::CutTail(title)) {
            }
            if (!title.empty() && font->CalcTextSizeA(fs, FLT_MAX, 0.0f, title.c_str()).x <= right - x)
                dl->AddText(font, fs, PluginUi::SnapPx(ImVec2(x, y)), theme::kTextDim, title.c_str());
        }
        y += m.captionH;
        if (dl) dl->AddLine(PluginUi::SnapPx(ImVec2(origin.x, y + m.ruleH * 0.5f)), PluginUi::SnapPx(ImVec2(right, y + m.ruleH * 0.5f)), theme::kRule);
        y += m.ruleH;
    }

    if (advice.skillSet < 0 || advice.groups.empty()) {
        const char* text = I18n::Tr("ov_win_empty");
        const ImVec2 size = font->CalcTextSizeA(m.em, FLT_MAX, m.width, text);
        if (dl) dl->AddText(font, m.em, PluginUi::SnapPx(ImVec2(origin.x, y)), theme::kTextDim, text, nullptr, m.width);
        return y + size.y - origin.y;
    }

    const float nameLeft = origin.x + m.dotW;
    const float markCentre = right - m.markW * 0.5f;
    const float levelRight = right - m.markW - m.gapX;
    int folded = 0;
    bool first = true;
    for (size_t i = 0; i < advice.groups.size(); ++i) {
        const GroupAdvice& g = advice.groups[i];
        if (detail >= Detail::Folded && Complete(g)) {
            ++folded;
            continue;
        }
        if (!first) y += m.groupGap;
        first = false;
        const bool complete = detail != Detail::Full && Complete(g);

        // dot | name ... level | mark
        if (dl) {
            const float textY = PluginUi::SnapPx(y + (m.rowH - m.em) * 0.5f);
            dl->AddCircleFilled(ImVec2(PluginUi::SnapPx(origin.x + m.em * 0.35f), PluginUi::SnapPx(y + m.rowH * 0.5f)), m.em * 0.28f,
                                theme::GemColor(g.color), 12);
            float nameRight = levelRight;
            if (g.level > 0) {
                const std::string level = I18n::TrF("ov_win_level", {{"n", std::to_string(g.level)}});
                const float w = ImGui::CalcTextSize(level.c_str()).x;
                dl->AddText(ImVec2(PluginUi::SnapPx(levelRight - w), textY), theme::kTextDim, level.c_str());
                nameRight = levelRight - m.levelW - m.em * 0.6f;
            }
            // A skill the character lacks is dimmed and carries the dot of "missing".
            dl->AddText(ImVec2(PluginUi::SnapPx(nameLeft), textY), g.have == Have::No ? theme::kTextDim : theme::kText,
                        Shown(g.name, std::max(m.em * 3.0f, nameRight - nameLeft), scratch));
            if (g.have != Have::Unknown) {
                const Mark mark = g.have == Have::Yes ? Mark::Tick : Mark::Dot;
                theme::DrawMarkGlyph(dl, ImVec2(PluginUi::SnapPx(markCentre), PluginUi::SnapPx(y + m.rowH * 0.5f)), m.em * 0.45f, mark, theme::MarkColor(mark));
            }
        }
        y += m.rowH;

        // The supports as chips under the name, wrapped at the window's width.
        if (g.supports.empty() || complete) continue;
        const float chipTextMax = right - nameLeft - m.em * 1.8f;   // less the chip's padding and its mark
        float x = nameLeft;
        bool any = false;
        for (const SupportAdvice& sup : g.supports) {
            const Mark mark = SupportMark(sup);
            if (detail == Detail::Todo && mark == Mark::Tick) continue;
            any = true;
            const char* text = Shown(sup.name, chipTextMax, scratch);
            const ImVec2 size = theme::MarkChipSize(text, mark);
            if (x > nameLeft && x + size.x > right) {
                x = nameLeft;
                y += m.chipH + m.chipGapY;
            }
            if (dl) theme::DrawMarkChip(dl, ImVec2(x, y), text, theme::GemColor(sup.color), mark);
            x += size.x + m.chipGapX;
        }
        if (any) y += m.chipH + m.chipGapY;
    }
    if (folded > 0) {
        if (!first) y += m.groupGap;
        if (dl) {
            const std::string text = I18n::TrF("ov_win_folded", {{"n", std::to_string(folded)}});
            theme::DrawMarkGlyph(dl, ImVec2(PluginUi::SnapPx(origin.x + m.em * 0.35f), PluginUi::SnapPx(y + m.rowH * 0.5f)), m.em * 0.4f, Mark::Tick,
                                 theme::MarkColor(Mark::Tick));
            dl->AddText(ImVec2(PluginUi::SnapPx(nameLeft), PluginUi::SnapPx(y + (m.rowH - m.em) * 0.5f)), theme::kTextDim,
                        Shown(text, right - nameLeft, scratch));
        }
        y += m.rowH;
    }
    return y - origin.y;
}

Metrics Measure(const SkillAdvice& advice, float displayW) {
    Metrics m;
    m.em = Em();
    m.rowH = PluginUi::SnapPx(m.em * 1.3f);
    m.gapX = PluginUi::SnapPx(m.em * 0.35f);
    m.dotW = PluginUi::SnapPx(m.em * 0.7f) + m.gapX;
    m.markW = PluginUi::SnapPx(m.em);
    m.chipH = theme::ChipSize("A").y;
    m.chipGapX = PluginUi::SnapPx(m.em * 0.3f);
    m.chipGapY = PluginUi::SnapPx(m.em * 0.25f);
    m.groupGap = PluginUi::SnapPx(m.em * 0.2f);
    m.captionH = theme::FontPx(0.85f) + PluginUi::SnapPx(m.em * 0.25f);
    m.ruleH = PluginUi::SnapPx(m.em * 0.5f);

    // The level column is as wide as the widest level; the name column takes the widest name, up to a cap.
    float nameW = 0.0f;
    int widestLevel = 0;
    for (const GroupAdvice& g : advice.groups) {
        nameW = std::max(nameW, ImGui::CalcTextSize(g.name.c_str()).x);
        widestLevel = std::max(widestLevel, g.level);
    }
    if (widestLevel > 0) m.levelW = ImGui::CalcTextSize(I18n::TrF("ov_win_level", {{"n", std::to_string(widestLevel)}}).c_str()).x;
    // Between 15 and 22 em, and never more than about two fifths of a small client.
    const float maxW = std::max(m.em * 12.0f, std::min(m.em * 22.0f, displayW * 0.42f));
    const float minW = std::min(m.em * 15.0f, maxW);
    const float natural = m.dotW + std::min(nameW, m.em * 13.0f) + (m.levelW > 0.0f ? m.em * 0.6f + m.levelW : 0.0f) + m.gapX + m.markW;
    m.width = PluginUi::SnapPx(std::clamp(natural, minW, maxW));
    return m;
}

void DrawWindow(App& app) {
    theme::SkillsWindowBox() = Rect{};   // not drawn yet this frame
    if (!app.SkillsWindowShown()) return;
    const PluginSDK::Context* ctx = app.Ctx();
    if (!ctx || !app.Active().build) return;
    const OverlayFrame& f = app.Frame();
    const float em = Em();
    const float opacity = app.Config().cardOpacity;

    // Over the game: never on a tooltip of the game. It comes back when the tooltip is gone.
    if (g_lastBox.ok && f.overlayActive) {
        const theme::Avoid tips = theme::GameTooltips(f.pose, f.gearFrame, f.skillsFrame);
        if (tips.Hits(ImVec2(g_lastBox.x, g_lastBox.y), ImVec2(g_lastBox.w, g_lastBox.h))) return;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float clientW = f.clientW > 0.0f ? f.clientW : display.x;
    const float clientH = f.clientH > 0.0f ? f.clientH : display.y;
    const ImVec2 padding(PluginUi::SnapPx(em * 0.6f), PluginUi::SnapPx(em * 0.5f));
    const Metrics m = Measure(app.Skills(), clientW);

    PluginSDK::InfoWindowOptions o;
    o.id = "skills";
    o.title = nullptr;   // title-less, like a card: the host's gear sits in the top right corner on hover
    o.flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
              ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    // Over the game it never takes the mouse while the player plays: it can be moved and scrolled
    // while the program's menu is shown. Out of the game the host drops these two (PluginInfoWindow.h).
    if (!f.menuVisible) o.flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoMouseInputs;
    // The default place (the host keeps the user's own afterwards; it is applied when the window
    // appears): right beside the game's Skills panel, whose real edge is used when it is on screen, a
    // little below the top; moved left only as far as it takes to stay inside the client (a 4:3 client
    // has no room between the two side panels).
    // When the Skills panel overlay draws its column of cards beside that panel, the window goes right
    // of the column instead when it fits there whole before the inventory panel: on the column it would
    // hide those cards, which skip whatever our window covers. Where it does not fit, it stays beside
    // the panel as before (it lists what the cards say).
    // The place is decided as if BOTH side panels were open, whatever is open at the moment the window
    // appears: the window stays where it appeared, and a panel opened later must not find it on top of
    // itself. A panel that is not on screen counts at its anchored place.
    {
        const float margin = PluginUi::SnapPx(em * 0.5f);
        const float anchored = clientH * theme::kSidePanelWidthPerHeight;
        const float panelRight = (f.DrawSkills() && f.skillsFrame.panel.ok) ? f.skillsFrame.panel.Right() : anchored;
        const float inventoryLeft = (f.inventoryOpen && f.gearFrame.panel.ok) ? f.gearFrame.panel.x : clientW - anchored;
        const float width = m.width + padding.x * 2.0f;
        float x = std::max(margin, std::min(panelRight + margin, clientW - width - margin));
        const Settings& s = app.Config();
        if (s.showSkillMarks || s.showMissingSkills) {
            const float endX = std::min(clientW, inventoryLeft);
            const theme::SkillsColumn column = theme::SkillsColumnBeside(panelRight, endX);
            const float beside = PluginUi::SnapPx(column.Right() + margin);
            if (column.ok && beside + width + margin <= endX) x = beside;
        }
        o.defaultPos = ImVec2(PluginUi::SnapPx(x), PluginUi::SnapPx(std::max(margin, clientH * 0.06f)));
    }

    // The list is shortened when it would not fit: the window cannot be scrolled while playing. At
    // its default place the tallest window ends above the HUD's bars (they begin at 0.857 of the height).
    const float maxHeight = std::max(em * 12.0f, display.y * 0.76f);
    const ActiveView& av = app.Active();
    const SkillAdvice& advice = app.Skills();
    Detail detail = Detail::Full;
    float height = LayOut(nullptr, ImVec2(0.0f, 0.0f), m, av, advice, detail);
    while (height + padding.y * 2.0f > maxHeight && detail != Detail::Todo) {
        detail = static_cast<Detail>(static_cast<int>(detail) + 1);
        height = LayOut(nullptr, ImVec2(0.0f, 0.0f), m, av, advice, detail);
    }

    // The window's own look is pushed before it and popped after it ends (PluginInfoWindow.h): the
    // dark panel of the overlay cards, whatever theme the program uses.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, PluginUi::SnapPx(em * 0.35f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(12.0f / 255.0f, 14.0f / 255.0f, 18.0f / 255.0f, opacity));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.14f * opacity));

    // A build with very many skills: the window stops growing at most of the screen and scrolls
    // (while the program's menu is shown).
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, maxHeight));

    bool open = true;
    try {
        PluginSDK::InfoWindow window(ctx->Overlay, o, &open);
        if (window.Began() && !window.Detached() && f.overlayActive) {
            const ImVec2 pos = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
            g_lastBox = Rect{pos.x, pos.y, size.x, size.y, true};
            theme::SkillsWindowBox() = g_lastBox;
        } else if (window.Began()) {
            g_lastBox = Rect{};   // outside the game: its place means nothing in client pixels
        }
        if (window) {
            try {
                // One item gives the window its size; the rows are drawn on its draw list (clipped and
                // scrolled with it). Nothing here opens an ImGui scope.
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(m.width, height));
                LayOut(ImGui::GetWindowDrawList(), origin, m, av, advice, detail);
            } catch (...) {
                // LayOut opens no scope: the stacks are balanced and the window still ends below.
            }
        }
    } catch (...) {
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);

    if (!open) app.CloseSkillsWindow();
}

} // namespace

void DrawSkillsWindow(App& app) {
    try {
        DrawWindow(app);
    } catch (...) {
    }
}

} // namespace bp
