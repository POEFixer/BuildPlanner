// BuildPlanner - the passive tree panel (reader)
// TreeReader.cpp - see TreeReader.h. Reads the game READ-ONLY through IGame; every count, pointer and
// float that comes from the game is checked before it is used.
//
// Rules this file adds to the header's contract (tools/make_fixture.py mirrors the walk read for read):
//   - A panel candidate is an element of unscaled size 2560 x 1600 (+-1) with 1..64 children; its
//     canvas is the child with >= 1000 children (child Offsets::canvasChildIndex first, else the one
//     with the most). A candidate passes only when a sample of the canvas' grandchildren (24 canvas
//     children spread over the list, their first two children each) holds >= 6 known passive ids.
//   - The node walk: every canvas child with 1..256 children is a group; each of its children is a
//     candidate that is a node when its id chain gives a known id. A child that is no node but has
//     1..256 children of its own is a group inside a container (the ascendancy container): its
//     children are the candidates instead. A node's own children (a socketed jewel) are never walked.
//   - Elements whose ids name kNodeOnlyImage / kNodeClassStart passives (the mastery art) count as
//     resolved but are not in the table: nothing is drawn or allocated there. An id met twice keeps the
//     first element unless only the later one is visible.
//   - Over 1 % of the walked elements unreadable is a torn read (ReadFailed, retried), not a mismatch.
//   - The occluders are the panel's children other than the canvas covering 0..25 % of the panel; the
//     table lists the visible ones (largest first), refreshed every tick while the tree is open.
//   - The HUD's parts are found by control name (UiFind depth 1 from the panel root, the HUD
//     and its side frames) with the table; while the tree is open the HUD's visible bit is read every
//     tick, a HUD not found is looked for again every second, and the tooltip source (GearReader.h) is
//     checked every 2 s. A change publishes a copy, like the occluders.
#include "TreeReader.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace bp {
namespace {

constexpr float kPanelW = 2560.0f, kPanelH = 1600.0f, kSizeTolerance = 1.0f;
constexpr int kMaxPanelChildren = 64;
constexpr int kMinCanvasChildren = 1000;
constexpr int kMaxGroupChildren = 256;
constexpr double kMinResolvedShare = 0.95;
constexpr double kMinCoverage = 0.90;
constexpr size_t kSampleGroups = 24, kSamplePerGroup = 2;
constexpr int kSampleMinKnown = 6;
constexpr size_t kMaxOccluderCandidates = 8;
constexpr float kOccluderMaxShare = 0.25f;
constexpr int64_t kRevalidateMs = 250;
constexpr int64_t kVisibilityRefreshMs = 1000;
constexpr size_t kRevalidateProbes = 4;
constexpr float kMinScale = 1e-4f, kMaxScale = 50.0f;
constexpr float kMaxCoord = 1e7f, kMaxNodeSize = 1e5f;
constexpr int64_t kHudRetryMs = 1000;
constexpr int64_t kTooltipSourceMs = 2000;
// A DataMismatch found on a canvas is trusted this long while the canvas stands unchanged: the
// verdict depends only on the tree data and the UI, so re-walking it every 2 s cost ~20k
// host calls for nothing. The full walk still runs once a minute, so a UI that was not yet complete
// when it was walked cannot keep a wrong verdict for the whole session.
constexpr int64_t kMismatchRewalkMs = 60000;
constexpr float kMaxHudCoord = 1e6f;

bool Finite(float v) { return std::isfinite(v); }

bool IsPanelSize(const UiElem& e) {
    return e.valid && std::fabs(e.w - kPanelW) <= kSizeTolerance && std::fabs(e.h - kPanelH) <= kSizeTolerance;
}

bool GroupLike(const UiElem& e) { return e.valid && e.childCount >= 1 && e.childCount <= kMaxGroupChildren; }

// The node id chain: [element + nodeSkillInfo] -> [+ skillInfoRow] -> u16 at + rowGraphId.
std::optional<uint16_t> ResolveId(IGame& game, const Offsets& o, uint64_t element) {
    const uint64_t info = game.Ptr(element + o.nodeSkillInfo);
    if (!PlausibleObject(info)) return std::nullopt;
    const uint64_t row = game.Ptr(info + o.skillInfoRow);
    if (!PlausiblePtr(row)) return std::nullopt;
    uint16_t id = 0;
    if (!game.Get(row + o.rowGraphId, id)) return std::nullopt;
    return id;
}

uint32_t KnownIndex(const PassiveTree& tree, std::optional<uint16_t> id) {
    return id ? tree.IndexOf(*id) : kNoIndex;
}

// The first element below `under` (depth 1, hidden ones too) with this control name; 0 = none.
uint64_t FindNamed(IGame& g, uint64_t under, const char* name) {
    if (under == 0) return 0;
    for (const uint64_t a : g.UiFind(under, name, 1, false, 2))
        if (a != under && PlausibleObject(a)) return a;
    return 0;
}

// The HUD's parts by control name (TreeReader.h TreeHud). Nothing when the HUD or both frames are missing.
TreeHud FindHud(IGame& g, uint64_t root) {
    TreeHud h;
    h.container = FindNamed(g, root, "HUD");
    if (h.container == 0) return {};
    h.frameL = FindNamed(g, h.container, "HUDLeft");
    h.frameR = FindNamed(g, h.container, "HUDRight");
    if (h.frameL == 0 && h.frameR == 0) return {};
    h.orbL = FindNamed(g, h.frameL, "energy_shield_frame");
    if (h.orbL == 0) h.orbL = FindNamed(g, h.container, "life_orb");
    h.orbR = FindNamed(g, h.frameR, "spirit_bar_frame");
    if (h.orbR == 0) h.orbR = FindNamed(g, h.container, "mana_orb");
    h.xpBar = FindNamed(g, h.container, "experience_bar");
    h.barTop = FindNamed(g, h.frameL, "poise");
    return h;
}

bool UsableHudRect(const Rect& r) {
    return r.ok && Finite(r.x) && Finite(r.y) && Finite(r.w) && Finite(r.h) && r.w > 0.0f && r.h > 0.0f &&
           std::fabs(r.x) < kMaxHudCoord && std::fabs(r.y) < kMaxHudCoord && r.w < kMaxHudCoord && r.h < kMaxHudCoord;
}

struct BuildResult {
    TreeProblem problem = TreeProblem::NoPanel;
    std::shared_ptr<TreeTable> table;   // set for None and for the diagnostics of a mismatch
};

} // namespace

