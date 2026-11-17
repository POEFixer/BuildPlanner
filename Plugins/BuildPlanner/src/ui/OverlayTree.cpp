// BuildPlanner - the passive tree overlay
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// OverlayTree.cpp - the passive tree overlay: rings, step numbers, lines, weapon set and
// attribute badges, the summary box and the tooltip, drawn while the game's passive tree is open.
//
// Layers, bottom to top: lines, rings, badges, the summary card, the tooltip card. Everything is in
// game client pixels on OverlayDrawList(); the node positions come from the live UI (TreeTable +
// TreePose), the tree data gives kinds, names and the shape of the links.
// Nothing is drawn over what covers the tree (Covers): the tree panel's own parts (the points panel,
// the title) and the HUD at the bottom of the client (TreePose::occluders), a tooltip of the game
// (TreePose::tooltip: the passive's under the cursor, or any other) and our own "Build skills"
// window. Nodes under them get no ring, lines are cut at their edges, and no card touches a tooltip.
// Only when the readers could not find where the game reports its tooltips (TooltipSource::manager
// == 0) a guess stands in for the passive tooltip (GuessGameTip): rings and lines in it are faint.
// The node loop allocates nothing once warm (two file-static scratch vectors); cards are built only
// for the summary and for the one hovered ring.
#include "Theme.h"
#include "Ui.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace bp {
namespace {

using theme::Card;
using theme::Em;
using theme::Mark;

constexpr float kPi = 3.14159265358979f;
constexpr int kMaxArcSegments = 32;
constexpr int kMaxTooltipStats = 8;
constexpr int kMaxJewelMods = 6;
constexpr size_t kMaxNoteBytes = 600;    // of the author's note shown in the tooltip
constexpr size_t kMaxTitleBytes = 60;    // of the build's name and of a stage's title in the summary
constexpr float kTipFade = 0.22f;        // rings and lines where the game's tooltip is only guessed

// The guess of the game's own passive tooltip (measured on the 0.5.5 client): 728 units wide at 1.3 x
// clientH / 1600, centred on the node, its bottom 20 units above the node's top, moved down to stay
// inside the client. Its height follows the text: the smallest seen is 263 units; kTipTallUnits is a
// generous bound for what it may cover.
constexpr float kTipScale = 1.3f / 1600.0f;
constexpr float kTipWidthUnits = 728.0f, kTipGapUnits = 20.0f, kTipMinUnits = 263.0f, kTipTallUnits = 560.0f;

struct Proj {
    float x = 0.0f, y = 0.0f, r = 0.0f;   // centre and ring radius, client px
    float fade = 1.0f;                    // alpha factor (under the game's tooltip)
    bool ok = false;
};

struct Ring {
    uint32_t node = kNoIndex;
    float x = 0.0f, y = 0.0f, r = 0.0f, thick = 0.0f, fade = 1.0f;
    ImU32 color = 0;
    int32_t number = 0;
    NodeState state = NodeState::None;
};

// Scratch of the node loop, kept across frames so that the loop allocates nothing once warm.
// DrawUI runs on the render thread only.
std::vector<Proj> g_proj;
std::vector<Ring> g_rings;

bool Shown(const Settings& s, NodeState st) {
    switch (st) {
    case NodeState::Missing: return true;
    case NodeState::Wrong: return s.showWrong;
    case NodeState::Adjust: return s.showAdjust;
    case NodeState::Ok: return s.showOk;
    case NodeState::None:
    default: return false;
    }
}

ImU32 StateColor(const Settings& s, NodeState st) {
    switch (st) {
    case NodeState::Missing: return s.colorMissing;
    case NodeState::Wrong: return s.colorWrong;
    case NodeState::Adjust: return s.colorAdjust;
    case NodeState::Ok: return s.colorOk;
    case NodeState::None:
    default: return 0;
    }
}

float WrapPi(float a) {
    while (a > kPi) a -= 2.0f * kPi;
    while (a <= -kPi) a += 2.0f * kPi;
    return a;
}

bool Finite(float a, float b) { return std::isfinite(a) && std::isfinite(b); }

// What covers the tree this frame: nothing is drawn under these.
constexpr int kMaxCovers = static_cast<int>(kMaxTreeOccluders) + theme::Avoid::kMax + 2;
struct Covers {
    Rect rects[kMaxCovers];
    int count = 0;
    void Add(const Rect& r) {
        if (r.ok && count < kMaxCovers) rects[count++] = r;
    }
    void Add(const theme::Avoid& a) {
        for (int i = 0; i < a.count; ++i) Add(a.rects[i]);
    }
    // Does the box touch one of them?
    bool Hits(const ImVec2& pos, const ImVec2& size) const {
        for (int i = 0; i < count; ++i)
            if (theme::Overlaps(rects[i], pos, size)) return true;
        return false;
    }
};

// Does a disc touch one of the covers?
bool Covered(const Covers& covers, float x, float y, float radius) {
    for (int i = 0; i < covers.count; ++i) {
        const Rect& r = covers.rects[i];
        if (x >= r.x - radius && y >= r.y - radius && x < r.Right() + radius && y < r.Bottom() + radius) return true;
    }
    return false;
}

// A stroke on the game picture: the coloured line over a dark, slightly wider one.
void Stroke(ImDrawList* dl, const ImVec2* pts, int count, ImU32 color, float width) {
    if (count < 2) return;
    const float alpha = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFF) / 255.0f;
    dl->AddPolyline(pts, count, theme::WithAlpha(theme::kUnderlay, alpha), width + 2.0f, ImDrawFlags_None);
    dl->AddPolyline(pts, count, color, width, ImDrawFlags_None);
}

