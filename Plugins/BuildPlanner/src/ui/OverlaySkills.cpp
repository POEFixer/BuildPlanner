// BuildPlanner - the Skills panel overlay
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// OverlaySkills.cpp - hints on the game's Skills panel: a mark on every
// filled support socket of the skills the build also has, and a column of cards right beside the
// panel: "Missing skills" at its head, then one card level with each skill block ("In the build",
// or what to add, upgrade and take out).
//
// Nothing is drawn over the panel except the socket marks, and those only on blocks whose sockets
// agree with the character data (SkillBlockUi::socketsAgree) and inside the scroll view.
// Nothing is drawn on a tooltip of the game (SkillsFrame::tooltip: a gem's opens right of its socket,
// an item's left of the item, both over the column's place), on the build item card or on our own
// "Build skills" window (theme::Avoid): a mark or a card that would touch one is left out. Only when
// the readers could not find where the game reports its tooltips (TooltipSource::manager == 0) the
// old guess stands in: no column while the cursor is on a socket or on the inventory panel, no mark
// right of the hovered socket.
#include "Theme.h"
#include "Ui.h"

#include <cfloat>
#include <cmath>
#include <string>

namespace bp {
namespace {

using theme::Card;
using theme::Em;
using theme::Mark;

constexpr int kMaxBlocks = 32;          // live blocks considered (the panel shows about five at once)
constexpr size_t kMaxMissing = 64;      // missing skills considered for the head card (kMaxGroups)

Mark MarkOf(SocketMark mark) {
    switch (mark) {
    case SocketMark::InBuild: return Mark::Tick;
    case SocketMark::OtherTier: return Mark::Arrow;
    case SocketMark::NotInBuild: return Mark::Cross;
    case SocketMark::None:
    default: return Mark::None;
    }
}

// Pops the draw list's clip rectangle on every path.
struct ClipScope {
    ImDrawList* dl;
    ClipScope(ImDrawList* list, const ImVec2& a, const ImVec2& b) : dl(list) { dl->PushClipRect(a, b, true); }
    ~ClipScope() { dl->PopClipRect(); }
    ClipScope(const ClipScope&) = delete;
    ClipScope& operator=(const ClipScope&) = delete;
};

// The socket of the panel under the cursor (a big one or a support one); ok == false when none.
Rect HoveredSocket(const OverlayFrame& f) {
    Rect none;
    const SkillsFrame& sf = f.skillsFrame;
    if (!f.mouseValid || !sf.clip.Contains(f.mouse)) return none;
    for (const SkillsFrame::Block& b : sf.blocks) {
        if (!b.inView) continue;
        if (b.bigSocket.Contains(f.mouse)) return b.bigSocket;
        for (const Rect& r : b.sockets)
            if (r.Contains(f.mouse)) return r;
    }
    return none;
}

void DrawSocketMarks(ImDrawList* dl, const OverlayFrame& f, const SkillAdvice& advice, const theme::Avoid& avoid, const Rect& hovered) {
    const SkillsState& state = *f.skills;
    const SkillsFrame& sf = f.skillsFrame;
    const ClipScope clip(dl, ImVec2(sf.clip.x, sf.clip.y), ImVec2(sf.clip.Right(), sf.clip.Bottom()));
    for (size_t i = 0; i < state.blocks.size(); ++i) {
        const SkillBlockUi& block = state.blocks[i];
        const SkillsFrame::Block& fb = sf.blocks[i];
        if (!fb.inView || !block.socketsAgree) continue;
        if (block.skill < 0 || static_cast<size_t>(block.skill) >= advice.charSkills.size()) continue;
        const CharSkillAdvice& cs = advice.charSkills[static_cast<size_t>(block.skill)];
        if (cs.group < 0) continue;   // a skill the build does not have: nothing to say about its sockets
        const int shown = std::min(block.socketCount, kMaxSupportSockets);
        for (int k = 0; k < shown; ++k) {
            const Rect& r = fb.sockets[static_cast<size_t>(k)];
            if (!r.ok || static_cast<size_t>(k) >= cs.sockets.size()) continue;
            const Mark mark = MarkOf(cs.sockets[static_cast<size_t>(k)]);
            if (mark == Mark::None) continue;
            const float radius = theme::MarkRadiusOn(r, 0.22f);
            const ImVec2 centre(r.Right() - radius * 0.8f, r.Bottom() - radius * 0.8f);   // the lower right corner
            if (avoid.HitsDisc(centre, radius + 1.0f)) continue;                          // a tooltip of the game, our card, our window
            if (hovered.ok && centre.x - radius >= hovered.Right() - 1.0f) continue;      // the guess: right of the hovered socket
            theme::DrawMark(dl, centre, radius, mark);
        }
    }
}

// The card of one skill block; false when there is nothing to say (the build does not have the skill).
bool BuildBlockCard(Card& card, const SkillsState& state, const SkillAdvice& advice, const SkillBlockUi& block) {
    if (block.skill < 0 || static_cast<size_t>(block.skill) >= advice.charSkills.size()) return false;
    const CharSkillAdvice& cs = advice.charSkills[static_cast<size_t>(block.skill)];
    if (cs.group < 0 || static_cast<size_t>(cs.group) >= advice.groups.size()) return false;
    const GroupAdvice& group = advice.groups[static_cast<size_t>(cs.group)];
    if (cs.Complete()) {
        card.MarkLine(Mark::Tick, I18n::Tr("ov_skills_in_build"), theme::kGood);
        return true;
    }
    if (!cs.add.empty()) {
        card.Chips(I18n::Tr("ov_skills_add"), theme::kWarn);
        for (const int idx : cs.add)
            if (idx >= 0 && static_cast<size_t>(idx) < group.supports.size())
                card.AddChip(theme::GemLabel(group.supports[static_cast<size_t>(idx)].name), theme::GemColor(group.supports[static_cast<size_t>(idx)].color),
                             Mark::Plus);
    }
    if (!cs.upgrade.empty()) {
        card.Chips(I18n::Tr("ov_skills_upgrade"), theme::kWarn);
        for (const int idx : cs.upgrade)
            if (idx >= 0 && static_cast<size_t>(idx) < group.supports.size())
                card.AddChip(theme::GemLabel(group.supports[static_cast<size_t>(idx)].name), theme::GemColor(group.supports[static_cast<size_t>(idx)].color),
                             Mark::Arrow);
    }
    if (!cs.notInBuild.empty() && static_cast<size_t>(block.skill) < state.skills.size()) {
        const CharSkill& skill = state.skills[static_cast<size_t>(block.skill)];
        bool any = false;
        for (const int socket : cs.notInBuild) {
            if (socket < 0 || static_cast<size_t>(socket) >= skill.supports.size()) continue;
            const CharGem& gem = skill.supports[static_cast<size_t>(socket)];
            if (!gem.present || gem.name.empty()) continue;
            if (!any) card.Chips(I18n::Tr("ov_skills_not_in_build"), theme::kBad);
            any = true;
            card.AddChip(gem.name, theme::GemColor(Attr::None), Mark::Cross);
        }
    }
    return !card.Empty();
}

void DrawColumn(ImDrawList* dl, App& app, const SkillAdvice& advice, const theme::Avoid& avoid) {
    const Settings& s = app.Config();
    const OverlayFrame& f = app.Frame();
    const SkillsState& state = *f.skills;
    const SkillsFrame& sf = f.skillsFrame;
    const float m = PluginUi::SnapPx(Em() * 0.25f), gap = PluginUi::SnapPx(Em() * 0.4f);

    // Right beside the panel; while the inventory is open the column ends where that panel begins
    // (its own rectangle; where the panel is anchored while the gear reader has none). The "Build
    // skills" window places itself by the same rule (theme::SkillsColumnBeside).
    const float inventoryLeft = theme::InventoryLeft(f.gearFrame, f.clientW, f.clientH);
    const theme::SkillsColumn column = theme::SkillsColumnBeside(sf.panel.Right(), f.inventoryOpen ? std::min(f.clientW, inventoryLeft) : f.clientW);
    if (!column.ok) return;   // no room beside the panel (both panels open in a small window)
    const float left = column.left, textWidth = column.textWidth;

    float nextTop = -FLT_MAX;   // the lowest a card may start: below the one before it

    // The head of the column: what the character lacks entirely, or why nothing is known.
    Card head(textWidth);
    if (!advice.charKnown) {
        const SkillsProblem problem = state.problem;
        if (problem == SkillsProblem::NoGems || problem == SkillsProblem::NoPlayerData) {
            const char* text = SkillsProblemText(problem);
            if (text && *text) head.MarkLine(Mark::Dot, text, theme::kWarn);
        }
    } else if (s.showMissingSkills && !advice.missing.empty()) {
        // As many names as fit in the panel's head area (the "Build skills" window has them all), then "+n".
        const float headRoom = sf.clip.y - gap - (sf.panel.y + m) - PluginUi::PanelPadding().y * 2.0f - theme::FontPx(0.85f);
        const float chipHeight = theme::ChipSize("A").y + PluginUi::SnapPx(Em() * 0.2f);
        const int rows = std::max(1, static_cast<int>(headRoom / chipHeight));
        const int total = static_cast<int>(std::min<size_t>(advice.missing.size(), kMaxMissing));
        float widths[kMaxMissing];
        for (int i = 0; i < total; ++i) {
            const int idx = advice.missing[static_cast<size_t>(i)];
            const bool valid = idx >= 0 && static_cast<size_t>(idx) < advice.groups.size();
            widths[i] = valid ? std::min(theme::ChipSize(theme::GemLabel(advice.groups[static_cast<size_t>(idx)].name).c_str()).x, textWidth) : 0.0f;
        }
        const float chipGap = PluginUi::SnapPx(Em() * 0.3f);
        const float moreWidth = ImGui::CalcTextSize("+00").x * 0.9f + chipGap;
        // The row the first `count` chips end in, and where that row ends.
        const auto layout = [&](int count, float& endX) {
            int row = 1;
            float x = 0.0f;
            for (int i = 0; i < count; ++i) {
                if (widths[i] <= 0.0f) continue;
                if (x > 0.0f && x + widths[i] > textWidth) {
                    ++row;
                    x = 0.0f;
                }
                x += widths[i] + chipGap;
            }
            endX = x;
            return row;
        };
        int shown = total;
        for (; shown > 1; --shown) {
            float endX = 0.0f;
            const int row = layout(shown, endX);
            if (row <= rows && (shown == total || endX + moreWidth <= textWidth)) break;
        }
        head.Caption(I18n::Tr("ov_skills_missing"));
        head.Chips({});
        for (int i = 0; i < shown; ++i) {
            const int idx = advice.missing[static_cast<size_t>(i)];
            if (idx >= 0 && static_cast<size_t>(idx) < advice.groups.size())
                head.AddChip(theme::GemLabel(advice.groups[static_cast<size_t>(idx)].name), theme::GemColor(advice.groups[static_cast<size_t>(idx)].color));
        }
        const int more = static_cast<int>(advice.missing.size()) - shown;
        if (more > 0) head.AddPiece(I18n::TrF("ov_skills_more", {{"n", std::to_string(more)}}));
    }
    if (!head.Empty()) {
        // In the panel's head area, ending just above the list; a tall one grows downwards from the
        // panel's top and pushes the block cards.
        const ImVec2 size = head.Size();
        const float top = std::max(sf.panel.y + m, sf.clip.y - gap - size.y);
        const ImVec2 pos = theme::ClampToClient(ImVec2(left, top), size, f.clientW, f.clientH);
        if (!avoid.Hits(pos, size)) head.Draw(dl, pos, s.cardOpacity);
        nextTop = pos.y + size.y + gap;
    }
    if (!advice.charKnown || !s.showSkillMarks) return;

    // The blocks top to bottom (the watcher lists them in no particular order).
    int order[kMaxBlocks];
    int count = 0;
    for (size_t i = 0; i < state.blocks.size() && count < kMaxBlocks; ++i)
        if (sf.blocks[i].inView && sf.blocks[i].block.ok) order[count++] = static_cast<int>(i);
    for (int i = 1; i < count; ++i) {
        const int v = order[i];
        int j = i - 1;
        while (j >= 0 && sf.blocks[static_cast<size_t>(order[j])].block.y > sf.blocks[static_cast<size_t>(v)].block.y) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = v;
    }

    // Level with their blocks and cut where the list is cut.
    const ClipScope clip(dl, ImVec2(left - 1.0f, sf.clip.y), ImVec2(f.clientW, sf.clip.Bottom()));
    for (int i = 0; i < count; ++i) {
        const size_t index = static_cast<size_t>(order[i]);
        Card card(textWidth);
        if (!BuildBlockCard(card, state, advice, state.blocks[index])) continue;
        const ImVec2 size = card.Size();
        const float top = std::max(sf.blocks[index].block.y, nextTop);
        if (top >= sf.clip.Bottom()) break;
        // A card keeps its place while a tooltip of the game (or our own card or window) is on it: it
        // is left out, the others stay where they are.
        if (!avoid.Hits(ImVec2(left, top), size)) card.Draw(dl, ImVec2(left, top), s.cardOpacity);
        nextTop = top + size.y + gap;
    }
}

void DrawSkills(App& app) {
    const Settings& s = app.Config();
    if (!s.showSkillMarks && !s.showMissingSkills) return;
    const OverlayFrame& f = app.Frame();
    if (!f.overlayActive || !f.skillsOpen) return;
    const ActiveView& av = app.Active();
    const SkillAdvice& advice = app.Skills();
    if (!av.build || advice.skillSet < 0) return;
    ImDrawList* dl = OverlayDrawList(app);
    if (!dl || !(f.clientW > 0.0f) || !(f.clientH > 0.0f)) return;

    // No warning line here: a Skills panel the reader cannot find (SkillsProblem::NoPanel)
    // is never "open" to the overlay (its visible bit is read from the panel found), so its cause shows
    // only in the Diagnostics card; and a frame whose rects could not be read is retried next frame.
    if (!f.DrawSkills()) return;

    const SkillsState& state = *f.skills;
    const SkillsFrame& sf = f.skillsFrame;
    if (!sf.panel.ok || !sf.clip.ok || sf.blocks.size() != state.blocks.size()) return;

    // What nothing here may touch: the game's tooltips, the build item card the gear overlay drew this
    // frame, our own "Build skills" window.
    theme::Avoid avoid = theme::GameTooltips(f.pose, f.gearFrame, sf);
    avoid.Add(theme::HoverCardBox());
    avoid.Add(theme::SkillsWindowBox());

    // The guess for a game whose tooltips cannot be read: a gem's tooltip opens right of the hovered
    // socket, an item's left of the item, over the column's place.
    Rect hovered;
    bool guessHidesColumn = false;
    if (state.tooltipSource.manager == 0) {
        hovered = HoveredSocket(f);
        const float inventoryLeft = theme::InventoryLeft(f.gearFrame, f.clientW, f.clientH);
        guessHidesColumn = hovered.ok || (f.inventoryOpen && f.mouseValid && f.mouse.x >= inventoryLeft);
    }
    if (s.showSkillMarks && advice.charKnown) DrawSocketMarks(dl, f, advice, avoid, hovered);
    if (!guessHidesColumn) DrawColumn(dl, app, advice, avoid);
}

} // namespace

void DrawSkillsOverlay(App& app) {
    try {
        DrawSkills(app);
    } catch (...) {
        // The only scope opened here is a draw-list clip rectangle, popped by its guard: balanced.
    }
}

} // namespace bp
