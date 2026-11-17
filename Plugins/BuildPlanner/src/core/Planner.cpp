// BuildPlanner - the passive planner
// Planner.cpp - see Planner.h. The ordering is POE1 PassiveTreePlanner's without
// masteries, with the PoE2 rules of the header.
//
// Decisions the header leaves open (kept here so the reviewer of a plan does not have to guess):
//  - Reached ground of the MAIN tree: the character's allocated main-tree nodes, the root class start
//    AND the character's allocated ascendancy nodes. A Pathfinder's "Path of the Sorceress" option
//    links into the main tree next to the Sorceress start (the game then lets her allocate from
//    there); no other ascendancy node links into the main tree (only ascendancy starts link to class
//    starts, and the game never lists those as allocated).
//  - The root: the start of PlanInput::className when it has an allocated neighbour; else the first
//    class start (by id) with an allocated neighbour; else the start of className. Only one root: a
//    node next to another class's start does not open that start's other neighbours.
//  - Gates (multiple-choice options, unlockConstraint nodes) count as open when the node they wait for
//    is reached: allocated (in ANY part of the tree: an Oracle's hidden main-tree paths wait for her
//    ascendancy notable "The Unseen Path"), the root, or an earlier step of the list being ordered. A
//    gate is judged when a walk is planned; a node behind a gate that a later step of the same round
//    opens is found by the next round (rules 3 and 4 run again while rule 4 took a step). A main-tree
//    node waiting for an ascendancy node the character has not allocated is unreachable (rule 5): the
//    game hides it until then.
//  - Weapon sets: Path of Building keeps the ordinary tree, Weapon Set I and Weapon Set
//    II as separate pathing graphs (PassiveSpec.lua CanPathThroughAllocMode: "normal allocation can
//    only use normal nodes, weapon set allocation can also use its own set"). Every walk follows it: a
//    node is entered from an ordinary node (the root, an ordinary allocated node or step) or from a node
//    of its own set; a node of a set is allocated / wanted with that set (the character's set where
//    allocated, the build's where missing). A node with no such connector falls to rule 5.
//  - Shortest walks: a multi-source BFS from R through missing nodes, counted in nodes. Among the
//    shortest walks to a node: fewer point-costing nodes, then lower travel, then the smaller parent id,
//    so the chosen walk does not depend on container order. `cost` of rule 3 = the point-costing nodes
//    on that walk (a free node or a choice option counts 0).
//  - travel counts the earlier trees whether or not tiers are on (it only breaks ties between costs).
//  - Adjust is decided on the main tree only: ascendancy nodes, keystones and jewel sockets are never
//    allocated with weapon set points, and attribute nodes are main-tree nodes. Plan::adjust counts it.
//  - An ascendancy start gets state Ok when it is in the target or the character's list, or when any
//    node of its ascendancy has a state (so the lines from it get a colour). It is never a step and is
//    never counted.
//  - Counters of an ascendancy that is NOT the ordered one (a build of another ascendancy): its nodes
//    get states (they are drawn) but no counter and no step.
//  - On an internal failure (out of memory) BuildPlan returns an empty Plan (state.size() == 0).
#include "Planner.h"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <tuple>
#include <unordered_set>

namespace bp {
namespace {

// Scratch shared by every walk of one BuildPlan call: sized once, reused per BFS (epoch stamps, so no
// per-round clearing of node-sized buffers).
struct Walk {
    const PassiveTree& t;
    size_t n = 0;
    std::vector<uint8_t> inR;        // reached: allocated, the root, or an earlier step
    std::vector<uint8_t> pending;    // missing, in the domain being ordered, not a step yet
    std::vector<int32_t> tier;       // 1-based first earlier tree holding the node; 0 none / tiers off
    std::vector<uint8_t> fresh;      // 1 when the node is in no earlier tree (counts as travel)
    // The pathing graph a node belongs to (WeaponSet as a number): the character's set for an
    // allocated node, the build's for a pending one, 0 for the root and ordinary points. Path of
    // Building keeps the ordinary tree, Weapon Set I and Weapon Set II as separate pathing graphs
    // (PassiveSpec.lua CanPathThroughAllocMode): a node is entered only from an ordinary node or one
    // of its own set, so an ordinary node never through a weapon-set node.
    std::vector<uint8_t> pathSet;
    std::vector<uint32_t> stamp;     // == epoch: dist / pts / travel / parent are valid for this BFS
    std::vector<int32_t> dist;       // missing nodes on the shortest walk, including the node
    std::vector<int32_t> pts;        // of those, the ones that cost a point
    std::vector<int32_t> travel;     // of those, the ones in no earlier tree
    std::vector<uint32_t> parent;    // previous node on the chosen walk (kNoIndex at a source)
    std::vector<uint32_t> queue;
    std::vector<uint32_t> path;
    uint32_t epoch = 0;

