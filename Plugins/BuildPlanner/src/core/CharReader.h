#pragma once
// BuildPlanner - the character's passives, points and level (reader)
// CharReader.h - the character's allocated passives, weapon sets, attribute choices, points and
// level, read from the passive allocation object behind the PlayerServerData (valid while the tree
// is closed too). The path, the fields, their checks and the bounded fallback scans, and how to find
// them again after a patch: docs/OFFSETS.md section 4.2. No SDK: everything goes through IGame.
//
// WATCHER thread only (2 Hz, 10 Hz while the tree is open; the App's watcher decides when to call).
// Publishes an immutable CharState.
//
// Validation (all of it, every read; a failure publishes valid == false with a problem code and the
// overlay of that part draws nothing): the self-pointer pair of the object, the vtable inside the
// module image, vector headers sane (count <= 400, 4-byte entries, ids strictly ascending), >= 90 %
// of the ids known to the tree data, level 1..100 and equal to the SDK's player level, quest points
// 0..60, ascendancy points 0..16, free points >= 0 (main tree and ascendancy). With the tree open the
// node state bytes (Offsets::nodeState over TreeTable::nodes) cross-check the list.
// POINTS: only nodes with Node::CostsPoint() spend one. "isFreeAllocate" nodes and the options of a
// multiple-choice node (Pathfinder, Deadeye, Acolyte of Chayula, Gemling Legionnaire) are allocated
// but cost nothing: counting them as spent makes "ascendancy allocated <= total" fail for every such
// character (BadScalars, and the tree overlay goes dark).
// IDS: every id read from the game goes through PassiveTree::BaseId, so the lists below hold BASE
// ids also when the game lists the variant id of a kNodeSwitchable node (TreeData.h); "known to the
// tree data" means PassiveTree::IndexOf finds it.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Game.h"
#include "Offsets.h"
#include "Planner.h"     // AllocatedNode, ChosenAttr
#include "TreeData.h"
#include "TreeReader.h"  // TreeTable (the open-tree cross-check)
#include "Types.h"

namespace bp {

enum class CharProblem : uint8_t {
    None = 0,
    NotInGame,
    NoTreeData,        // the plugin's tree data is not loaded (ids cannot be validated)
    NoUiRoot,
    NoPlayerData,      // no PlayerServerData pointer validates (slot and scan both failed)
    NoPassiveObject,   // the PSD holds no object with the self-pointer signature
    BadList,           // the allocated vector is malformed (a torn read, or the layout moved)
    UnknownIds,        // under 90 % of the allocated ids are in the tree data: wrong data version or wrong vector
    BadScalars,        // level / points fail their range checks (the scalar tail moved)
    LevelMismatch,     // the level read differs from the SDK's player level
    ReadFailed
};

// The character's passives as of one read. Immutable once published.
struct CharState {
    CharProblem problem = CharProblem::NotInGame;
    bool valid = false;                        // true = every field below passed validation

    std::vector<AllocatedNode> allocated;      // BASE ids, sorted by id, unique; as the game lists it:
                                               // ascendancy nodes included, class start and ascendancy start NOT
    std::vector<ChosenAttr> attributes;        // sorted by node: the attribute chosen on allocated attribute nodes
    std::vector<uint16_t> granted;             // nodes granted by items (not paid with points), sorted
    std::vector<uint16_t> jewelSockets;        // allocated jewel socket ids, sorted

    int level = 0;                             // 1..100
    int questPoints = 0;                       // passive points from quests
    int weaponSetPoints = 0;                   // weapon set points, the TOTAL per set
    int ascPointsTotal = 0;                    // max of the two ascendancy counters