// The part of the segment p -> q inside `r`, as the parameter interval [t0, t1]; false = none.
bool SegmentInRect(const ImVec2& p, const ImVec2& q, const Rect& r, float margin, float& t0, float& t1) {
    const float dx = q.x - p.x, dy = q.y - p.y;
    const float edgeP[4] = {-dx, dx, -dy, dy};
    const float edgeQ[4] = {p.x - (r.x - margin), (r.Right() + margin) - p.x, p.y - (r.y - margin), (r.Bottom() + margin) - p.y};
    t0 = 0.0f;
    t1 = 1.0f;
    for (int i = 0; i < 4; ++i) {
        if (edgeP[i] == 0.0f) {
            if (edgeQ[i] < 0.0f) return false;
        } else {
            const float t = edgeQ[i] / edgeP[i];
            if (edgeP[i] < 0.0f) t0 = std::max(t0, t);
            else t1 = std::min(t1, t);
        }
    }
    return t0 < t1;
}

// A straight stroke from p to q, without the parts under the covers.
void StrokeUncovered(ImDrawList* dl, const Covers& covers, const ImVec2& p, const ImVec2& q, ImU32 color, float width) {
    float lo[kMaxCovers + 1], hi[kMaxCovers + 1];
    int count = 1;
    lo[0] = 0.0f;
    hi[0] = 1.0f;
    const float margin = width * 0.5f + 1.0f;
    for (int o = 0; o < covers.count; ++o) {
        float t0 = 0.0f, t1 = 1.0f;
        if (!SegmentInRect(p, q, covers.rects[o], margin, t0, t1)) continue;
        for (int i = count - 1; i >= 0; --i) {
            if (t1 <= lo[i] || t0 >= hi[i]) continue;
            const float l = lo[i], h = hi[i];
            if (t0 > l && t1 < h) {   // the cover cuts the piece in two
                if (count <= kMaxCovers) {
                    lo[count] = t1;
                    hi[count] = h;
                    ++count;
                }
                hi[i] = t0;
            } else if (t0 > l) {
                hi[i] = t0;
            } else if (t1 < h) {
                lo[i] = t1;
            } else {
                lo[i] = lo[count - 1];
                hi[i] = hi[count - 1];
                --count;
            }
        }
    }
    const float dx = q.x - p.x, dy = q.y - p.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    for (int i = 0; i < count; ++i) {
        if ((hi[i] - lo[i]) * len < 1.0f) continue;
        const ImVec2 pts[2] = {ImVec2(p.x + dx * lo[i], p.y + dy * lo[i]), ImVec2(p.x + dx * hi[i], p.y + dy * hi[i])};
        Stroke(dl, pts, 2, color, width);
    }
}

void StraightLink(ImDrawList* dl, const Covers& covers, const Proj& a, const Proj& b, ImU32 color, float width) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (!(len > a.r + b.r + 1.0f)) return;   // the rings touch: nothing to draw between them
    const float ux = dx / len, uy = dy / len;
    StrokeUncovered(dl, covers, ImVec2(a.x + ux * a.r, a.y + uy * a.r), ImVec2(b.x - ux * b.r, b.y - uy * b.r), color, width);
}

// The shorter arc of the circle (cx, cy, radius) from a to b, cut at both rings and at the covers.
// false = the arc is not drawable (the caller falls back to a straight line).
bool ArcLink(ImDrawList* dl, const Covers& covers, const Proj& a, const Proj& b, float cx, float cy, float radius, ImU32 color, float width) {
    if (!(radius >= 2.0f) || !Finite(cx, cy) || !std::isfinite(radius)) return false;
    const float angleA = std::atan2(a.y - cy, a.x - cx), angleB = std::atan2(b.y - cy, b.x - cx);
    const float delta = WrapPi(angleB - angleA);
    const float dir = delta >= 0.0f ? 1.0f : -1.0f;
    const float trimA = std::min(a.r / radius, 1.5f), trimB = std::min(b.r / radius, 1.5f);
    const float sweep = std::fabs(delta) - trimA - trimB;
    if (sweep <= 0.0f) return true;   // the rings touch along the arc
    const int segments = std::clamp(static_cast<int>(std::ceil(sweep * radius / 6.0f)), 2, kMaxArcSegments);
    ImVec2 pts[kMaxArcSegments + 1];
    const float start = angleA + dir * trimA;
    const float margin = width * 0.5f + 1.0f;
    int run = 0;
    for (int i = 0; i <= segments; ++i) {
        const float t = start + dir * sweep * static_cast<float>(i) / static_cast<float>(segments);
        const ImVec2 p(cx + radius * std::cos(t), cy + radius * std::sin(t));
        if (Covered(covers, p.x, p.y, margin)) {
            Stroke(dl, pts, run, color, width);
            run = 0;
        } else {
            pts[run++] = p;
        }
    }
    Stroke(dl, pts, run, color, width);
    return true;
}

