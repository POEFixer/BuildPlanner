#pragma once
// BuildPlanner - the passive tree data
// TreeData.h - the PoE2 passive tree as a graph: Path of Building (PoE2)'s tree.json -> PassiveTree.
// The plugin ships a stripped copy as data/tree.json
// (tools/strip_tree.py); the loader reads an unstripped PoB tree.json too ("Update tree data").
// Pure: no SDK, no ImGui.
//
// What tree data is used for: node kinds, names, stats, the graph (adjacency), classes and
// ascendancies. Node POSITIONS ON SCREEN come from the live UI (TreeReader.h), never from here:
// Path of Building relocates the ascendancy areas, so Node::x / y of an ascendancy node are not
// the game's. x / y are kept for the main tree (they equal the game's group coordinates) and for
// tests.
//
// Threads: a PassiveTree is immutable after ParseTree. The App loads it on the job thread and
// publishes it as shared_ptr<const PassiveTree>; any thread may read it.
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Types.h"

namespace bp {

enum NodeFlag : uint32_t {
    kNodeNotable = 1u << 0,
    kNodeKeystone = 1u << 1,
    kNodeJewel = 1u << 2,                 // "isJewelSocket"
    kNodeClassStart = 1u << 3,            // has "classesStart": the class art, not an allocatable node
    kNodeAscStart = 1u << 4,              // "isAscendancyStart"
    kNodeOnlyImage = 1u << 5,             // "isOnlyImage": decorative mastery art; does not exist for planning
    kNodeAttribute = 1u << 6,             // "isAttribute": "+5 to any Attribute" (Node::options)
    kNodeMultipleChoice = 1u << 7,        // "isMultipleChoice": the parent of choice options
    kNodeMultipleChoiceOption = 1u << 8,  // "isMultipleChoiceOption": needs its parent (Node::choiceParent)
    kNodeFreeAllocate = 1u << 9,          // "isFreeAllocate": costs no point
    kNodeSwitchable = 1u << 10,           // "isSwitchable": another class or ascendancy sees another passive
                                          // in this place (Node::variants)
    kNodeConstrained = 1u << 11           // has "unlockConstraint" (Node::unlockNodes)
};

// One option of an attribute node, in PoB's override order (index 0 Str, 1 Dex, 2 Int).
struct AttrOption {
    uint16_t id = 0;        // the option's passive graph id (26297 / 14927 / 57022 in 0_5): what the
                            // game stores as the chosen option. NOT a tree node.
    Attr attr = Attr::None;
};

// What a kNodeSwitchable node is for one class or one ascendancy (the tree data's "options" of an
// "isSwitchable" node). In 0_5: 59 nodes are keyed by a CLASS name ("Witch", "Druid", "Huntress": the
// class that shares its start with another one sees its own passive there, with its own id, name and
// stats) and 19 by an ASCENDANCY name ("Abyssal Lich" reuses the Lich nodes; only the art differs).
struct NodeVariant {
    std::string key;                  // a ClassInfo::name or an AscendancyInfo::name (display name)
    uint16_t id = 0;                  // the variant's own passive graph id, 0 = it has none (the base id is
                                      // used). NOT a tree node: PassiveTree::aliasOf maps it to the base node
    std::string name;                 // "" = the base node's name
    std::vector<std::string> stats;   // used when hasStats (may be an empty list)
    bool hasStats = false;            // false = the base node's stats
};

// Node::drawnOrbit value of a link the tree data marks as a plain straight line (orbit 2147483647).
inline constexpr int8_t kLinkStraight = 127;

struct Node {
    uint16_t id = 0;                  // "skill" = PassiveSkillGraphId: the id PoB and the game use
    std::string stringId;             // PassiveSkills.dat Id ("witch_sorceress_notable2"): the .build format's id
    std::string name;
    uint32_t flags = 0;               // NodeFlag
    uint32_t group = kNoIndex;        // index into PassiveTree::groups
    int orbit = 0;
    int orbitIndex = 0;
    float x = 0.0f, y = 0.0f;         // centre in tree-data units (see the header comment)
    float angle = 0.0f;               // radians on its orbit, 0 = up, clockwise (x = sin, y = -cos)
    float orbitRadius = 0.0f;         // tree units; 0 on orbit 0
    // The ascendancy the node belongs to, as the tree data writes it ("ascendancyName": the DISPLAY
    // name, e.g. "Disciple of Varashta"); "" on the main tree.
    std::string ascendancy;
    std::vector<NodeVariant> variants;       // kNodeSwitchable only, sorted by key
    std::vector<std::string> classesStart;   // kNodeClassStart: the class names that start here
    // Adjacent node INDICES, both directions, deduplicated, sorted. Everything the game lets a path
    // cross is here: also links between the main tree and an ascendancy and links from class starts.
    // kNodeOnlyImage nodes are never in it.
    std::vector<uint32_t> links;
    // The subset of `links` that is DRAWN as a line (both ends on the main tree or in one ascendancy,
    // no class start). Sorted. The overlay draws lines only along these.
    std::vector<uint32_t> drawnLinks;
    // Parallel to drawnLinks: how the game draws that line (the tree data's connection "orbit", read
    // the way Path of Building's PassiveTree.lua BuildConnector reads it):
    //   0              no orbit given: an arc along the shared orbit when both ends are in the same
    //                  group on the same orbit (> 0), else a straight line;
    //   +-1 .. +-9     an arc of radius orbitRadii[|v|] through both ends. The sign picks the side of
    //                  the chord the centre is on, for the direction THIS node -> the linked node
    //                  (the linked node's own entry holds the negated value, so both give one centre):
    //                    d = |p2 - p1|, r = the radius; when d >= 2 r the line is straight; else
    //                    h = sqrt(r * r - d * d / 4) * sign(v)
    //                    centre = p1 + (p2 - p1) / 2 + h * ((p2.y - p1.y) / d, -(p2.x - p1.x) / d)
    //                  (y grows downwards, in tree units and on screen alike);
    //   kLinkStraight  the data says 2147483647: always a straight line, also on a shared orbit.
    // 1878 of the 5846 links of 0_5 carry a non-zero value (143 of them 2147483647): the "same group,
    // same orbit" rule alone draws a third of the tree wrong.
    std::vector<int8_t> drawnOrbit;
    AttrOption options[3];            // kNodeAttribute only
    uint32_t choiceParent = kNoIndex; // kNodeMultipleChoiceOption: the kNodeMultipleChoice node it hangs off
    std::vector<uint32_t> unlockNodes;       // kNodeConstrained: every one must be allocated first
    std::vector<std::string> stats;   // notables, keystones, jewel sockets, ascendancy nodes (the strip
                                      // tool drops the stats of plain small nodes)