    explicit Walk(const PassiveTree& tree) : t(tree), n(tree.nodes.size()) {
        inR.assign(n, 0);
        pending.assign(n, 0);
        tier.assign(n, 0);
        fresh.assign(n, 1);
        pathSet.assign(n, 0);
        stamp.assign(n, 0);
        dist.assign(n, 0);
        pts.assign(n, 0);
        travel.assign(n, 0);
        parent.assign(n, kNoIndex);
        queue.reserve(n);
        path.reserve(n);
    }

    uint16_t Id(uint32_t i) const { return t.nodes[i].id; }
    bool Visited(uint32_t i) const { return stamp[i] == epoch; }
    int32_t Cost(uint32_t i) const { return t.nodes[i].CostsPoint() ? 1 : 0; }

    // A multiple-choice option needs its parent, an unlockConstraint node every node it names.
    bool GateOpen(uint32_t i) const {
        const Node& node = t.nodes[i];
        if (node.Is(kNodeMultipleChoiceOption) && node.choiceParent < n && !inR[node.choiceParent]) return false;
        if (node.Is(kNodeConstrained))
            for (const uint32_t u : node.unlockNodes)
                if (u < n && !inR[u]) return false;
        return true;
    }

    void NextEpoch() {
        if (++epoch == 0) {   // never within one plan (one BFS per round), kept for safety
            std::fill(stamp.begin(), stamp.end(), 0u);
            epoch = 1;
        }
    }

    // Multi-source BFS from every node of `sources` (all in R) through pending nodes whose gate is open,
    // along the links a weapon set allows (pathSet: u -> w only when u is ordinary or of w's set; a
    // static filter, so one BFS per round still finds every reachable node). Pending objectives are
    // entered only when `enterObjectives`. Ties between equally short walks: fewer points, lower
    // travel, then the smaller parent id. FIFO order makes a node's pts and travel final before it is
    // expanded (every predecessor one level up is dequeued first).
    void Bfs(const std::vector<uint32_t>& sources, bool enterObjectives) {
        NextEpoch();
        queue.clear();
        for (const uint32_t s : sources) {
            if (s >= n || Visited(s)) continue;
            stamp[s] = epoch;
            dist[s] = 0;
            pts[s] = 0;
            travel[s] = 0;
            parent[s] = kNoIndex;
            queue.push_back(s);
        }
        for (size_t head = 0; head < queue.size(); ++head) {
            const uint32_t u = queue[head];
            const int32_t d = dist[u] + 1;
            for (const uint32_t w : t.nodes[u].links) {
                if (w >= n || !pending[w]) continue;
                if (pathSet[u] != 0 && pathSet[u] != pathSet[w]) continue;   // another weapon set's path
                if (!enterObjectives && t.nodes[w].IsObjective()) continue;
                if (!GateOpen(w)) continue;
                const int32_t pw = pts[u] + Cost(w);
                const int32_t tw = travel[u] + fresh[w];
                if (!Visited(w)) {
                    stamp[w] = epoch;
                    dist[w] = d;
                    pts[w] = pw;
                    travel[w] = tw;
                    parent[w] = u;
                    queue.push_back(w);
                } else if (dist[w] == d && std::tie(pw, tw) < std::tie(pts[w], travel[w])) {
                    pts[w] = pw;
                    travel[w] = tw;
                    parent[w] = u;
                } else if (dist[w] == d && pw == pts[w] && tw == travel[w] && parent[w] < n && Id(u) < Id(parent[w])) {
                    parent[w] = u;
                }
            }
        }
    }