// One link of the plan, shaped as the game draws it (Planner.h PlanEdge, TreeData.h Node::drawnOrbit).
void DrawEdge(ImDrawList* dl, const PassiveTree& tree, const TreePose& pose, const Covers& covers, const PlanEdge& e, const Proj& a, const Proj& b,
              bool curved, ImU32 color, float width) {
    if (curved && e.arcOrbit != 0) {
        const size_t orbit = static_cast<size_t>(e.arcOrbit < 0 ? -e.arcOrbit : e.arcOrbit);
        if (orbit < tree.orbitRadii.size()) {
            const float radius = pose.scale * static_cast<float>(tree.orbitRadii[orbit]);
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (d > 1.0f && d < 2.0f * radius) {
                const float h = std::sqrt(radius * radius - d * d * 0.25f) * (e.arcOrbit > 0 ? 1.0f : -1.0f);
                const float cx = a.x + dx * 0.5f + h * (dy / d), cy = a.y + dy * 0.5f - h * (dx / d);
                if (ArcLink(dl, covers, a, b, cx, cy, radius, color, width)) return;
            }
        }
    } else if (curved && e.groupArc) {
        const Node& na = tree.nodes[e.a];
        const Node& nb = tree.nodes[e.b];
        const float radius = pose.scale * na.orbitRadius;
        if (radius >= 2.0f) {
            // The projection is a uniform scale plus an offset, so the group's centre follows from one
            // end and its angle; the other end must then sit on the same circle, or the data is not
            // what the game shows and the line stays straight.
            const float cx = a.x - radius * std::sin(na.angle), cy = a.y + radius * std::cos(na.angle);
            const float bx = cx + radius * std::sin(nb.angle), by = cy - radius * std::cos(nb.angle);
            const float ex = bx - b.x, ey = by - b.y;
            if (ex * ex + ey * ey <= 4.0f + radius * radius * 1e-4f && ArcLink(dl, covers, a, b, cx, cy, radius, color, width)) return;
        }
    }
    StraightLink(dl, covers, a, b, color, width);
}

// The centre of a badge on the ring's edge, in the diagonal direction (dx, dy) = (+-1, +-1).
ImVec2 BadgeAt(const Ring& d, float dx, float dy, float badgeRadius) {
    const float k = (d.r + badgeRadius * 0.45f) * 0.7071f;
    return ImVec2(d.x + dx * k, d.y + dy * k);
}

std::string Num(int v) { return std::to_string(v); }

std::string NodeLabel(const Node& n, const PlanView& pv) {
    const std::string& name = n.NameFor(pv.className, pv.ascendancyName);
    if (!name.empty()) return name;
    return "#" + std::to_string(static_cast<unsigned>(n.id));
}

const char* AttrName(Attr a) {
    switch (a) {
    case Attr::Str: return I18n::Tr("ov_attr_str");
    case Attr::Dex: return I18n::Tr("ov_attr_dex");
    case Attr::Int: return I18n::Tr("ov_attr_int");
    case Attr::None:
    default: return "";
    }
}

// The stage's title as the summary and the tooltip name it: the tree's own title without Path of
// Building's loadout tag, else "Tree n". Stage::title is already clean, and empty exactly when the raw
// title was only a tag ("{2}") or blanks: the raw title must never stand in for it. The
// raw one is read, cleaned the same way, only when the stages are missing.
std::string SpecLabel(const StoredBuild& build, const std::vector<Stage>& stages, int spec) {
    std::string title;
    if (spec >= 0 && static_cast<size_t>(spec) < stages.size()) title = stages[static_cast<size_t>(spec)].title;
    else if (spec >= 0 && static_cast<size_t>(spec) < build.specs.size()) title = theme::TitleLabel(build.specs[static_cast<size_t>(spec)].title);
    if (!title.empty()) return theme::Shortened(title, kMaxTitleBytes);
    return I18n::TrF("common_tree_n", {{"n", Num(spec + 1)}});   // the tabs' name for an untitled tree
}

const ObjectiveRow* RowOf(const std::vector<ObjectiveRow>& rows, uint32_t objective) {
    for (const ObjectiveRow& r : rows)
        if (r.node == objective) return &r;
    return nullptr;
}

// Where the game's own tooltip of the passive under the cursor is expected (the guess above): the
// stand-in while the game's real tooltip cannot be read. ok == false: the cursor is on no passive.
Rect GuessGameTip(const OverlayFrame& f) {
    Rect guess;
    if (!f.mouseValid) return guess;
    const TreePose& pose = f.pose;
    float bestD = FLT_MAX, bx = 0.0f, by = 0.0f, br = 0.0f;
    for (const TreeNodeUi& n : f.table->nodes) {
        if (!n.visible) continue;
        const Vec2 c = pose.Centre(n);
        const float dx = f.mouse.x - c.x, dy = f.mouse.y - c.y, r = pose.Radius(n);
        const float dd = dx * dx + dy * dy;
        if (dd <= r * r && dd < bestD) {
            bestD = dd;
            bx = c.x;
            by = c.y;
            br = r;
        }
    }
    if (bestD == FLT_MAX) return guess;
    const float unit = kTipScale * f.clientH;
    const float w = kTipWidthUnits * unit;
    const float bottom = by - br - kTipGapUnits * unit;
    guess.ok = true;
    guess.x = std::clamp(bx - w * 0.5f, 0.0f, std::max(0.0f, f.clientW - w));
    guess.w = w;
    if (bottom - kTipMinUnits * unit >= 0.0f) {
        // It fits above the node, where the game puts it.
        guess.y = std::max(0.0f, bottom - kTipTallUnits * unit);
        guess.h = bottom - guess.y;
    } else {
        // No room above: the game moves it down, over the node.
        guess.y = 0.0f;
        guess.h = kTipTallUnits * unit;
    }
    return guess;
}