struct TreeReaderImpl {
    // A problem table, reused while the problem and its diagnostics stay the same. The UI it names
    // (panel root, canvas, child count) is part of the diagnostics: a remembered DataMismatch is keyed
    // by them (MismatchStands), so a rebuilt canvas must not hide behind the previous table.
    static std::shared_ptr<const TreeTable> Problem(TreeReader& r, TreeProblem p, const TreeTable* diag = nullptr) {
        const TreeTable* last = r.m_problem.get();
        if (last && last->problem == p && last->tree == r.m_tree &&
            (diag == nullptr ? last->candidates == 0 && last->panel == 0
                             : last->candidates == diag->candidates && last->resolved == diag->resolved &&
                                   last->panel == diag->panel && last->coverage == diag->coverage &&
                                   last->uiRoot == diag->uiRoot && last->canvas == diag->canvas &&
                                   last->canvasChildren == diag->canvasChildren))
            return r.m_problem;
        auto t = std::make_shared<TreeTable>();
        if (diag) {
            t->uiRoot = diag->uiRoot;
            t->panel = diag->panel;
            t->canvas = diag->canvas;
            t->canvasW = diag->canvasW;
            t->canvasH = diag->canvasH;
            t->canvasChildren = diag->canvasChildren;
            t->ascendancy = diag->ascendancy;
            t->candidates = diag->candidates;
            t->resolved = diag->resolved;
            t->coverage = diag->coverage;
            t->viaShortcut = diag->viaShortcut;
            t->scaleCheck = diag->scaleCheck;
        }
        t->problem = p;
        t->found = false;
        t->tree = r.m_tree;
        t->serial = r.m_nextSerial++;
        r.m_problem = t;
        return r.m_problem;
    }

    static void DropTable(TreeReader& r) {
        r.m_table.reset();
        r.m_occluderCandidates.clear();
        r.m_probe = 0;
        r.m_mismatch.reset();
    }

    // Does the remembered DataMismatch still stand: the same tree data, the same panel root, the panel
    // still a tree panel, the same canvas under it with the same child count, walked less than
    // kMismatchRewalkMs ago? GameUiRoot and two UiReads; no node is resolved.
    static bool MismatchStands(TreeReader& r, int64_t now) {
        const TreeTable* k = r.m_mismatch.get();
        if (!k || k->tree != r.m_tree || k->canvas == 0 || k->panel == 0) return false;
        if (now - r.m_mismatchMs >= kMismatchRewalkMs || now < r.m_mismatchMs) return false;
        if (r.m_game.GameUiRoot() != k->uiRoot) return false;
        const UiElem pe = r.m_game.UiRead(k->panel);
        if (!IsPanelSize(pe)) return false;
        const UiElem ce = r.m_game.UiRead(k->canvas);
        return ce.valid && ce.parent == k->panel && ce.childCount == k->canvasChildren;
    }

