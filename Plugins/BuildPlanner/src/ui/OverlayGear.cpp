// BuildPlanner - the gear overlay
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// OverlayGear.cpp - gear hints on the game's inventory panel: a corner mark on every
// equipment slot the build has an item for, and the "build item" card for the hovered slot.
//
// Nothing here is drawn on a tooltip of the game (GearReader.h GearFrame::tooltip: an item's, or a
// gem's that the Skills panel opened over the inventory; theme::Avoid): a mark under one is left out,
// and the card goes into a free REGION of the client: beside the tooltip, never on it and never on the
// hovered slot. The regions are tried in the order the contract names; when the card fits in none of
// them whole, it is rebuilt narrower / with fewer mod lines for the one that keeps the most; when not
// even the bare card fits beside the tooltip and the slot, no card is drawn (the slot's mark stays).
#include "GearShown.h"
#include "Theme.h"
#include "Ui.h"

#include <cmath>
#include <string>

namespace bp {
namespace {

using theme::Card;
using theme::Em;
using theme::Mark;

constexpr int kMaxImplicitLines = 8;
constexpr int kMaxModLines = 22;      // implicit + explicit lines a card shows at most
constexpr size_t kMaxNameBytes = 80;  // of an item's name or base (untrusted build text)
constexpr size_t kMaxModBytes = 200;  // of one mod line
constexpr size_t kMaxRuneBytes = 160; // of the runes line
constexpr int kMaxRegions = 4;

Mark MarkOf(SlotMatch match) {
    switch (match) {
    case SlotMatch::Match: return Mark::Tick;
    case SlotMatch::Differs:
    case SlotMatch::Empty: return Mark::Dot;
    case SlotMatch::NoBuildItem:
    case SlotMatch::Unknown:
    default: return Mark::None;   // nothing is claimed about a slot whose item is not known
    }
}

// The build item card. `modBudget` caps the implicit + explicit lines; what is cut is counted in
// one "+n more lines" row.
void BuildCard(Card& card, const BuildItem& item, GearSlot slot, const SlotAdvice& advice, int modBudget) {
    const ImU32 rarity = theme::RarityColor(item.rarity);
    card.Accent(rarity);
    card.Inline({{I18n::Tr("ov_gear_caption"), theme::kCaption}, {GearSlotText(slot), theme::kTextDim}}, 0.85f);
    if (!item.name.empty()) card.Line(theme::Shortened(item.name, kMaxNameBytes), rarity);
    if (!item.base.empty()) card.Line(theme::Shortened(item.base, kMaxNameBytes), rarity);

    const std::string quality = item.quality > 0 ? I18n::TrF("ov_gear_quality", {{"n", std::to_string(item.quality)}}) : std::string();
    const int socketCount = shown::CountSockets(item.sockets);   // the Gear tab counts them the same way
    const std::string sockets = socketCount > 0 ? I18n::TrF("ov_gear_sockets", {{"n", std::to_string(socketCount)}}) : std::string();
    const std::string itemLevel = item.itemLevel > 0 ? I18n::TrF("ov_gear_item_level", {{"n", std::to_string(item.itemLevel)}}) : std::string();
    const std::string levelReq = item.levelReq > 0 ? I18n::TrF("ov_gear_level_req", {{"n", std::to_string(item.levelReq)}}) : std::string();
    if (!quality.empty() || !sockets.empty() || !itemLevel.empty() || !levelReq.empty())
        card.Inline({{quality, theme::kTextDim}, {sockets, theme::kTextDim}, {itemLevel, theme::kTextDim}, {levelReq, theme::kTextDim}}, 0.9f);
    if (!item.runes.empty()) {
        std::string names;
        for (const std::string& rune : item.runes) {
            if (!names.empty()) names += ", ";
            names += rune;
        }
        card.Small(I18n::TrF("ov_gear_runes", {{"names", theme::Shortened(names, kMaxRuneBytes)}}));
    }

    const int total = static_cast<int>(item.implicits.size() + item.explicits.size());
    int budget = std::clamp(modBudget, 0, kMaxModLines);
    const int implicits = std::min({static_cast<int>(item.implicits.size()), kMaxImplicitLines, budget});
    budget -= implicits;
    const int explicits = std::min(static_cast<int>(item.explicits.size()), budget);
    if (implicits > 0) {
        card.Separator();
        for (int i = 0; i < implicits; ++i)
            card.Small(theme::Shortened(item.implicits[static_cast<size_t>(i)].text, kMaxModBytes), theme::ModLineColor(item.implicits[static_cast<size_t>(i)].tags));
    }
    if (explicits > 0) {
        card.Separator();
        for (int i = 0; i < explicits; ++i)
            card.Small(theme::Shortened(item.explicits[static_cast<size_t>(i)].text, kMaxModBytes), theme::ModLineColor(item.explicits[static_cast<size_t>(i)].tags));
    }
    if (implicits + explicits < total) card.Small(I18n::TrF("ov_gear_more", {{"n", std::to_string(total - implicits - explicits)}}));
    if (item.corrupted) card.Small(I18n::Tr("ov_gear_corrupted"), theme::kBad);

    card.Separator();
    switch (advice.match) {
    case SlotMatch::Match:
        // A rare or magic item matches by its base type: under another name it is "the same base",
        // as the Gear tab says it (shown::SameBaseOnly), never "this item". The slot's tick stays.
        if (shown::SameBaseOnly(item, advice.match, advice.equipped))
            card.MarkLine(Mark::Tick, I18n::TrF("ov_gear_same_base", {{"name", theme::Shortened(advice.equipped, kMaxNameBytes)}}), theme::kGood);
        else
            card.MarkLine(Mark::Tick, I18n::Tr("ov_gear_worn"), theme::kGood);
        break;
    case SlotMatch::Differs:
        if (!advice.equipped.empty()) card.MarkLine(Mark::Dot, I18n::TrF("ov_gear_equipped", {{"name", theme::Shortened(advice.equipped, kMaxNameBytes)}}), theme::kWarn);
        else card.MarkLine(Mark::Dot, I18n::Tr("ov_gear_differs"), theme::kWarn);
        break;
    case SlotMatch::Empty: card.MarkLine(Mark::Dot, I18n::Tr("ov_gear_empty"), theme::kWarn); break;
    case SlotMatch::Unknown:
    case SlotMatch::NoBuildItem:
    default: card.Small(I18n::Tr("ov_gear_unknown")); break;
    }
}

// A free area of the client the card may use, and where in it the card wants to be.
struct Region {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
    float wantX = 0.0f, wantY = 0.0f;   // the card's preferred top-left
    bool alignRight = false;            // the card's right edge goes to x1 (wantX is ignored)