void AddStateLines(Card& card, const PassiveTree& tree, const PlanView& pv, const ActiveView& av, const Ring& d) {
    const Plan& plan = pv.plan;
    const Node& n = tree.nodes[d.node];
    const bool asc = n.IsAscendancy();
    const std::vector<PlanStep>& steps = asc ? plan.ascSteps : plan.steps;
    const int32_t si = plan.stepOf[d.node];
    const PlanStep* step = (si >= 0 && static_cast<size_t>(si) < steps.size()) ? &steps[static_cast<size_t>(si)] : nullptr;
    const ImU32 color = theme::Opaque(d.color);
    const WeaponSet wantSet = plan.wantSet[d.node];
    const Attr wantAttr = plan.wantAttr[d.node];
    const auto setLine = [&](WeaponSet set) {
        card.Line(set == WeaponSet::One ? I18n::Tr("ov_tip_set_one") : set == WeaponSet::Two ? I18n::Tr("ov_tip_set_two") : I18n::Tr("ov_tip_set_none"),
                  set == WeaponSet::One ? theme::kSetOne : set == WeaponSet::Two ? theme::kSetTwo : theme::kText);
    };

    switch (d.state) {
    case NodeState::Missing:
        if (d.number > 0)
            card.Line(asc ? I18n::TrF("ov_tip_take_asc_step", {{"n", Num(d.number)}}) : I18n::TrF("ov_tip_take_step", {{"n", Num(d.number)}}), color);
        else if (step && !step->reachable) card.Line(I18n::Tr("ov_tip_unreachable"), color);
        else card.Line(I18n::Tr("ov_tip_take"), color);
        if (wantSet != WeaponSet::None) setLine(wantSet);
        if (n.Is(kNodeAttribute) && wantAttr != Attr::None)
            card.Line(I18n::TrF("ov_tip_choose", {{"attribute", AttrName(wantAttr)}}), theme::AttrColor(wantAttr));
        if (!n.CostsPoint()) card.Small(I18n::Tr("ov_tip_free"));
        break;
    case NodeState::Wrong: card.Line(I18n::Tr("ov_tip_refund"), color); break;
    case NodeState::Adjust:
        // What is wrong with it, then what to do, in the colours the badges on the ring use.
        card.Line(I18n::Tr("ov_tip_adjust"), color);
        if (plan.adjustBits[d.node] & kAdjustWeaponSet) setLine(wantSet);
        if ((plan.adjustBits[d.node] & kAdjustAttribute) && wantAttr != Attr::None)
            card.Line(I18n::TrF("ov_tip_choose", {{"attribute", AttrName(wantAttr)}}), theme::AttrColor(wantAttr));
        break;
    case NodeState::Ok: card.Line(I18n::Tr("ov_tip_ok"), color); break;
    case NodeState::None:
    default: break;
    }

    if (step) {
        const std::vector<ObjectiveRow>& rows = asc ? pv.numbers.ascObjectives : pv.numbers.objectives;
        if (step->objective != kNoIndex && step->objective < tree.nodes.size()) {
            const ObjectiveRow* row = RowOf(rows, step->objective);
            if (step->objective != d.node) {
                const std::string name = NodeLabel(tree.nodes[step->objective], pv);
                if (row) card.Small(I18n::TrF("ov_tip_on_way", {{"name", name}, {"n", Num(row->cost)}}));
                else card.Small(I18n::TrF("ov_tip_on_way_plain", {{"name", name}}));
            } else if (row && row->cost > 1) {
                card.Small(I18n::TrF("ov_tip_objective", {{"n", Num(row->cost)}}));
            }
        }
        if (step->tier > 0 && av.build && static_cast<size_t>(step->tier - 1) < av.build->specs.size())
            card.Small(I18n::TrF("ov_tip_tier", {{"title", SpecLabel(*av.build, av.stages, step->tier - 1)}}));
    }
}