    // The canvas of a panel candidate: (address, element), address 0 when it has none.
    static std::pair<uint64_t, UiElem> FindCanvas(TreeReader& r, const std::vector<uint64_t>& kids) {
        const size_t hint = r.m_off.canvasChildIndex;
        if (hint < kids.size()) {
            const UiElem e = r.m_game.UiRead(kids[hint]);
            if (e.valid && e.childCount >= kMinCanvasChildren) return {kids[hint], e};
        }
        std::pair<uint64_t, UiElem> best{0, UiElem{}};
        for (size_t i = 0; i < kids.size(); ++i) {
            if (i == hint) continue;
            const UiElem e = r.m_game.UiRead(kids[i]);
            if (e.valid && e.childCount >= kMinCanvasChildren && (best.first == 0 || e.childCount > best.second.childCount))
                best = {kids[i], e};
        }
        return best;
    }

    // Do the canvas' grandchildren hold passives at all? A cheap look before the full walk.
    static bool SampleKnows(TreeReader& r, uint64_t canvas) {
        const std::vector<uint64_t> groups = r.m_game.UiChildren(canvas);
        if (groups.empty()) return false;
        const size_t step = std::max<size_t>(1, groups.size() / kSampleGroups);
        int known = 0;
        for (size_t i = 0; i < groups.size() && known < kSampleMinKnown; i += step) {
            const std::vector<uint64_t> kids = r.m_game.UiChildren(groups[i]);
            for (size_t k = 0; k < kids.size() && k < kSamplePerGroup; ++k)
                if (KnownIndex(*r.m_tree, ResolveId(r.m_game, r.m_off, kids[k])) != kNoIndex) ++known;
        }
        return known >= kSampleMinKnown;
    }