    // Derived (the formulas: docs/OFFSETS.md section 4.2). Every counter here counts only nodes that
    // cost a point (Node::CostsPoint(): kNodeFreeAllocate and kNodeMultipleChoiceOption nodes cost
    // nothing):
    int mainAllocated = 0;                     // allocated main-tree nodes (any set)
    int ascAllocated = 0;                      // allocated ascendancy nodes
    int set1Count = 0, set2Count = 0;          // nodes allocated with Weapon Set I / II points
    int freePoints = 0;                        // (level - 1) + questPoints - (main nodes with ordinary points
                                               // + max(set1Count, set2Count))
    int freeSet1 = 0, freeSet2 = 0;            // weaponSetPoints - set1Count / set2Count
    int freeAscPoints = 0;                     // ascPointsTotal - ascAllocated

    // Who the character is, in the tree data's terms:
    int classId = -1;                          // ClassInfo::integerId, -1 unknown
    std::string className;                     // ClassInfo::name, "" unknown
    std::string ascendancyId;                  // AscendancyInfo::internalId, "" none / unknown. A GUESS when
                                               // two ascendancies share their nodes: an Abyssal Lich has only
                                               // Lich nodes and reads as "Witch3". Compare `ascendancy`.
    std::string ascendancy;                    // AscendancyInfo::nodesName (the Node::ascendancy of its nodes), "" none
    // Derivation: the ascendancy from the allocated ascendancy nodes (its class follows; of several
    // ascendancies with that nodesName the first in class order); without one, the class from the
    // player entity's path (PlayerInfo::path, a table in CharReader.cpp), cross-checked against the
    // class start node that has an allocated neighbour.

    // Diagnostics:
    uint64_t psd = 0, object = 0;              // the addresses used
    bool psdViaScan = false, objectViaScan = false;   // found by the fallback scan, not the configured slot
    bool uiChecked = false;                    // the open-tree cross-check ran on this read
    int uiDiffers = 0;                         // nodes whose state byte disagrees with the list (0 = agree)
    int unknownIds = 0;                        // allocated ids the tree data does not know
    uint64_t serial = 0;                       // moves only when the CONTENT changed (the App's plan key)

    // Sorted ids only (for Loadouts::StageContext::allocated).
    std::vector<uint16_t> AllocatedIds() const {
        std::vector<uint16_t> ids;
        ids.reserve(allocated.size());
        for (const AllocatedNode& a : allocated) ids.push_back(a.id);
        return ids;
    }
};

// WATCHER thread only. Holds references: `game` must outlive the reader.
class CharReader {
public:
    CharReader(IGame& game, const Offsets& offsets);

    void SetTree(std::shared_ptr<const PassiveTree> tree);   // null = NoTreeData
    void Kick();                                             // retry the slot discovery now
    void Reset();

    // One read. `table` (may be null) and `treeOpen` enable the UI cross-check. Returns the previous
    // pointer when nothing changed (same content -> same serial), a new CharState otherwise; never
    // null. The cached slots are tried first; the bounded scans (Offsets psdScanFrom..vecScanTo) run
    // only when they fail and the retry timer allows it.
    std::shared_ptr<const CharState> Tick(const WatchTick& t, const TreeTable* table, bool treeOpen);

private:
    IGame& m_game;
    Offsets m_off;
    std::shared_ptr<const PassiveTree> m_tree;
    std::shared_ptr<const CharState> m_state;
    RetryTimer m_retry;
    uint64_t m_areaSerial = 0;
    uint32_t m_psdSlot = 0, m_objSlot = 0;     // slots found by a scan (0 = use the configured ones)
    uint64_t m_nextSerial = 1;
    // ---- implementation state ----
    int32_t m_fieldShift = 0;                  // the object's vectors and scalars moved by this (vector scan)
    bool m_haveArea = false;
    int64_t m_lastUiCheckMs = 0;               // the open-tree cross-check: when it last ran,
    bool m_uiCheckValid = false;               // whether its result is current,
    int m_uiDiffers = 0;                       // what it found,
    std::vector<AllocatedNode> m_uiCheckedList;   // and for which list
    uint64_t m_uiCheckedTable = 0;             // ... and which table serial
    friend struct CharReaderImpl;   // helpers live in CharReader.cpp
};

} // namespace bp