void BuildTooltip(Card& card, App& app, const PassiveTree& tree, const Ring& d) {
    const PlanView& pv = app.Plan();
    const ActiveView& av = app.Active();
    const Node& n = tree.nodes[d.node];

    card.Accent(theme::Opaque(d.color));
    card.Line(NodeLabel(n, pv), theme::kText);
    AddStateLines(card, tree, pv, av, d);

    const BuildSpec* spec = av.Spec();
    if (spec) {
        for (const SpecNote& note : spec->notes) {
            if (tree.BaseId(note.nodeId) != n.id || note.text.empty()) continue;
            card.Separator();
            card.Caption(I18n::Tr("ov_tip_note"));
            card.Small(theme::Shortened(note.text, kMaxNoteBytes), theme::kText);
            break;
        }
    }
    const std::vector<std::string>& stats = n.StatsFor(pv.className, pv.ascendancyName);
    if (!stats.empty()) {
        card.Separator();
        int shown = 0;
        for (const std::string& line : stats) {
            if (shown++ >= kMaxTooltipStats) break;
            card.Small(line, theme::kMod);
        }
    }
    if (spec && av.build && n.Is(kNodeJewel)) {
        for (const SpecSocket& socket : spec->sockets) {
            if (tree.BaseId(socket.nodeId) != n.id) continue;
            if (socket.itemIndex < 0 || static_cast<size_t>(socket.itemIndex) >= av.build->items.size()) break;
            const BuildItem& item = av.build->items[static_cast<size_t>(socket.itemIndex)];
            card.Separator();
            card.Caption(I18n::Tr("ov_tip_jewel"));
            card.Line(item.DisplayName(), theme::RarityColor(item.rarity));
            if (!item.name.empty() && !item.base.empty()) card.Small(item.base, theme::RarityColor(item.rarity));
            int shown = 0;
            for (const ItemModLine& mod : item.explicits) {
                if (shown++ >= kMaxJewelMods) break;
                card.Small(mod.text, theme::ModLineColor(mod.tags));
            }
            break;
        }
    }
}

// Where the card of the hovered ring goes: below the node (the game's own tooltip sits above it):
// centred, else ending at the node's right edge (left of the game's key hints), else centred below
// those hints, else right of them; else beside the node, else beside or below a tooltip of the game,
// else above the node.
// Never on a tooltip of the game, the points panel, the title, our own window (`keepOff`) or the ring
// itself; off the HUD (`soft`) and the game's key hints (`hints`, ok == false: none) while another
// place is free, the HUD first. false = no place: no card this frame.
bool PlaceTooltip(const OverlayFrame& f, const Ring& d, const ImVec2& size, const Covers& keepOff, const Covers& soft, const Rect& hints,
                  const Covers& tips, ImVec2& out) {
    const float gap = PluginUi::SnapPx(Em() * 0.5f);
    const float ring = d.r + d.thick;
    ImVec2 candidates[7 + kMaxCovers * 3];
    int count = 0;
    candidates[count++] = ImVec2(d.x - size.x * 0.5f, d.y + ring + gap);
    candidates[count++] = ImVec2(d.x + ring - size.x, d.y + ring + gap);
    if (hints.ok) {
        candidates[count++] = ImVec2(d.x - size.x * 0.5f, hints.Bottom() + gap);
        candidates[count++] = ImVec2(hints.Right() + gap, d.y - ring);
    }
    candidates[count++] = ImVec2(d.x + ring + gap, d.y - ring);
    candidates[count++] = ImVec2(d.x - ring - gap - size.x, d.y - ring);
    for (int i = 0; i < tips.count; ++i) {
        const Rect& t = tips.rects[i];
        candidates[count++] = ImVec2(t.Right() + gap, d.y - ring);
        candidates[count++] = ImVec2(t.x - gap - size.x, d.y - ring);
        candidates[count++] = ImVec2(d.x - size.x * 0.5f, t.Bottom() + gap);
    }
    candidates[count++] = ImVec2(d.x - size.x * 0.5f, d.y - ring - gap - size.y);
    const Rect node{d.x - ring, d.y - ring, ring * 2.0f, ring * 2.0f, true};
    // Four passes, the strictest first: off the HUD and the key hints; off the HUD; off the hints; any.
    for (int pass = 0; pass < 4; ++pass) {
        const bool offHud = pass <= 1, offHints = pass == 0 || pass == 2;
        for (int i = 0; i < count; ++i) {
            const ImVec2 p = theme::ClampToClient(candidates[i], size, f.clientW, f.clientH);
            if (keepOff.Hits(p, size) || theme::Overlaps(node, p, size)) continue;
            if (offHud && soft.Hits(p, size)) continue;
            if (offHints && theme::Overlaps(hints, p, size)) continue;
            out = p;
            return true;
        }
    }
    return false;
}