    // The full node walk over one canvas.
    static BuildResult Build(TreeReader& r, uint64_t root, uint64_t panel, const std::vector<uint64_t>& panelKids,
                             uint64_t canvas, const UiElem& ce, bool viaShortcut) {
        IGame& g = r.m_game;
        const PassiveTree& tree = *r.m_tree;
        BuildResult res;
        auto t = std::make_shared<TreeTable>();
        t->tree = r.m_tree;
        t->uiRoot = root;
        t->panel = panel;
        t->canvas = canvas;
        t->canvasW = ce.w;
        t->canvasH = ce.h;
        t->viaShortcut = viaShortcut;
        res.table = t;

        const std::vector<uint64_t> canvasKids = g.UiChildren(canvas);
        t->canvasChildren = static_cast<int>(canvasKids.size());
        if (canvasKids.empty() || static_cast<int>(canvasKids.size()) != ce.childCount || !(ce.w > 0.0f) || !Finite(ce.w)) {
            res.problem = TreeProblem::ReadFailed;
            return res;
        }

        std::vector<TreeNodeUi> found;
        std::vector<uint32_t> slot(tree.nodes.size(), kNoIndex);   // tree index -> index into `found`
        int candidates = 0, resolved = 0, unreadable = 0, walked = 0;

        // chain[0] is a canvas child; local() per the header's projection rule.
        const auto add = [&](uint64_t element, const UiElem& e, uint32_t idx, const UiElem* const* chain, size_t depth) {
            const Node& node = tree.nodes[idx];
            if (!node.Allocatable()) return;
            float lx = 0.0f, ly = 0.0f;
            for (size_t i = 0; i < depth; ++i) {
                const UiElem& x = *chain[i];
                if (x.scaleIndex != ce.scaleIndex) return;   // the closed-form projection needs one scale space
                lx += x.relX;
                ly += x.relY;
                if (i == 0) {
                    if (!x.modPos) {   // the pose adds the canvas modifier to every node: take it back out
                        lx -= ce.modX;
                        ly -= ce.modY;
                    }
                } else if (x.modPos) {
                    lx += chain[i - 1]->modX;
                    ly += chain[i - 1]->modY;
                }
            }
            if (!Finite(lx) || !Finite(ly) || std::fabs(lx) > kMaxCoord || std::fabs(ly) > kMaxCoord) return;
            if (!Finite(e.w) || !Finite(e.h) || !(e.w > 0.0f) || !(e.h > 0.0f) || e.w > kMaxNodeSize || e.h > kMaxNodeSize) return;
            TreeNodeUi n;
            n.id = node.id;
            n.node = idx;
            n.element = element;
            n.localX = lx;
            n.localY = ly;
            n.w = e.w;
            n.h = e.h;
            n.visible = e.visible;
            if (slot[idx] != kNoIndex) {
                TreeNodeUi& kept = found[slot[idx]];
                if (!kept.visible && n.visible) kept = n;
                return;
            }
            slot[idx] = static_cast<uint32_t>(found.size());
            found.push_back(n);
        };

        for (const uint64_t c : canvasKids) {
            const UiElem cE = g.UiRead(c);
            ++walked;
            if (!cE.valid) {
                ++unreadable;
                continue;
            }
            if (!GroupLike(cE)) continue;
            for (const uint64_t k : g.UiChildren(c)) {
                const UiElem kE = g.UiRead(k);
                ++walked;
                if (!kE.valid) {
                    ++unreadable;
                    continue;
                }
                const uint32_t idx = KnownIndex(tree, ResolveId(g, r.m_off, k));
                if (idx != kNoIndex) {
                    ++candidates;
                    ++resolved;
                    const UiElem* chain[2] = {&cE, &kE};
                    add(k, kE, idx, chain, 2);
                } else if (GroupLike(kE)) {
                    for (const uint64_t n : g.UiChildren(k)) {
                        const UiElem nE = g.UiRead(n);
                        ++walked;
                        if (!nE.valid) {
                            ++unreadable;
                            continue;
                        }
                        ++candidates;
                        const uint32_t idx2 = KnownIndex(tree, ResolveId(g, r.m_off, n));
                        if (idx2 == kNoIndex) continue;
                        ++resolved;
                        const UiElem* chain[3] = {&cE, &kE, &nE};
                        add(n, nE, idx2, chain, 3);
                    }
                } else {
                    ++candidates;
                }
            }
        }

        // The ascendancy shown: the most frequent Node::ascendancy among the nodes found.
        std::map<std::string, int> ascCount;
        for (const TreeNodeUi& n : found)
            if (tree.nodes[n.node].IsAscendancy()) ++ascCount[tree.nodes[n.node].ascendancy];
        int bestCount = 0;
        for (const auto& [name, count] : ascCount)
            if (count > bestCount) {
                bestCount = count;
                t->ascendancy = name;
            }
        int covered = 0;
        for (const TreeNodeUi& n : found) {
            const Node& node = tree.nodes[n.node];
            if (!node.IsAscendancy() || node.ascendancy == t->ascendancy) ++covered;
        }
        const int allocatable = tree.AllocatableCount(t->ascendancy);
        t->candidates = candidates;
        t->resolved = resolved;
        t->coverage = allocatable > 0 ? std::min(1.0f, static_cast<float>(covered) / static_cast<float>(allocatable)) : 0.0f;

        // The game's own canvas scale against the host's (diagnostics).
        float gameScale = 0.0f;
        const Rect cr = g.UiRect(canvas);
        if (cr.ok && g.Get(canvas + r.m_off.elemEffectiveScale, gameScale) && Finite(gameScale) && gameScale > 0.0f &&
            Finite(cr.w) && cr.w > 0.0f)
            t->scaleCheck = gameScale / (cr.w / ce.w);

        const double share = candidates > 0 ? static_cast<double>(resolved) / candidates : 0.0;
        if (static_cast<int64_t>(unreadable) * 100 > walked) {
            res.problem = TreeProblem::ReadFailed;
            return res;
        }
        if (resolved == 0) {
            res.problem = TreeProblem::NoNodes;
            return res;
        }
        if (share < kMinResolvedShare || t->coverage < kMinCoverage) {
            res.problem = TreeProblem::DataMismatch;
            return res;
        }

        std::sort(found.begin(), found.end(), [](const TreeNodeUi& a, const TreeNodeUi& b) { return a.id < b.id; });
        t->uiOfNode.assign(tree.nodes.size(), kNoIndex);
        for (size_t i = 0; i < found.size(); ++i) t->uiOfNode[found[i].node] = static_cast<uint32_t>(i);
        t->nodes = std::move(found);

        // Occluder candidates: the panel's other children covering at most a quarter of it.
        struct Occ {
            uint64_t addr;
            float area;
            size_t index;
            bool visible;
        };
        std::vector<Occ> occ;
        for (size_t i = 0; i < panelKids.size(); ++i) {
            if (panelKids[i] == canvas) continue;
            const UiElem e = g.UiRead(panelKids[i]);
            if (!e.valid || !Finite(e.w) || !Finite(e.h)) continue;
            const float area = e.w * e.h;
            if (area > 0.0f && area <= kOccluderMaxShare * kPanelW * kPanelH) occ.push_back({panelKids[i], area, i, e.visible});
        }
        std::stable_sort(occ.begin(), occ.end(), [](const Occ& a, const Occ& b) { return a.area > b.area; });
        if (occ.size() > kMaxOccluderCandidates) occ.resize(kMaxOccluderCandidates);
        r.m_occluderCandidates.clear();
        for (const Occ& o : occ) {
            r.m_occluderCandidates.push_back(o.addr);
            if (o.visible && t->occluders.size() < kMaxPanelOccluders) t->occluders.push_back(o.addr);
        }

        t->hud = FindHud(g, root);
        if (t->hud.container != 0) {
            const UiElem he = g.UiRead(t->hud.container);
            t->hudVisible = he.valid && he.visible;
        }
        t->tooltipSource = FindTooltipSource(g, r.m_off, root);

        t->problem = TreeProblem::None;
        t->found = true;
        res.problem = TreeProblem::None;
        return res;
    }

