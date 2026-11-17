#pragma once
// BuildPlanner - the passive planner
// Planner.h - compares the character's passives with the build's tree and decides the ORDER in which
// the missing points should be taken. Pure and deterministic: the same input gives the
// same plan, bit for bit. No SDK, no ImGui. The algorithm is POE1 PassiveTreePlanner's
// with the PoE2 changes below.
//
// States, per tree node:
//   Ok       allocated and in the build, the same way               (optional dim gold ring)
//   Missing  in the build, not allocated                            (green, numbered in order)
//   Wrong    allocated, not in the build                            (red: refund it)
//   Adjust   allocated and in the build, but with another weapon set or another attribute than
//            the build (orange); Plan::adjustBits says which
// Never given a state: kNodeOnlyImage nodes, class start nodes, unknown ids. The ascendancy START
// node is always Ok and never a step (the game does not list it as allocated).
//
// Order (main tree; the ascendancy is ordered the same way on its own, from its start node):
//   1. reached R = the character's allocated main-tree nodes + the root class start node: the class
//      start that has an allocated neighbour (PlanInput::className's own first), else the start of
//      PlanInput::className. The character's allocated ascendancy nodes are walked from too: a
//      Pathfinder's "Path of the X" option links into the main tree next to another class's start.
//   2. objectives = missing notables, keystones and jewel sockets (in the ascendancy: notables).
//   3. repeat: among objectives reachable from R by walking only through MISSING nodes, pick the one
//      with the lowest (tier, cost, travel, node id): tier = index of the first EARLIER tree of the
//      build that contains it (0 when tiers are off or none does), cost = missing nodes on its
//      shortest path including itself, travel = how many of those are in NO earlier tree. Append
//      that path in walking order, add it to R.
//   4. then the missing non-objective nodes still reachable, breadth-first from R (ties by node id).
//   5. the rest cannot be reached through missing nodes alone: appended in node-id order with
//      reachable = false.
// PoE2 rules on top: a kNodeMultipleChoiceOption node is reachable only once its choiceParent is
// in R (or an earlier step); a kNodeConstrained node only once every one of its unlockNodes is. For
// these gates every allocated node counts, whatever its part of the tree: an Oracle's hidden main-tree
// paths wait for an ascendancy notable, and stay unreachable (rule 5) until the character has it. A
// step of rule 4 that opens a gate sends the order back to rule 3.
// Nodes that cost no point (Node::CostsPoint() false: kNodeFreeAllocate nodes and the options of a
// multiple-choice node, as in Path of Building's CountAllocNodes) are steps like any other, but they
// are not counted as points: the ordering key `cost` of rule 3 and ObjectiveRow::cost count only
// nodes that cost a point, ObjectiveRow::freeSteps and Plan::freeSteps count the others. A
// Pathfinder's or a Gemling Legionnaire's chosen option therefore never makes the plan ask for more
// ascendancy points than the game gives.
// Weapon sets: the ordinary tree, Weapon Set I and Weapon Set II are separate pathing graphs (Path of
// Building's CanPathThroughAllocMode). Every walk of rules 3 and 4 enters a node only from an ordinary
// node or one of the node's own set (the character's set where allocated, Plan::wantSet where
// missing): a Set II node is never taken through a Set I node, an ordinary node never through a
// weapon-set node. A node without such a connector is left to rule 5.
// Every reachable step is adjacent to R or an earlier step, through a link its weapon set may use: a
// valid allocation sequence. Steps are never duplicated.
//
// Ids: every id of PlanInput (the target's nodes, the earlier trees, the character's list) is taken
// through PassiveTree::IndexOf, so a variant id of a kNodeSwitchable node (TreeData.h) and its base
// id are the same node. Two ids that resolve to one node count once.
//
// Threads: stateless; the App calls BuildPlan on the render thread (it takes well under a
// millisecond; a timing test pins that).
#include <cstdint>
#include <string>
#include <vector>

#include "PlanSummary.h"
#include "TreeData.h"
#include "Types.h"