void BuildSummary(Card& card, App& app, const PassiveTree& tree) {
    const Settings& s = app.Config();
    const OverlayFrame& f = app.Frame();
    const PlanView& pv = app.Plan();
    const ActiveView& av = app.Active();
    const Plan& plan = pv.plan;
    const CharState& ch = *f.character;

    // The build and its stage: one line when they fit, two when not.
    const std::string name = theme::Shortened(av.build->name, kMaxTitleBytes);
    const std::string stage = SpecLabel(*av.build, av.stages, av.sets.spec);
    card.Inline({{name, theme::kCaption}, {stage, theme::kTextDim}});

    const int take = plan.missing + plan.ascMissing, refund = plan.wrong + plan.ascWrong, adjust = plan.adjust;
    const std::string takeText = I18n::TrF("ov_sum_take", {{"n", Num(take)}});
    const std::string refundText = refund > 0 ? I18n::TrF("ov_sum_refund", {{"n", Num(refund)}}) : std::string();
    const std::string adjustText = adjust > 0 ? I18n::TrF("ov_sum_adjust", {{"n", Num(adjust)}}) : std::string();
    card.Inline({{takeText, theme::Opaque(s.colorMissing)}, {refundText, theme::Opaque(s.colorWrong)}, {adjustText, theme::Opaque(s.colorAdjust)}});

    const std::string freeText = I18n::TrF("ov_sum_free", {{"n", Num(ch.freePoints)}});
    const std::string setText = ch.weaponSetPoints > 0
                                    ? I18n::TrF("ov_sum_free_sets", {{"one", Num(ch.freeSet1)}, {"two", Num(ch.freeSet2)}})
                                    : std::string();
    const std::string ascText = ch.freeAscPoints > 0 ? I18n::TrF("ov_sum_free_asc", {{"n", Num(ch.freeAscPoints)}}) : std::string();
    card.Inline({{freeText, theme::kText}, {setText, theme::kTextDim}, {ascText, theme::kTextDim}}, 0.9f);

    // "Nothing left to take" needs the ascendancy done too; with the main tree done or blocked the
    // ascendancy's next objective is named instead (PlanSummary.h).
    const NextObjective next = PickNext(pv.numbers.objectives, pv.numbers.ascObjectives, plan.missing, plan.ascMissing);
    if (next.row && (next.kind == NextKind::Main || next.kind == NextKind::Ascendancy)) {
        const std::string objective = (next.row->node != kNoIndex && next.row->node < tree.nodes.size())
                                          ? NodeLabel(tree.nodes[next.row->node], pv)
                                          : std::string(I18n::Tr("ov_other_points"));
        const std::string cost = Num(next.row->cost);
        card.Line(next.kind == NextKind::Main ? I18n::TrF("ov_sum_next", {{"name", objective}, {"n", cost}})
                                              : I18n::TrF("ov_sum_next_asc", {{"name", objective}, {"n", cost}}),
                  theme::Opaque(s.colorNext));
    } else if (next.kind == NextKind::Done) {
        card.MarkLine(Mark::Tick, I18n::Tr("ov_sum_done"), theme::Opaque(s.colorMissing));
    } else if (!pv.classDiffers) {
        // With another class nothing is in reach by design: the class line below says what to do,
        // and "refund the red ones first" would send the player the wrong way.
        card.Small(I18n::Tr("ov_sum_blocked"));
    }

    if (pv.classDiffers) card.MarkLine(Mark::Dot, I18n::Tr("ov_sum_class_differs"), theme::kWarn);
    if (!plan.unknownTarget.empty())
        card.MarkLine(Mark::Dot, I18n::TrF("ov_sum_unknown", {{"n", Num(static_cast<int>(plan.unknownTarget.size()))}}), theme::kWarn);
    else if (pv.treeOlder) card.MarkLine(Mark::Dot, I18n::Tr("ov_sum_tree_older"), theme::kWarn);
}