    // One panel candidate. NoPanel = not the tree panel at all.
    static BuildResult TryPanel(TreeReader& r, uint64_t root, uint64_t panel, bool viaShortcut) {
        BuildResult res;
        const UiElem pe = r.m_game.UiRead(panel);
        if (!IsPanelSize(pe) || pe.childCount < 1 || pe.childCount > kMaxPanelChildren) return res;
        const std::vector<uint64_t> kids = r.m_game.UiChildren(panel);
        if (kids.empty()) return res;
        const auto [canvas, ce] = FindCanvas(r, kids);
        if (canvas == 0) {
            res.problem = viaShortcut ? TreeProblem::NoCanvas : TreeProblem::NoPanel;
            return res;
        }
        if (!SampleKnows(r, canvas)) {
            res.problem = TreeProblem::NoNodes;
            return res;
        }
        return Build(r, root, panel, kids, canvas, ce, viaShortcut);
    }

    static int Rank(TreeProblem p) {
        switch (p) {
        case TreeProblem::DataMismatch: return 5;
        case TreeProblem::ReadFailed: return 4;
        case TreeProblem::NoNodes: return 3;
        case TreeProblem::NoCanvas: return 2;
        default: return 1;
        }
    }

    // Discovery: the shortcut slot first (it must pass the same test), then every panel-root child.
    static BuildResult Discover(TreeReader& r) {
        BuildResult best;
        const uint64_t root = r.m_game.GameUiRoot();
        const UiElem rootEl = root == 0 ? UiElem{} : r.m_game.UiRead(root);
        if (!rootEl.valid) {
            best.problem = TreeProblem::NoUiRoot;
            return best;
        }
        uint64_t shortcut = 0;
        if (r.m_off.treePanelSlot != 0) {
            shortcut = r.m_game.Ptr(root + r.m_off.treePanelSlot);
            if (PlausibleObject(shortcut)) {
                BuildResult res = TryPanel(r, root, shortcut, true);
                if (res.problem == TreeProblem::None || res.problem == TreeProblem::DataMismatch ||
                    res.problem == TreeProblem::ReadFailed)
                    return res;   // the shortcut's panel is the tree panel: its verdict stands
                best = std::move(res);
            } else {
                shortcut = 0;
            }
        }
        const std::vector<uint64_t> kids = r.m_game.UiChildren(root);
        if (kids.empty()) {
            // A root that says it has children but whose list cannot be read is a torn read.
            if (rootEl.childCount > 0 && Rank(TreeProblem::ReadFailed) > Rank(best.problem)) best.problem = TreeProblem::ReadFailed;
            return best;
        }
        for (const uint64_t k : kids) {
            if (k == shortcut || !PlausibleObject(k)) continue;
            BuildResult res = TryPanel(r, root, k, false);
            if (res.problem == TreeProblem::None || res.problem == TreeProblem::DataMismatch ||
                res.problem == TreeProblem::ReadFailed)
                return res;
            if (Rank(res.problem) > Rank(best.problem)) best = std::move(res);
        }
        return best;
    }

    // Cheap checks of a found table: the panel root, the canvas, its child count, a few id chains.
    static bool Revalidate(TreeReader& r, const TreeTable& t) {
        if (r.m_game.GameUiRoot() != t.uiRoot) return false;
        const UiElem ce = r.m_game.UiRead(t.canvas);
        if (!ce.valid || ce.childCount != t.canvasChildren || ce.parent != t.panel || std::fabs(ce.w - t.canvasW) > kSizeTolerance)
            return false;
        if (t.nodes.empty()) return false;
        const size_t n = t.nodes.size();
        const size_t stride = std::max<size_t>(1, n / kRevalidateProbes);
        for (size_t i = 0; i < kRevalidateProbes && i < n; ++i) {
            const TreeNodeUi& node = t.nodes[(r.m_probe + i * stride) % n];
            const std::optional<uint16_t> id = ResolveId(r.m_game, r.m_off, node.element);
            if (!id || t.tree->BaseId(*id) != node.id) return false;
        }
        ++r.m_probe;
        return true;
    }