namespace bp {

enum class NodeState : uint8_t { None = 0, Ok, Missing, Wrong, Adjust };

// Why a node is Adjust (bits of Plan::adjustBits).
enum AdjustBit : uint8_t {
    kAdjustWeaponSet = 1u << 0,   // allocated with another weapon set than the build wants
    kAdjustAttribute = 1u << 1    // an attribute node whose chosen attribute differs from the build's
};

// One allocated passive of the character (CharReader publishes the same shape).
struct AllocatedNode {
    uint16_t id = 0;                 // a base id (PassiveTree::BaseId) when it comes from CharReader
    WeaponSet set = WeaponSet::None;
    bool operator==(const AllocatedNode&) const = default;
};

// The attribute chosen on an allocated attribute node.
struct ChosenAttr {
    uint16_t node = 0;
    Attr attr = Attr::None;
    bool operator==(const ChosenAttr&) const = default;
};

struct PlanInput {
    const PassiveTree* tree = nullptr;
    const BuildSpec* target = nullptr;                  // the stage's tree (nodes, weapon sets, attributes)
    std::vector<const BuildSpec*> earlierSpecs;         // tiers 1..n: the trees before the target, PoB order
                                                        // (empty when tiers are off)
    std::vector<AllocatedNode> allocated;               // from the game, sorted by id; empty = nothing known
    std::vector<ChosenAttr> allocatedAttrs;             // from the game, sorted by node
    // The class whose start anchors the main tree when no start node has an allocated neighbour:
    // the character's class when known, else the build's ("Sorceress"). "" = take the build's.
    std::string className;
    // The ascendancy to order, as the Node::ascendancy value its nodes carry
    // (AscendancyInfo::nodesName): the character's when known, else the build's. "" = none.
    std::string ascendancy;
    bool useTiers = true;
};

struct PlanStep {
    uint32_t node = kNoIndex;        // tree node index
    uint32_t objective = kNoIndex;   // the objective this step walks to (itself for an objective); kNoIndex none
    int tier = 0;
    bool reachable = true;
};

struct PlanEdge {
    uint32_t a = kNoIndex, b = kNoIndex;   // a < b, a drawn link (Node::drawnLinks) between two nodes with a state
    NodeState state = NodeState::None;     // Wrong if either end is Wrong, else Missing if either end is
                                           // Missing, else Adjust if either is Adjust, else Ok
    // The line's shape, from Node::drawnOrbit of the link a -> b (TreeData.h has the rule and the
    // centre formula):
    //   arcOrbit != 0   an arc of radius orbitRadii[|arcOrbit|] through both ends; the sign is for the
    //                   direction a -> b. The overlay computes the centre from the two SCREEN centres
    //                   with r = TreePose::scale * orbitRadii[|arcOrbit|] (the canvas' units are the
    //                   tree data's), and draws a straight line when the ends are 2 r or more apart.
    //   else groupArc   both ends in one group on one orbit (> 0) and the data gives no orbit: an arc
    //                   around the group's centre (radius TreePose::scale * Node::orbitRadius; the centre
    //                   is either end's screen centre minus r * (sin angle, -cos angle)).
    //   else            a straight line (also for a link the data marks kLinkStraight).
    int8_t arcOrbit = 0;                   // -9..9
    bool groupArc = false;
};

struct Plan {
    // Per tree node index (each vector has tree->nodes.size() entries; empty = no plan):
    std::vector<NodeState> state;
    std::vector<uint8_t> adjustBits;       // AdjustBit set, non-zero only where state == Adjust
    std::vector<WeaponSet> wantSet;        // how the build allocates it (None = ordinary points or not in the build)
    std::vector<Attr> wantAttr;            // the attribute the build picks on it (None = does not say / not an attribute node)
    std::vector<WeaponSet> haveSet;        // how the character has it
    std::vector<Attr> haveAttr;            // the attribute the character chose
    std::vector<int32_t> stepOf;           // index into steps (main tree) or ascSteps (ascendancy); -1 none

    std::vector<PlanStep> steps;           // main tree, in order
    std::vector<PlanStep> ascSteps;        // the ascendancy, in order
    std::vector<PlanEdge> edges;

    int ok = 0, missing = 0, wrong = 0, adjust = 0;       // main tree (adjust: nodes in state Adjust)
    int ascOk = 0, ascMissing = 0, ascWrong = 0;          // the ordered ascendancy
    int unreachable = 0;                   // main-tree steps with reachable == false
    int freeSteps = 0;                     // missing nodes that cost no point (!Node::CostsPoint()), main
                                           // tree and ascendancy together
    int pointsNeeded = 0;                  // missing main-tree nodes that cost a point (missing - its free steps)
    int ascPointsNeeded = 0;               // missing ascendancy nodes that cost a point
    std::vector<uint16_t> unknownTarget;   // build ids the tree data does not know (older / newer tree?)
    std::vector<uint16_t> unknownAllocated;
    uint32_t rootNode = kNoIndex;          // the class start used as the root
    uint32_t ascStartNode = kNoIndex;      // the start node of the ordered ascendancy

    bool Valid() const { return !state.empty(); }
};

// Never throws: bad input (null tree, null target) gives an empty Plan.
Plan BuildPlan(const PlanInput& in);

// Step numbers and objective rows derived from a plan (the overlay's numbers, the Passives tab's
// table).
struct PlanNumbers {
    // Per tree node index: its 1-based step number among the reachable steps of its own list (main
    // tree, or the ascendancy); 0 = no number.
    std::vector<int32_t> number;
    std::vector<ObjectiveRow> objectives;      // main tree, in order: consecutive steps with one objective form a row
    std::vector<ObjectiveRow> ascObjectives;   // the ascendancy, in order
};
PlanNumbers Annotate(const PassiveTree& tree, const Plan& plan);

} // namespace bp