    // Appends the walk from R to `v` (a pending node visited by the last BFS), R excluded, in walking
    // order.
    void AppendWalkTo(uint32_t v) {
        const size_t start = path.size();
        for (size_t guard = 0; v != kNoIndex && v < n && !inR[v] && guard < n; ++guard) {
            path.push_back(v);
            v = parent[v];
        }
        std::reverse(path.begin() + static_cast<std::ptrdiff_t>(start), path.end());
    }

    void Take(uint32_t v, std::vector<uint32_t>& rList) {
        pending[v] = 0;
        if (!inR[v]) {
            inR[v] = 1;
            rList.push_back(v);
        }
    }
};

struct Candidate {
    uint32_t node = kNoIndex;
    int32_t tierKey = INT_MAX, cost = INT_MAX, travel = INT_MAX;
    uint16_t id = 0;

    bool Better(const Candidate& o) const {
        if (o.node == kNoIndex) return true;
        return std::tie(tierKey, cost, travel, id) < std::tie(o.tierKey, o.cost, o.travel, o.id);
    }
};

PlanStep MakeStep(const Walk& w, uint32_t v, uint32_t objective, bool reachable) {
    PlanStep s;
    s.node = v;
    s.objective = objective;
    s.tier = w.tier[v];
    s.reachable = reachable;
    return s;
}

// Orders the pending nodes of one domain (the main tree or one ascendancy). `domain` lists every
// pending node of it; `rList` holds the domain's reached nodes and grows as steps are taken.
void OrderDomain(Walk& w, const std::vector<uint32_t>& domain, std::vector<uint32_t>& rList,
                 std::vector<PlanStep>& steps, bool hasGates) {
    const PassiveTree& t = w.t;
    std::vector<uint32_t> objectives;
    for (const uint32_t v : domain)
        if (t.nodes[v].IsObjective()) objectives.push_back(v);

    std::vector<std::tuple<int32_t, uint16_t, uint32_t>> rest;
    while (true) {
        // 3. the cheapest reachable objective first, repeatedly.
        while (!objectives.empty()) {
            w.Bfs(rList, true);
            Candidate best;
            for (const uint32_t o : objectives) {
                if (!w.pending[o] || !w.Visited(o)) continue;
                Candidate c;
                c.node = o;
                c.id = t.nodes[o].id;
                c.tierKey = w.tier[o] > 0 ? w.tier[o] : INT_MAX;
                c.cost = w.pts[o];
                c.travel = w.travel[o];
                if (c.Better(best)) best = c;
            }
            if (best.node == kNoIndex) break;
            w.path.clear();
            w.AppendWalkTo(best.node);
            for (const uint32_t v : w.path) {
                if (!w.pending[v]) continue;   // cannot happen (a walk runs through pending nodes only)
                steps.push_back(MakeStep(w, v, best.node, true));
                w.Take(v, rList);
            }
            const uint32_t chosen = best.node;
            objectives.erase(std::remove_if(objectives.begin(), objectives.end(),
                                            [&](uint32_t v) { return v == chosen || !w.pending[v]; }),
                             objectives.end());
        }

        // 4. the reachable non-objectives, breadth-first from R, ties by node id.
        w.Bfs(rList, false);
        rest.clear();
        for (const uint32_t v : domain)
            if (w.pending[v] && w.Visited(v)) rest.emplace_back(w.dist[v], w.Id(v), v);
        std::sort(rest.begin(), rest.end());
        for (const auto& r : rest) {
            const uint32_t v = std::get<2>(r);
            steps.push_back(MakeStep(w, v, kNoIndex, true));
            w.Take(v, rList);
        }
        // A step of rule 4 may have opened a gate: run both rules again. Without gates nothing new can
        // be reached (rule 3's walks already crossed every pending node reachable from R).
        if (rest.empty() || !hasGates) break;
    }

    // 5. what is left cannot be reached through missing nodes alone.
    std::vector<std::pair<uint16_t, uint32_t>> left;
    for (const uint32_t v : domain)
        if (w.pending[v]) left.emplace_back(w.Id(v), v);
    std::sort(left.begin(), left.end());
    for (const auto& l : left) {
        const uint32_t v = l.second;
        steps.push_back(MakeStep(w, v, t.nodes[v].IsObjective() ? v : kNoIndex, false));
        w.pending[v] = 0;
    }
}

void SortUnique(std::vector<uint16_t>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

// Marks every known id of `ids` in `flags` (node-indexed) with `value`.
template <class T>
void MarkIds(const PassiveTree& t, const std::vector<uint16_t>& ids, std::vector<T>& flags, T value) {
    for (const uint16_t id : ids) {
        const uint32_t i = t.IndexOf(id);
        if (i < flags.size()) flags[i] = value;
    }
}

uint32_t ChooseRoot(const PassiveTree& t, const std::vector<uint8_t>& inA, const std::string& className) {
    const size_t n = t.nodes.size();
    const uint32_t own = t.ClassStartNode(className);
    auto hasAllocatedNeighbour = [&](uint32_t s) {
        for (const uint32_t l : t.nodes[s].links)
            if (l < n && inA[l] && !t.nodes[l].IsAscendancy()) return true;
        return false;
    };
    if (own < n && hasAllocatedNeighbour(own)) return own;
    for (uint32_t i = 0; i < n; ++i)
        if (t.nodes[i].Is(kNodeClassStart) && hasAllocatedNeighbour(i)) return i;
    return own;
}

Plan BuildPlanImpl(const PlanInput& in) {
    Plan p;
    if (!in.tree || !in.target || in.tree->nodes.empty()) return p;
    const PassiveTree& t = *in.tree;
    const BuildSpec& target = *in.target;
    const size_t n = t.nodes.size();

    p.state.assign(n, NodeState::None);
    p.adjustBits.assign(n, 0);
    p.wantSet.assign(n, WeaponSet::None);
    p.wantAttr.assign(n, Attr::None);
    p.haveSet.assign(n, WeaponSet::None);
    p.haveAttr.assign(n, Attr::None);
    p.stepOf.assign(n, -1);

    // Membership.
    std::vector<uint8_t> inT(n, 0), inA(n, 0);
    for (const uint16_t id : target.nodes) {
        const uint32_t i = t.IndexOf(id);
        if (i >= n) p.unknownTarget.push_back(id);
        else inT[i] = 1;
    }
    for (const AllocatedNode& a : in.allocated) {
        const uint32_t i = t.IndexOf(a.id);
        if (i >= n) {
            p.unknownAllocated.push_back(a.id);
            continue;
        }
        inA[i] = 1;
        if (a.set != WeaponSet::None) p.haveSet[i] = a.set;
    }
    SortUnique(p.unknownTarget);
    SortUnique(p.unknownAllocated);
    MarkIds(t, target.weaponSet1, p.wantSet, WeaponSet::One);
    MarkIds(t, target.weaponSet2, p.wantSet, WeaponSet::Two);
    MarkIds(t, target.attrStr, p.wantAttr, Attr::Str);
    MarkIds(t, target.attrDex, p.wantAttr, Attr::Dex);
    MarkIds(t, target.attrInt, p.wantAttr, Attr::Int);
    for (const ChosenAttr& c : in.allocatedAttrs) {
        const uint32_t i = t.IndexOf(c.node);
        if (i < n) p.haveAttr[i] = c.attr;
    }
    for (uint32_t i = 0; i < n; ++i) {
        if (!inT[i]) {   // "how the build allocates it": nothing for a node outside the build
            p.wantSet[i] = WeaponSet::None;
            p.wantAttr[i] = Attr::None;
        }
        if (!inA[i]) {
            p.haveSet[i] = WeaponSet::None;
            p.haveAttr[i] = Attr::None;
        }
        if (!t.nodes[i].Is(kNodeAttribute)) {
            p.wantAttr[i] = Attr::None;
            p.haveAttr[i] = Attr::None;
        }
    }

    // States and counters.
    std::unordered_set<std::string> ascWithState;
    for (uint32_t i = 0; i < n; ++i) {
        if (!inT[i] && !inA[i]) continue;
        const Node& node = t.nodes[i];
        if (!node.Allocatable() || node.Is(kNodeAscStart)) continue;   // starts: below
        const bool asc = node.IsAscendancy();
        const bool ordered = asc && !in.ascendancy.empty() && node.ascendancy == in.ascendancy;
        if (asc) ascWithState.insert(node.ascendancy);
        if (inT[i] && inA[i]) {
            uint8_t bits = 0;
            if (!asc) {
                if (p.wantSet[i] != p.haveSet[i]) bits |= kAdjustWeaponSet;
                if (p.wantAttr[i] != Attr::None && p.haveAttr[i] != Attr::None && p.wantAttr[i] != p.haveAttr[i])
                    bits |= kAdjustAttribute;
            }
            p.adjustBits[i] = bits;
            p.state[i] = bits ? NodeState::Adjust : NodeState::Ok;
            if (!asc) ++(bits ? p.adjust : p.ok);
            else if (ordered) ++p.ascOk;
        } else if (inT[i]) {
            p.state[i] = NodeState::Missing;
            if (!asc) ++p.missing;
            else if (ordered) ++p.ascMissing;
        } else {
            p.state[i] = NodeState::Wrong;
            if (!asc) ++p.wrong;
            else if (ordered) ++p.ascWrong;
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        const Node& node = t.nodes[i];
        if (node.Is(kNodeAscStart) && (inT[i] || inA[i] || ascWithState.count(node.ascendancy)))
            p.state[i] = NodeState::Ok;
    }

    // Edges along the drawn lines.
    for (uint32_t a = 0; a < n; ++a) {
        if (p.state[a] == NodeState::None) continue;
        const Node& na = t.nodes[a];
        for (size_t k = 0; k < na.drawnLinks.size(); ++k) {
            const uint32_t b = na.drawnLinks[k];
            if (b <= a || b >= n || p.state[b] == NodeState::None) continue;
            const Node& nb = t.nodes[b];
            const NodeState sa = p.state[a], sb = p.state[b];
            PlanEdge e;
            e.a = a;
            e.b = b;
            if (sa == NodeState::Wrong || sb == NodeState::Wrong) e.state = NodeState::Wrong;
            else if (sa == NodeState::Missing || sb == NodeState::Missing) e.state = NodeState::Missing;
            else if (sa == NodeState::Adjust || sb == NodeState::Adjust) e.state = NodeState::Adjust;
            else e.state = NodeState::Ok;
            const int8_t code = k < na.drawnOrbit.size() ? na.drawnOrbit[k] : 0;
            if (code != 0 && code != kLinkStraight) e.arcOrbit = code;
            e.groupArc = code == 0 && na.group != kNoIndex && na.group == nb.group && na.orbit == nb.orbit && na.orbit > 0;
            p.edges.push_back(e);
        }
    }

    // Tiers and travel.
    Walk w(t);
    for (size_t s = 0; s < in.earlierSpecs.size(); ++s) {
        if (!in.earlierSpecs[s]) continue;
        const int tierNo = s < static_cast<size_t>(INT_MAX) ? static_cast<int>(s) + 1 : INT_MAX;
        for (const uint16_t id : in.earlierSpecs[s]->nodes) {
            const uint32_t i = t.IndexOf(id);
            if (i >= n) continue;
            w.fresh[i] = 0;
            if (in.useTiers && w.tier[i] == 0) w.tier[i] = tierNo;
        }
    }
    bool hasGates = false;
    for (uint32_t i = 0; i < n && !hasGates; ++i)
        hasGates = p.state[i] == NodeState::Missing && t.nodes[i].Is(kNodeMultipleChoiceOption | kNodeConstrained);

    // Every allocated node is reached ground (gates see it).
    for (uint32_t i = 0; i < n; ++i)
        if (inA[i]) w.inR[i] = 1;
    // The pathing graph of each node (Walk::pathSet): what the character has where allocated (an Adjust
    // node paths as it is allocated now), what the build wants where pending.
    for (uint32_t i = 0; i < n; ++i)
        w.pathSet[i] = static_cast<uint8_t>(inA[i] ? p.haveSet[i] : p.wantSet[i]);

    // Main tree.
    {
        std::vector<uint32_t> rList, domain;
        for (uint32_t i = 0; i < n; ++i)
            if (inA[i]) rList.push_back(i);   // ascendancy nodes too: see the top of this file
        p.rootNode = ChooseRoot(t, inA, in.className);
        if (p.rootNode < n && !w.inR[p.rootNode]) {
            w.inR[p.rootNode] = 1;
            w.pathSet[p.rootNode] = 0;   // a class start roots every pathing graph
            rList.push_back(p.rootNode);
        }
        for (uint32_t i = 0; i < n; ++i)
            if (p.state[i] == NodeState::Missing && !t.nodes[i].IsAscendancy()) {
                w.pending[i] = 1;
                domain.push_back(i);
            }
        OrderDomain(w, domain, rList, p.steps, hasGates);
        for (size_t k = 0; k < p.steps.size(); ++k) {
            p.stepOf[p.steps[k].node] = static_cast<int32_t>(k);
            if (!p.steps[k].reachable) ++p.unreachable;
        }
    }

    // The ascendancy, on its own.
    const uint32_t ascStart = in.ascendancy.empty() ? kNoIndex : t.AscendancyStartNode(in.ascendancy);
    if (ascStart < n) {
        p.ascStartNode = ascStart;
        std::vector<uint32_t> rList, domain;
        for (uint32_t i = 0; i < n; ++i)
            if (inA[i] && t.nodes[i].ascendancy == in.ascendancy) rList.push_back(i);
        w.inR[ascStart] = 1;
        w.pathSet[ascStart] = 0;
        rList.push_back(ascStart);
        for (uint32_t i = 0; i < n; ++i)
            if (p.state[i] == NodeState::Missing && t.nodes[i].ascendancy == in.ascendancy) {
                w.pending[i] = 1;
                domain.push_back(i);
            }
        OrderDomain(w, domain, rList, p.ascSteps, hasGates);
        for (size_t k = 0; k < p.ascSteps.size(); ++k) p.stepOf[p.ascSteps[k].node] = static_cast<int32_t>(k);
    }

    // Points.
    for (const PlanStep& s : p.steps) {
        if (t.nodes[s.node].CostsPoint()) ++p.pointsNeeded;
        else ++p.freeSteps;
    }
    for (const PlanStep& s : p.ascSteps) {
        if (t.nodes[s.node].CostsPoint()) ++p.ascPointsNeeded;
        else ++p.freeSteps;
    }
    return p;
}

void AnnotateList(const PassiveTree& t, const std::vector<PlanStep>& steps, std::vector<int32_t>& number,
                  std::vector<ObjectiveRow>& rows) {
    int32_t next = 0;
    for (const PlanStep& st : steps) {
        if (st.node >= t.nodes.size()) continue;
        int32_t num = 0;
        if (st.reachable) {
            num = ++next;
            number[st.node] = num;
        }
        const uint32_t obj = st.objective < t.nodes.size() ? st.objective : kNoIndex;
        if (rows.empty() || rows.back().node != obj || rows.back().reachable != st.reachable) {
            ObjectiveRow r;
            r.node = obj;
            r.tier = st.tier;
            r.reachable = st.reachable;
            r.firstNumber = num;
            rows.push_back(r);
        }
        ObjectiveRow& r = rows.back();
        if (t.nodes[st.node].CostsPoint()) ++r.cost;
        else ++r.freeSteps;
        if (st.node == obj) r.tier = st.tier;
    }
}

} // namespace

PlanIdentity ResolvePlanIdentity(int buildClassId, const std::string& buildClassName, const std::string& buildAscendancy,
                                 bool charKnown, int charClassId, const std::string& charClassName,
                                 const std::string& charAscendancy) {
    PlanIdentity r;
    r.className = buildClassName;
    r.ascendancy = buildAscendancy;
    if (!charKnown) return r;
    const bool otherClass = charClassId >= 0 && buildClassId >= 0 && charClassId != buildClassId;
    if (!charClassName.empty()) r.className = charClassName;
    if (!charAscendancy.empty()) r.ascendancy = charAscendancy;
    else if (otherClass) r.ascendancy.clear();   // the build's ascendancy is not this character's to take
    r.classDiffers = otherClass || (!charAscendancy.empty() && !buildAscendancy.empty() && charAscendancy != buildAscendancy);
    return r;
}

Plan BuildPlan(const PlanInput& in) {
    try {
        return BuildPlanImpl(in);
    } catch (...) {
        return Plan{};
    }
}

PlanNumbers Annotate(const PassiveTree& tree, const Plan& plan) {
    PlanNumbers out;
    try {
        if (!plan.Valid() || plan.state.size() != tree.nodes.size()) return out;
        out.number.assign(tree.nodes.size(), 0);
        AnnotateList(tree, plan.steps, out.number, out.objectives);
        AnnotateList(tree, plan.ascSteps, out.number, out.ascObjectives);
    } catch (...) {
        out = PlanNumbers{};
    }
    return out;
}

} // namespace bp