    // While the tree is open: the visible occluders every tick, the constrained nodes' visible bits
    // once a second (and when the panel opens). A change publishes a copy with a new serial.
    static void Refresh(TreeReader& r, bool justOpened, int64_t now) {
        const TreeTable& t = *r.m_table;
        std::vector<uint64_t> occluders;
        for (const uint64_t o : r.m_occluderCandidates) {
            const UiElem e = r.m_game.UiRead(o);
            if (e.valid && e.visible && occluders.size() < kMaxPanelOccluders) occluders.push_back(o);
        }
        // The HUD: its visible bit every tick; looked for again every second while it is not known.
        TreeHud hud = t.hud;
        bool hudVisible = false;
        if (hud.container != 0) {
            const UiElem he = r.m_game.UiRead(hud.container);
            if (he.valid) hudVisible = he.visible;
            else hud = TreeHud{};   // replaced or gone: look for it again
        }
        if (hud.container == 0 && (justOpened || now - r.m_lastHudMs >= kHudRetryMs || now < r.m_lastHudMs)) {
            r.m_lastHudMs = now;
            hud = FindHud(r.m_game, t.uiRoot);
            if (hud.container != 0) {
                const UiElem he = r.m_game.UiRead(hud.container);
                hudVisible = he.valid && he.visible;
            }
        }
        TooltipSource source = t.tooltipSource;
        if (justOpened || now - r.m_lastSourceMs >= kTooltipSourceMs || now < r.m_lastSourceMs) {
            r.m_lastSourceMs = now;
            source = FindTooltipSource(r.m_game, r.m_off, t.uiRoot);
        }
        std::vector<std::pair<size_t, bool>> flips;
        if (justOpened || now - r.m_lastVisibleMs >= kVisibilityRefreshMs || now < r.m_lastVisibleMs) {
            r.m_lastVisibleMs = now;
            for (size_t i = 0; i < t.nodes.size(); ++i) {
                if (!t.tree->nodes[t.nodes[i].node].Is(kNodeConstrained)) continue;
                const UiElem e = r.m_game.UiRead(t.nodes[i].element);
                if (e.valid && e.visible != t.nodes[i].visible) flips.emplace_back(i, e.visible);
            }
        }
        if (flips.empty() && occluders == t.occluders && hud == t.hud && hudVisible == t.hudVisible && source == t.tooltipSource)
            return;
        auto copy = std::make_shared<TreeTable>(t);
        for (const auto& [i, visible] : flips) copy->nodes[i].visible = visible;
        copy->occluders = std::move(occluders);
        copy->hud = hud;
        copy->hudVisible = hudVisible;
        copy->tooltipSource = source;
        copy->serial = r.m_nextSerial++;
        r.m_table = std::move(copy);
    }

    static TreeWatch Tick(TreeReader& r, const WatchTick& t) {
        TreeWatch w;
        const int64_t now = t.nowMs;
        if (!r.m_tree || r.m_tree->Empty()) {
            w.table = Problem(r, TreeProblem::NoTreeData);
            w.open = t.status.treeVisible;
            return w;
        }
        const bool areaChanged = r.m_haveArea && t.status.areaSerial != r.m_areaSerial;
        r.m_areaSerial = t.status.areaSerial;
        r.m_haveArea = true;
        if (!t.status.attached || !t.status.inGame) {
            if (areaChanged) DropTable(r);
            r.m_wasOpen = false;
            r.m_forceValidate = true;   // a kept table is checked again on the way back
            w.table = Problem(r, TreeProblem::NotInGame);
            return w;
        }
        if (areaChanged) {
            r.m_forceValidate = true;
            r.m_retry.Kick();
        }

        if (r.m_table) {
            const UiElem panel = r.m_game.UiRead(r.m_table->panel);
            const bool open = panel.valid && panel.visible;
            const bool justOpened = open && !r.m_wasOpen;
            const bool due = r.m_forceValidate || justOpened || now - r.m_lastValidateMs >= kRevalidateMs || now < r.m_lastValidateMs;
            if (due) {
                r.m_forceValidate = false;
                r.m_lastValidateMs = now;
                if (!panel.valid || !Revalidate(r, *r.m_table)) {
                    DropTable(r);
                    r.m_retry.Kick();   // the UI was rebuilt: look again now, not after a retry wait
                }
            }
            if (r.m_table) {
                if (open) Refresh(r, justOpened, now);
                r.m_wasOpen = open;
                w.table = r.m_table;
                w.open = open;
                return w;
            }
        }

        const bool open = t.status.treeVisible;
        if (open && !r.m_wasOpen) {   // the panel opened
            r.m_retry.Kick();
            // A remembered mismatch may come from a walk of a tree the game had not finished building:
            // the player opening the panel is the moment to look again in full, once.
            r.m_mismatch.reset();
        }
        r.m_wasOpen = open;
        r.m_forceValidate = false;
        if (!r.m_retry.Due(now)) {
            w.table = r.m_problem ? r.m_problem : Problem(r, TreeProblem::NoPanel);
            w.open = open;
            return w;
        }
        if (MismatchStands(r, now)) {   // the full walk would only find the same mismatch again
            r.m_retry.Failed(now);
            r.m_problem = r.m_mismatch;
            w.table = r.m_mismatch;
            w.open = open;
            return w;
        }
        r.m_mismatch.reset();
        BuildResult res = Discover(r);
        if (res.problem == TreeProblem::None && res.table) {
            r.m_retry.Succeeded();
            res.table->serial = r.m_nextSerial++;
            r.m_table = res.table;
            r.m_lastValidateMs = now;
            r.m_lastVisibleMs = now;
            r.m_lastHudMs = now;
            r.m_lastSourceMs = now;
            const UiElem panel = r.m_game.UiRead(r.m_table->panel);
            w.open = panel.valid && panel.visible;
            r.m_wasOpen = w.open;
            w.table = r.m_table;
            return w;
        }
        r.m_retry.Failed(now);
        w.table = Problem(r, res.problem, res.table.get());
        if (res.problem == TreeProblem::DataMismatch && w.table->canvas != 0) {
            r.m_mismatch = w.table;
            r.m_mismatchMs = now;
        }
        w.open = open;
        return w;
    }
};