    bool Is(uint32_t f) const { return (flags & f) != 0; }
    bool IsAscendancy() const { return !ascendancy.empty(); }
    // Does allocating it spend a point? Not for "isFreeAllocate" nodes and not for the options of a
    // multiple-choice node (Path of Building's CountAllocNodes), nor for the start nodes and the art,
    // which are never allocated at all.
    bool CostsPoint() const {
        return !Is(kNodeFreeAllocate | kNodeMultipleChoiceOption | kNodeClassStart | kNodeAscStart | kNodeOnlyImage);
    }
    // The variant a character sees: the one keyed by its class name, else the one keyed by its
    // ascendancy's display name (Path of Building's order); nullptr = the base node as it is.
    const NodeVariant* VariantFor(std::string_view className, std::string_view ascendancyName) const {
        for (const NodeVariant& v : variants)
            if (!className.empty() && v.key == className) return &v;
        for (const NodeVariant& v : variants)
            if (!ascendancyName.empty() && v.key == ascendancyName) return &v;
        return nullptr;
    }
    // The name and the stats to show to that character (tooltips, the Passives tab): the variant's
    // where it has its own, else the base node's.
    const std::string& NameFor(std::string_view className, std::string_view ascendancyName) const {
        const NodeVariant* v = VariantFor(className, ascendancyName);
        return (v && !v->name.empty()) ? v->name : name;
    }
    const std::vector<std::string>& StatsFor(std::string_view className, std::string_view ascendancyName) const {
        const NodeVariant* v = VariantFor(className, ascendancyName);
        return (v && v->hasStats) ? v->stats : stats;
    }
    // The game has a node element for it that can be allocated (not art, not a class start).
    bool Allocatable() const { return !Is(kNodeOnlyImage | kNodeClassStart); }
    // A node the planner routes to: notable, keystone or jewel socket.
    bool IsObjective() const { return Is(kNodeNotable | kNodeKeystone | kNodeJewel); }
    // May be allocated with weapon set points: PoB forces ordinary points on ascendancy nodes,
    // keystones and jewel sockets.
    bool WeaponSetEligible() const { return !IsAscendancy() && !Is(kNodeKeystone | kNodeJewel | kNodeClassStart); }
};

struct Group {
    float x = 0.0f, y = 0.0f;
    std::vector<uint32_t> nodes;      // node indices
};

struct AscendancyInfo {
    std::string name;                 // display name ("Abyssal Lich")
    std::string internalId;           // "Witch3b": what a BuildSpec::ascendancyId holds
    // The Node::ascendancy value its nodes carry: the name itself, except for an ascendancy that
    // reuses another one's switchable nodes ("Abyssal Lich" -> "Lich"). Two ascendancies with one
    // nodesName cannot be told apart by a character's allocated nodes: whenever one side comes from
    // the game, compare ascendancies by nodesName, never by internalId.
    std::string nodesName;
};

struct ClassInfo {
    std::string name;                 // "Sorceress"
    int integerId = -1;               // what a BuildSpec::classId holds
    std::vector<AscendancyInfo> ascendancies;   // tree-data order (index + 1 = PoB's legacy ascendClassId)
};

struct PassiveTree {
    // bp_meta of the stripped file. An unstripped PoB tree.json names no version at all: source and
    // commit are then "" and treeVersion is the `versionHint` given to ParseTree / LoadTreeFile.
    std::string source, treeVersion, commit;
    std::vector<Node> nodes;
    std::vector<Group> groups;
    std::vector<ClassInfo> classes;
    std::vector<int> orbitRadii;
    std::vector<int> skillsPerOrbit;
    std::vector<uint16_t> jewelSlots;         // the tree data's "jewelSlots" order
    std::unordered_map<uint16_t, uint32_t> indexById;   // Node::id -> node index (base ids only)
    // NodeVariant::id -> the index of the node it is a variant of (72 ids in 0_5; none of them is a
    // Node::id). Path of Building lists the BASE id of such a passive; what the game lists for a
    // Witch, Druid or Huntress is not captured yet. So every id that comes from outside (a build, the
    // character's list, a node element) goes through IndexOf / BaseId, which accept both.
    std::unordered_map<uint16_t, uint32_t> aliasOf;
    // AttrOption::id -> its attribute (filled by ParseTree; AttrOfOption falls back to a scan of the
    // attribute nodes when it is empty, e.g. for a tree built by hand).
    std::unordered_map<uint16_t, Attr> attrOfOption;