void DrawTree(App& app) {
    const Settings& s = app.Config();
    const OverlayFrame& f = app.Frame();
    if (!s.showTree || !f.overlayActive || !f.treeOpen) return;
    const ActiveView& av = app.Active();
    if (!av.build) return;
    ImDrawList* dl = OverlayDrawList(app);
    if (!dl || !(f.clientW > 0.0f) || !(f.clientH > 0.0f)) return;

    const float em = Em();
    const ImVec2 boxAt(PluginUi::SnapPx(em * 0.75f), PluginUi::SnapPx(em * 0.75f));
    const float boxMaxWidth = std::max(em * 14.0f, std::min(em * 24.0f, f.clientW * 0.4f));

    // Fail closed: without the node table or the character's passives, one line that says why.
    if (const int warning = f.TreeWarning()) {
        const char* text = warning == 1 ? TreeProblemText(f.table->problem) : CharProblemText(f.character->problem);
        if (text && *text) {
            Card card(boxMaxWidth);
            card.MarkLine(Mark::Dot, text, theme::kWarn);
            card.Draw(dl, theme::ClampToClient(boxAt, card.Size(), f.clientW, f.clientH), s.cardOpacity);
        }
        return;
    }
    if (!f.DrawTree()) return;

    const PlanView& pv = app.Plan();
    const std::shared_ptr<const PassiveTree> treePtr = app.Tree();
    if (!pv.valid || !treePtr || f.table->tree.get() != treePtr.get()) return;
    const PassiveTree& tree = *treePtr;
    const TreeTable& table = *f.table;
    const TreePose& pose = f.pose;
    const Plan& plan = pv.plan;
    const size_t n = tree.nodes.size();
    if (plan.state.size() != n || plan.stepOf.size() != n || plan.wantSet.size() != n || plan.wantAttr.size() != n ||
        plan.adjustBits.size() != n || pv.numbers.number.size() != n || table.uiOfNode.size() != n)
        return;

    const float cw = f.clientW, ch = f.clientH;
    const float cull = em * 4.0f;   // px outside the client that still count as on screen
    const float px = em / 13.0f;    // Settings::lineWidth is in px at font size 13
    const float baseThick = std::max(1.5f, s.lineWidth * 0.8f) * px;

    // What covers the tree: the panel's own parts and the HUD, the game's tooltips, our own window.
    const theme::Avoid tips = theme::GameTooltips(pose, f.gearFrame, f.skillsFrame);
    const Rect window = theme::SkillsWindowBox();
    const int occluders = std::clamp(pose.occluderCount, 0, static_cast<int>(kMaxTreeOccluders));
    const int hudFrom = std::clamp(occluders - pose.hudOccluders, 0, occluders);   // the HUD's rectangles come last
    Covers covers;    // no ring, line or badge under these
    Covers fixed;     // the same without the game's tooltips
    Covers keepOff;   // no card on these
    Covers soft;      // no card on these while another place is free: the HUD
    Covers tipRects;  // a card may go beside these
    for (int i = 0; i < occluders; ++i) {
        fixed.Add(pose.occluders[i]);
        (i < hudFrom ? keepOff : soft).Add(pose.occluders[i]);
    }
    fixed.Add(window);
    covers = fixed;
    covers.Add(tips);
    keepOff.Add(tips);
    keepOff.Add(window);
    tipRects.Add(tips);
    // The stand-in while the game's tooltips cannot be read: rings in it are faint, cards stay off it.
    const Rect guess = table.tooltipSource.manager == 0 ? GuessGameTip(f) : Rect{};
    keepOff.Add(guess);
    tipRects.Add(guess);
    const uint64_t tipOwner = pose.tooltip.ok ? pose.tooltip.owner : 0;   // the passive the game's tooltip belongs to

    // Project every node that has a state once: lines need both ends, ringed or not.
    g_proj.assign(n, Proj{});
    g_rings.clear();
    for (uint32_t i = 0; i < n; ++i) {
        if (plan.state[i] == NodeState::None) continue;
        const TreeNodeUi* ui = table.OfNode(i);
        if (!ui || !ui->visible) continue;
        if (!s.showAscendancy && tree.nodes[i].IsAscendancy()) continue;
        const Vec2 c = pose.Centre(*ui);
        const float r = pose.Radius(*ui) * s.ringScale + 1.5f * px;
        if (!Finite(c.x, c.y) || !std::isfinite(r) || r <= 0.0f || r > cw + ch) continue;
        // Nothing under the points panel, the title, the HUD or a tooltip of the game. The passive the
        // game's tooltip belongs to keeps its ring while the tooltip only touches it (the game puts the
        // tooltip right above the node, and against it at the top of the client).
        if (ui->element == tipOwner ? (Covered(fixed, c.x, c.y, r + baseThick) || Covered(covers, c.x, c.y, pose.Radius(*ui) - 1.0f))
                                    : Covered(covers, c.x, c.y, r + baseThick))
            continue;
        Proj p{c.x, c.y, r, 1.0f, true};
        if (guess.Contains(c)) p.fade = kTipFade;   // where the game's own tooltip is expected
        g_proj[i] = p;
    }

    // Lines, under the rings. Width 0 is the thinnest line, a 1 px hairline, as the Display tab's tip
    // says; "Lines between passives" is what turns them off.
    if (s.showLines) {
        const float width = std::max(1.0f, s.lineWidth * px);
        for (const PlanEdge& e : plan.edges) {
            // A line says "go this way" (missing) or "this goes" (wrong). A node to adjust is taken
            // already: its links are drawn like those of any allocated node, its ring says the rest.
            const NodeState state = e.state == NodeState::Adjust ? NodeState::Ok : e.state;
            if (e.a >= n || e.b >= n || !Shown(s, state)) continue;
            const Proj& a = g_proj[e.a];
            const Proj& b = g_proj[e.b];
            if (!a.ok || !b.ok) continue;
            if (std::max(a.x, b.x) < -cull || std::max(a.y, b.y) < -cull || std::min(a.x, b.x) > cw + cull || std::min(a.y, b.y) > ch + cull)
                continue;
            DrawEdge(dl, tree, pose, covers, e, a, b, s.curvedLines, theme::WithAlpha(StateColor(s, state), 0.85f * std::min(a.fade, b.fade)), width);
        }
    }

    // Rings; step 1 brighter and thicker.
    const int numbersShown = s.numbersShown;
    for (uint32_t i = 0; i < n; ++i) {
        const Proj& p = g_proj[i];
        const NodeState st = plan.state[i];
        if (!p.ok || !Shown(s, st)) continue;
        if (p.x < -cull || p.y < -cull || p.x > cw + cull || p.y > ch + cull) continue;
        Ring d;
        d.node = i;
        d.x = p.x;
        d.y = p.y;
        d.r = p.r;
        d.fade = p.fade;
        d.state = st;
        d.number = pv.numbers.number[i];
        const bool next = st == NodeState::Missing && numbersShown >= 1 && d.number == 1;
        d.color = next ? s.colorNext : StateColor(s, st);
        d.thick = next ? baseThick * 1.8f : baseThick;
        const ImU32 color = theme::WithAlpha(d.color, d.fade);
        if (next) dl->AddCircle(ImVec2(d.x, d.y), d.r + d.thick * 1.6f, theme::WithAlpha(color, 0.35f), 0, d.thick);
        theme::DrawRingOutlined(dl, ImVec2(d.x, d.y), d.r, color, d.thick);
        g_rings.push_back(d);
    }

    // Badges, over the rings: the step number (upper right), the weapon set (lower right), the
    // attribute to pick (lower left). The small ones are left out when the tree is zoomed far out.
    const char* letterStr = I18n::Tr("ov_attr_letter_str");
    const char* letterDex = I18n::Tr("ov_attr_letter_dex");
    const char* letterInt = I18n::Tr("ov_attr_letter_int");
    const float smallScale = 0.78f;
    const float smallRadius = theme::BadgeRadius(smallScale);
    for (const Ring& d : g_rings) {
        if (d.fade < 1.0f) continue;
        if (d.r >= em * 0.42f && d.state != NodeState::Wrong) {
            const Node& node = tree.nodes[d.node];
            const WeaponSet set = plan.wantSet[d.node];
            const bool setMatters = d.state != NodeState::Adjust || (plan.adjustBits[d.node] & kAdjustWeaponSet) != 0;
            if (s.showSetBadges && set != WeaponSet::None && setMatters) {
                const ImVec2 at = BadgeAt(d, 1.0f, 1.0f, smallRadius);
                if (!Covered(covers, at.x, at.y, smallRadius))
                    theme::DrawBadge(dl, at, set == WeaponSet::One ? "I" : "II", set == WeaponSet::One ? theme::kSetOne : theme::kSetTwo, smallScale);
            }
            const Attr attr = plan.wantAttr[d.node];
            const bool attrMatters = d.state != NodeState::Adjust || (plan.adjustBits[d.node] & kAdjustAttribute) != 0;
            if (s.showAttrBadges && node.Is(kNodeAttribute) && attr != Attr::None && attrMatters) {
                const ImVec2 at = BadgeAt(d, -1.0f, 1.0f, smallRadius);
                if (!Covered(covers, at.x, at.y, smallRadius))
                    theme::DrawSolidBadge(dl, at, attr == Attr::Str ? letterStr : attr == Attr::Dex ? letterDex : letterInt, theme::AttrColor(attr),
                                          smallScale);
            }
        }
        if (d.state == NodeState::Missing && d.number > 0 && d.number <= numbersShown) {
            char text[16];
            std::snprintf(text, sizeof text, "%d", static_cast<int>(d.number));
            const float scale = d.number == 1 ? 1.0f : 0.84f;
            const float radius = theme::BadgeRadius(scale);
            // Upper right; next to the title or the points panel the first corner that is free of them.
            static constexpr float kCorners[4][2] = {{1.0f, -1.0f}, {-1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}};
            for (const auto& corner : kCorners) {
                const ImVec2 at = BadgeAt(d, corner[0], corner[1], radius);
                if (Covered(covers, at.x, at.y, radius)) continue;
                theme::DrawBadge(dl, at, text, theme::Opaque(d.color), scale);
                break;
            }
        }
    }

    // The ring under the cursor: the overlay takes no input, so the test is ours.
    const Ring* hovered = nullptr;
    if (s.showTreeTooltip && f.mouseValid) {
        float bestD = FLT_MAX;
        for (const Ring& d : g_rings) {
            const float dx = f.mouse.x - d.x, dy = f.mouse.y - d.y, reach = d.r + d.thick * 0.5f + 1.0f;
            const float dd = dx * dx + dy * dy;
            if (dd <= reach * reach && dd < bestD) {
                bestD = dd;
                hovered = &d;
            }
        }
    }

    // The summary, top left. It steps aside for a tooltip of the game, for our own window, for the
    // card of the hovered ring when the two would overlap, and while the cursor is on it (so that the
    // nodes under it can be seen and hovered).
    Card summary(boxMaxWidth);
    ImVec2 summaryPos, summarySize;
    bool showSummary = false;
    if (s.showSummary) {
        BuildSummary(summary, app, tree);
        summarySize = summary.Size();
        summaryPos = theme::ClampToClient(boxAt, summarySize, cw, ch);
        const Rect box{summaryPos.x, summaryPos.y, summarySize.x, summarySize.y, true};
        showSummary = !(f.mouseValid && box.Contains(f.mouse)) && !tips.Hits(summaryPos, summarySize) && !theme::Overlaps(window, summaryPos, summarySize);
    }
    if (hovered) {
        Card tooltip(std::min(em * 21.0f, std::max(em * 10.0f, cw * 0.4f)));
        BuildTooltip(tooltip, app, tree, *hovered);
        const ImVec2 size = tooltip.Size();
        // With its tooltip the game shows key hints ("Inspect", "Compare") right of the passive: two
        // short lines from the node's middle down (seen on the captures; they are no part of the
        // tooltip's rectangle).
        Rect hints;
        if (tipRects.count > 0) hints = Rect{hovered->x + hovered->r, hovered->y - hovered->r, ch * 0.17f, hovered->r * 2.0f + ch * 0.045f, true};
        ImVec2 pos;
        if (PlaceTooltip(f, *hovered, size, keepOff, soft, hints, tipRects, pos)) {
            const Rect box{pos.x, pos.y, size.x, size.y, true};
            if (showSummary && !theme::Overlaps(box, summaryPos, summarySize)) summary.Draw(dl, summaryPos, s.cardOpacity);
            tooltip.Draw(dl, pos, s.cardOpacity);
            return;
        }
    }
    if (showSummary) summary.Draw(dl, summaryPos, s.cardOpacity);
}

} // namespace

void DrawTreeOverlay(App& app) {
    try {
        DrawTree(app);
    } catch (...) {
        // Nothing here opens an ImGui scope, so the stacks are balanced: this frame's tree overlay is dropped.
    }
}

} // namespace bp