TreeReader::TreeReader(IGame& game, const Offsets& offsets) : m_game(game), m_off(offsets) {}

void TreeReader::SetTree(std::shared_ptr<const PassiveTree> tree) {
    if (tree == m_tree) return;
    m_tree = std::move(tree);
    TreeReaderImpl::DropTable(*this);
    m_problem.reset();
    m_retry.Kick();
    m_forceValidate = true;
}

void TreeReader::Kick() {
    m_retry.Kick();
    m_forceValidate = true;
}

void TreeReader::Reset() {
    TreeReaderImpl::DropTable(*this);
    m_problem.reset();
    m_retry = RetryTimer{};
    m_areaSerial = 0;
    m_haveArea = false;
    m_lastValidateMs = 0;
    m_lastVisibleMs = 0;
    m_lastHudMs = 0;
    m_lastSourceMs = 0;
    m_wasOpen = false;
    m_forceValidate = true;
}

TreeWatch TreeReader::Tick(const WatchTick& t) {
    try {
        return TreeReaderImpl::Tick(*this, t);
    } catch (...) {
        TreeWatch w;
        try {
            TreeReaderImpl::DropTable(*this);
            m_retry.Failed(t.nowMs);
            w.table = TreeReaderImpl::Problem(*this, TreeProblem::ReadFailed);
        } catch (...) {
            w.table = std::make_shared<const TreeTable>();   // still never null
        }
        return w;
    }
}

int HudOccluderRects(const Rect (&parts)[kHudParts], Rect (&out)[kMaxHudOccluders]) {
    enum { kFrameL, kFrameR, kOrbL, kOrbR, kXp, kTop };
    int n = 0;
    const auto add = [&](float x0, float y0, float x1, float y1) {
        if (n >= static_cast<int>(kMaxHudOccluders) || !Finite(x0) || !Finite(y0) || !Finite(x1) || !Finite(y1) || x1 - x0 < 0.5f ||
            y1 - y0 < 0.5f)
            return;
        Rect r;
        r.x = x0;
        r.y = y0;
        r.w = x1 - x0;
        r.h = y1 - y0;
        r.ok = true;
        out[static_cast<size_t>(n++)] = r;
    };
    for (Rect& r : out) r = Rect{};
    const Rect& fl = parts[kFrameL];
    const Rect& fr = parts[kFrameR];
    const Rect& xp = parts[kXp];
    const bool hasL = UsableHudRect(fl), hasR = UsableHudRect(fr), hasXp = UsableHudRect(xp);
    const Rect* ref = hasL ? &fl : hasR ? &fr : nullptr;
    if (ref == nullptr) return 0;
    float topL = 0.0f;
    if (hasL) {
        const Rect& orb = parts[kOrbL];
        const float orbRight =
            UsableHudRect(orb) && orb.Right() > fl.x && orb.Right() <= fl.Right() ? orb.Right() : fl.x + kHudOrbShare * fl.w;
        const Rect& top = parts[kTop];
        topL = UsableHudRect(top) && top.y > fl.y && top.y < fl.Bottom() ? top.y : fl.y + kHudBarTopShare * fl.h;
        const float inner = hasXp && xp.x > orbRight && xp.x <= fl.Right() ? xp.x : fl.Right();
        add(fl.x, fl.y, orbRight, fl.Bottom());
        add(orbRight, topL, inner, fl.Bottom());
    }
    if (hasXp && xp.y >= ref->y && xp.y < ref->Bottom()) add(xp.x, xp.y, xp.Right(), xp.Bottom());
    if (hasR) {
        const Rect& orb = parts[kOrbR];
        const float orbLeft =
            UsableHudRect(orb) && orb.x >= fr.x && orb.x < fr.Right() ? orb.x : fr.Right() - kHudOrbShare * fr.w;
        const float topR = hasL ? topL - fl.y + fr.y : fr.y + kHudBarTopShare * fr.h;   // the frames mirror each other
        const float inner = hasXp && xp.Right() >= fr.x && xp.Right() < orbLeft ? xp.Right() : fr.x;
        add(inner, topR, orbLeft, fr.Bottom());
        add(orbLeft, fr.y, fr.Right(), fr.Bottom());
    }
    return n;
}