    float W() const { return std::max(0.0f, x1 - x0); }
    float H() const { return std::max(0.0f, y1 - y0); }
    bool Fits(const ImVec2& size) const { return size.x <= W() && size.y <= H(); }
    ImVec2 Place(const ImVec2& size) const {
        const float x = std::clamp(alignRight ? x1 - size.x : wantX, x0, std::max(x0, x1 - size.x));
        const float y = std::clamp(wantY, y0, std::max(y0, y1 - size.y));
        return PluginUi::SnapPx(ImVec2(x, y));
    }
    // The largest part of the region that keeps `gap` away from `o`: one of the four strips beside it.
    // A region that does not reach `o` stays as it is.
    void Carve(const Rect& o, float gap) {
        if (!o.ok || o.x - gap >= x1 || o.Right() + gap <= x0 || o.y - gap >= y1 || o.Bottom() + gap <= y0) return;
        Region best = *this;
        best.x1 = best.x0;   // empty, unless a strip is left
        float bestArea = 0.0f;
        const auto consider = [&](Region r) {
            const float area = r.W() * r.H();
            if (area > bestArea) {
                bestArea = area;
                best = r;
            }
        };
        Region r = *this;
        r.x1 = std::min(x1, o.x - gap);        // left of it
        consider(r);
        r = *this;
        r.x0 = std::max(x0, o.Right() + gap);  // right of it
        r.alignRight = false;
        r.wantX = r.x0;
        consider(r);
        r = *this;
        r.y1 = std::min(y1, o.y - gap);        // above it
        consider(r);
        r = *this;
        r.y0 = std::max(y0, o.Bottom() + gap); // below it
        consider(r);
        *this = best;
    }
};

void DrawHoverCard(ImDrawList* dl, App& app, const BuildItem& item, GearSlot slot, const SlotAdvice& advice, const Rect& slotRect,
                   const theme::Avoid& tips) {
    const OverlayFrame& f = app.Frame();
    const GearFrame& gf = f.gearFrame;
    const float opacity = app.Config().cardOpacity;
    const float w = f.clientW, h = f.clientH;
    const float m = PluginUi::SnapPx(Em() * 0.25f), gap = PluginUi::SnapPx(Em() * 0.4f);

    // The regions, in the order of the contract (GearReader.h GearFrame::gameTooltip). The first
    // `primary` of them are the contract's places; the rest is used only when the card does not fit
    // there even without its mod lines.
    Region regions[kMaxRegions];
    int count = 0, primary = 0;
    // The tooltip the card goes beside: the hovered item's own, else whatever tooltip the game shows.
    const Rect tip = gf.gameTooltip.ok ? gf.gameTooltip : (tips.count > 0 ? tips.rects[0] : Rect{});
    if (tip.ok) {
        regions[count++] = Region{m, m, tip.x - gap, h - m, 0.0f, tip.y, true};                      // left of the tooltip
        regions[count++] = Region{m, tip.Bottom() + gap, w - m, h - m, tip.x, tip.Bottom() + gap};   // below it
        primary = count;
        regions[count++] = Region{tip.Right() + gap, m, w - m, h - m, tip.Right() + gap, slotRect.y};   // right of it: over the panel
        regions[count++] = Region{m, m, w - m, tip.y - gap, tip.x, tip.y - gap, false};                  // above it
    } else if (gf.hoveredHasItem) {
        // The game's tooltip is opening left of the item right now: stay below the slot.
        regions[count++] = Region{m, slotRect.Bottom() + gap, w - m, h - m, slotRect.x, slotRect.Bottom() + gap};
        primary = count;
    } else {
        regions[count++] = Region{m, m, slotRect.x - gap, h - m, 0.0f, slotRect.y, true};      // left of the empty slot
        regions[count++] = Region{slotRect.Right() + gap, m, w - m, h - m, slotRect.Right() + gap, slotRect.y};
        regions[count++] = Region{m, slotRect.Bottom() + gap, w - m, h - m, slotRect.x, slotRect.Bottom() + gap};
        primary = count;
    }
    // No region reaches a tooltip of the game or the hovered slot. `clear` also stays off our own
    // "Build skills" window: tried first, for the whole card only.
    Region clear[kMaxRegions];
    const Rect window = theme::SkillsWindowBox();
    for (int i = 0; i < count; ++i) {
        for (int k = 0; k < tips.count; ++k) regions[i].Carve(tips.rects[k], gap);
        regions[i].Carve(slotRect, gap);
        clear[i] = regions[i];
        clear[i].Carve(window, gap);
    }

    const auto draw = [&](Card& card, const Region& region) {
        const ImVec2 size = card.Size();
        const ImVec2 pos = region.Place(size);
        card.Draw(dl, pos, opacity);
        theme::HoverCardBox() = Rect{pos.x, pos.y, size.x, size.y, true};
    };

    const float preferred = std::min(Em() * 22.0f, std::max(Em() * 12.0f, w * 0.42f));
    const float pad = PluginUi::PanelPadding().x * 2.0f;
    const int total = std::min(kMaxModLines, static_cast<int>(item.implicits.size() + item.explicits.size()));

    // The whole card, in the first region of set[from, to) it fits in.
    const auto whole = [&](const Region* set, int from, int to) {
        Card card(preferred);
        BuildCard(card, item, slot, advice, kMaxModLines);
        const ImVec2 size = card.Size();
        for (int i = from; i < to; ++i) {
            if (!set[i].Fits(size)) continue;
            draw(card, set[i]);
            return true;
        }
        return false;
    };
    // The card cut to a region of [from, to): narrower lines, fewer mod lines. The region that keeps
    // the most mod lines wins, the earlier one on a tie. false when not even the bare card fits.
    const auto cut = [&](int from, int to) {
        int best = -1, bestBudget = -1;
        for (int i = from; i < to; ++i) {
            const Region& r = regions[i];
            if (r.W() - pad < Em() * 9.0f) continue;   // too narrow to read
            const float wrap = std::min(r.W() - pad, preferred);
            Card bare(wrap);
            BuildCard(bare, item, slot, advice, 0);
            const ImVec2 bareSize = bare.Size();
            if (!r.Fits(bareSize)) continue;
            int budget = total;
            if (total > 0) {
                Card full(wrap);
                BuildCard(full, item, slot, advice, total);
                const float fullHeight = full.Size().y;
                if (fullHeight > r.H()) {
                    // The share of the mod lines that fits (a wrapped line is taller: then one less, and again).
                    budget = static_cast<int>(static_cast<float>(total) * (r.H() - bareSize.y) / std::max(1.0f, fullHeight - bareSize.y));
                    for (int attempt = 0; attempt < 8 && budget > 0; ++attempt) {
                        Card trial(wrap);
                        BuildCard(trial, item, slot, advice, budget);
                        if (trial.Size().y <= r.H()) break;
                        --budget;
                    }
                    if (budget > 0) {
                        Card trial(wrap);
                        BuildCard(trial, item, slot, advice, budget);
                        if (trial.Size().y > r.H()) budget = 0;   // still too tall: the bare card, which fits
                    }
                }
            }
            if (budget > bestBudget) {
                bestBudget = budget;
                best = i;
            }
        }
        if (best < 0) return false;
        Card card(std::min(regions[best].W() - pad, preferred));
        BuildCard(card, item, slot, advice, bestBudget);
        if (!regions[best].Fits(card.Size())) return false;
        draw(card, regions[best]);
        return true;
    };

    // Fail closed: when it fits nowhere beside the tooltip and the slot, there is no card this frame.
    if (whole(clear, 0, primary) || whole(regions, 0, primary) || cut(0, primary) || whole(clear, primary, count) ||
        whole(regions, primary, count) || cut(primary, count))
        return;
}

void DrawGear(App& app) {
    theme::HoverCardBox() = Rect{};   // no card yet this frame
    const Settings& s = app.Config();
    if (!s.showGearCard && !s.showSlotMarks) return;
    const OverlayFrame& f = app.Frame();
    if (!f.overlayActive || !f.inventoryOpen) return;
    const ActiveView& av = app.Active();
    const GearAdvice& advice = app.Gear();
    if (!av.build || advice.itemSet < 0 || advice.withItem <= 0) return;
    ImDrawList* dl = OverlayDrawList(app);
    if (!dl || !(f.clientW > 0.0f) || !(f.clientH > 0.0f)) return;

    if (!f.DrawGear()) {
        // Fail closed, and say why while the panel is open: only for the problem that stays (a patch
        // moved an offset), never for a read that is retried in a moment. Neither the slots nor the
        // panel's rectangle are known here, so the line sits left of where the inventory panel is
        // anchored (the right edge of the client, theme::kSidePanelWidthPerHeight).
        // Only NoSlots can show: with GearProblem::NoPanel the reader has no panel whose
        // visible bit it could read, so the inventory never counts as open; that cause shows only in
        // the Diagnostics card.
        const GearProblem problem = f.gear->problem;
        if (!f.gear->found && problem == GearProblem::NoSlots) {
            const char* text = GearProblemText(problem);
            if (text && *text) {
                Card card(std::max(Em() * 12.0f, std::min(Em() * 22.0f, f.clientW * 0.3f)));
                card.MarkLine(Mark::Dot, text, theme::kWarn);
                const ImVec2 size = card.Size();
                const ImVec2 at(f.clientW - f.clientH * theme::kSidePanelWidthPerHeight - Em() * 0.5f - size.x, f.clientH * 0.12f);
                card.Draw(dl, theme::ClampToClient(at, size, f.clientW, f.clientH), s.cardOpacity);
            }
        }
        return;
    }

    const GearFrame& gf = f.gearFrame;
    const theme::Avoid tips = theme::GameTooltips(f.pose, gf, f.skillsFrame);
    theme::Avoid window;
    window.Add(theme::SkillsWindowBox());
    if (s.showSlotMarks) {
        const float inset = std::max(1.0f, PluginUi::SnapPx(Em() * 0.1f));
        for (size_t i = 0; i < kGearSlotCount; ++i) {
            const SlotAdvice& sa = advice.slots[i];
            const Rect& r = gf.slotRect[i];
            const Mark mark = sa.itemIndex >= 0 ? MarkOf(sa.match) : Mark::None;
            if (mark == Mark::None || !r.ok || !(r.w > 0.0f) || !(r.h > 0.0f)) continue;
            const float radius = theme::MarkRadiusOn(r, 0.2f);
            const ImVec2 centre(r.Right() - radius - inset, r.y + radius + inset);
            // No mark on a tooltip of the game (an item's covers the slots left of the hovered one, a
            // gem's from the Skills panel the top of the inventory), nor on our own window.
            if (tips.HitsDisc(centre, radius + 1.0f) || window.HitsDisc(centre, radius)) continue;
            theme::DrawMark(dl, centre, radius, mark);
        }
    }

    if (s.showGearCard && gf.hovered >= 0 && static_cast<size_t>(gf.hovered) < kGearSlotCount) {
        const size_t slot = static_cast<size_t>(gf.hovered);
        const SlotAdvice& sa = advice.slots[slot];
        const Rect& r = gf.slotRect[slot];
        if (sa.itemIndex >= 0 && static_cast<size_t>(sa.itemIndex) < av.build->items.size() && r.ok)
            DrawHoverCard(dl, app, av.build->items[static_cast<size_t>(sa.itemIndex)], static_cast<GearSlot>(slot), sa, r, tips);
    }
}

} // namespace

void DrawGearOverlay(App& app) {
    try {
        DrawGear(app);
    } catch (...) {
        // Nothing here opens an ImGui scope, so the stacks are balanced: this frame's gear hints are dropped.
    }
}

} // namespace bp