    bool Empty() const { return nodes.empty(); }
    // The node of a base id OR of a variant id; kNoIndex / nullptr when unknown.
    uint32_t IndexOf(uint16_t id) const;
    const Node* Find(uint16_t id) const;
    // The base id (Node::id) of a base or variant id; the id itself when the tree does not know it.
    // CharState::allocated, TreeNodeUi::id and everything the planner compares hold base ids.
    uint16_t BaseId(uint16_t id) const;

    const ClassInfo* ClassById(int integerId) const;             // nullptr when unknown
    const ClassInfo* ClassByName(std::string_view name) const;   // ASCII case-insensitive
    // The ascendancy with this internalId ("Sorceress3"); nullptr when unknown or "". *owner (optional)
    // gets its class.
    const AscendancyInfo* AscendancyById(std::string_view internalId, const ClassInfo** owner = nullptr) const;
    // The ascendancy whose nodes carry this Node::ascendancy value and, of those, the first in class
    // order (for "Lich": Lich, not Abyssal Lich). nullptr when unknown.
    const AscendancyInfo* AscendancyByNodesName(std::string_view nodesName, const ClassInfo** owner = nullptr) const;

    // The class start node of a class name ("Sorceress"); a start node is shared by two classes.
    // kNoIndex when unknown.
    uint32_t ClassStartNode(std::string_view className) const;
    // The start node of the ascendancy whose nodes carry `nodesName`. kNoIndex when unknown.
    uint32_t AscendancyStartNode(std::string_view nodesName) const;
    // Which attribute an option id of an attribute node means; Attr::None when it is no option id.
    Attr AttrOfOption(uint16_t optionId) const;
    // Ids that Path of Building lists in a tree but the game never lists as allocated: every class
    // start and ascendancy start node. Sorted ascending.
    std::vector<uint16_t> StartNodeIds() const;
    // How many nodes the game has a node element for when `nodesName` is the character's ascendancy
    // ("" = none): every Allocatable main-tree node plus that ascendancy's nodes. TreeReader's
    // coverage test divides by it.
    int AllocatableCount(std::string_view nodesName) const;
};

// Parses a PoB-PoE2 tree.json, stripped or not. On failure returns false, leaves `out` empty and
// names the problem in `error` (English, may be null). Requirements: at least 1000 nodes and 6
// classes, every node's group known. `versionHint` ("0_5") becomes PassiveTree::treeVersion when the
// text has no bp_meta.treeVersion of its own (an unstripped PoB file names no version): the update
// job knows the version folder it downloaded from and the App keeps it in
// Settings::treeDownloadedVersion. Never throws.
bool ParseTree(std::string_view json, PassiveTree& out, std::string* error, std::string_view versionHint = {});

// Reads a file (at most 64 MiB) and parses it. Never throws.
bool LoadTreeFile(const std::filesystem::path& path, PassiveTree& out, std::string* error,
                  std::string_view versionHint = {});

// ---- "Update tree data" (the HTTP itself runs in the App's job) ----

// The hosts the update may contact.
inline constexpr std::string_view kTreeUpdateHosts[] = {"api.github.com", "raw.githubusercontent.com"};
inline constexpr size_t kTreeDownloadMaxBytes = 32u << 20;
inline constexpr uint32_t kTreeDownloadTimeoutMs = 60000;

// The GitHub API listing of Path of Building (PoE2)'s src/TreeData folder (JSON array of entries).
std::string TreeListingUrl();
// The newest version folder in that listing ("0_5": compared numerically part by part; folders that
// are not <digits>_<digits> are ignored). "" when the listing holds none or is not JSON.
std::string NewestTreeVersion(std::string_view listingJson);
// The raw tree.json of a version folder. "" when `version` is not <digits>_<digits> (nothing else
// may end up in the URL path).
std::string TreeFileUrl(std::string_view version);
// Compares two version strings ("0_4" < "0_5" < "1_0"): -1, 0, 1. An unparsable one is the smaller.
int CompareTreeVersions(std::string_view a, std::string_view b);

} // namespace bp