namespace {

// The pose itself (TreePose without the tooltip); false = nothing may be drawn.
bool PoseOf(IGame& game, const TreeTable& table, TreePose& p) {
    const UiElem pe = game.UiRead(table.panel);
    p.panelVisible = pe.valid && pe.visible;
    const UiElem ce = game.UiRead(table.canvas);
    if (!pe.valid || !ce.valid) return false;
    // [0] the canvas, then the panel's occluders, then the HUD's parts.
    uint64_t addrs[1 + kMaxPanelOccluders + kHudParts] = {table.canvas};
    size_t n = 1;
    const size_t occ = std::min(table.occluders.size(), kMaxPanelOccluders);
    for (size_t i = 0; i < occ; ++i) addrs[n++] = table.occluders[i];
    int hudAt[kHudParts];
    const TreeHud& h = table.hud;
    const uint64_t parts[kHudParts] = {h.frameL, h.frameR, h.orbL, h.orbR, h.xpBar, h.barTop};
    for (size_t i = 0; i < kHudParts; ++i) {
        hudAt[i] = -1;
        if (table.hudVisible && h.container != 0 && parts[i] != 0) {
            hudAt[i] = static_cast<int>(n);
            addrs[n++] = parts[i];
        }
    }
    Rect rects[1 + kMaxPanelOccluders + kHudParts];
    game.UiRects(addrs, n, rects);
    const Rect& cr = rects[0];
    if (!cr.ok || !Finite(cr.x) || !Finite(cr.y) || !Finite(cr.w) || !Finite(ce.w) || !(ce.w > 0.0f)) return false;
    if (std::fabs(ce.w - table.canvasW) > kSizeTolerance) return false;   // not the canvas the table was built on
    const float s = cr.w / ce.w;
    if (!Finite(s) || !(s > kMinScale && s < kMaxScale)) return false;
    const float ox = cr.x + s * ce.modX, oy = cr.y + s * ce.modY;
    if (!Finite(ox) || !Finite(oy) || std::fabs(ox) > kMaxCoord || std::fabs(oy) > kMaxCoord) return false;
    p.canvasRect = cr;
    p.scale = s;
    p.originX = ox;
    p.originY = oy;
    for (size_t i = 0; i < occ; ++i) {
        p.occluders[i] = rects[1 + i];
        if (!Finite(p.occluders[i].x) || !Finite(p.occluders[i].y) || !Finite(p.occluders[i].w) || !Finite(p.occluders[i].h))
            p.occluders[i].ok = false;
    }
    Rect partRects[kHudParts];
    for (size_t i = 0; i < kHudParts; ++i)
        if (hudAt[i] >= 0) partRects[i] = rects[static_cast<size_t>(hudAt[i])];
    Rect hud[kMaxHudOccluders];
    const int hudCount = HudOccluderRects(partRects, hud);
    for (int i = 0; i < hudCount; ++i) p.occluders[occ + static_cast<size_t>(i)] = hud[i];
    p.occluderCount = static_cast<int>(occ) + hudCount;
    p.hudOccluders = hudCount;
    return true;
}

// The visible node whose rectangle contains `mouse` (the smallest when several do); nullptr = none.
const TreeNodeUi* NodeAt(const TreeTable& table, const TreePose& p, Vec2 mouse) {
    const TreeNodeUi* best = nullptr;
    float bestArea = 0.0f;
    for (const TreeNodeUi& n : table.nodes) {
        if (!n.visible) continue;
        const float x = p.originX + p.scale * n.localX, y = p.originY + p.scale * n.localY;
        const float w = p.scale * n.w, h = p.scale * n.h;
        if (mouse.x < x || mouse.y < y || mouse.x >= x + w || mouse.y >= y + h) continue;
        if (best == nullptr || w * h < bestArea) {
            best = &n;
            bestArea = w * h;
        }
    }
    return best;
}

} // namespace

TreePose ReadTreePose(IGame& game, const TreeTable& table) {
    TreePose p;
    try {
        if (!table.found || table.canvas == 0 || table.panel == 0) return p;
        if (!PoseOf(game, table, p)) {
            const bool visible = p.panelVisible;
            p = TreePose{};
            p.panelVisible = visible;
        } else {
            p.ok = true;
        }
        GameHover hover;
        p.tooltip = ReadGameTooltip(game, table.tooltipSource, &hover);
        if (!p.tooltip.ok && hover.ok && p.ok && p.panelVisible) {
            const TreeNodeUi* node = NodeAt(table, p, hover.mouse);
            if (node != nullptr) p.tooltip = ReadTooltipOf(game, table.tooltipSource, node->element, hover.mouse);
        }
    } catch (...) {
        p = TreePose{};
    }
    return p;
}

} // namespace bp
