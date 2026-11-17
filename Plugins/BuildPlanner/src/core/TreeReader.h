#pragma once
// BuildPlanner - the passive tree panel (reader)
// TreeReader.h - finds the game's passive tree panel and builds the table of its node elements
// (which element is which passive, and where it sits inside the canvas); plus the per-frame pose
// that turns the table into screen positions. The offsets it reads, and how to find them again after
// a patch: docs/OFFSETS.md section 4.1. No SDK: everything goes through IGame.
//
// Two halves, two threads:
//   WATCHER thread   TreeReader::Tick: discovery and the node table (thousands of small reads, once
//                    per UI session), cheap revalidation afterwards. Publishes an immutable TreeTable.
//   RENDER thread    ReadTreePose: per frame, 2 element reads and 1 rect batch (the canvas and the
//                    occluders). Nothing else about the tree is read on the render thread.
//
// Discovery, without a private offset first: among UiChildren(GameUiRoot()) the element of
// unscaled size 2560 x 1600 that has a child with >= 1000 children (the canvas) whose descendants
// resolve as nodes. Offsets::treePanelSlot / canvasChildIndex are shortcuts that must pass the same
// test. A node is an element whose [+nodeSkillInfo] -> [+skillInfoRow] -> u16 at +rowGraphId chain
// gives an id the tree data knows (PassiveTree::IndexOf: a base id or the variant id of a
// kNodeSwitchable node, TreeData.h); vtables are never compared.
// The table is accepted when >= 95 % of the candidate elements under the groups resolve AND >= 90 %
// of the tree data's allocatable nodes (PassiveTree::AllocatableCount for the ascendancy present)
// have an element; otherwise the problem is DataMismatch ("tree data does not match the game").
//
// Projection (exact to the pixel against the host formula on every node of the 0.5.5 captures):
//   local(node) = sum over the chain canvas -> ... -> node of child.rel + (parent.mod if child.modPos
//                 and the parent is not the canvas);   size(node) = node's unscaled size
//   per frame:  S = canvasRect.w / canvas.w
//               centre = canvasRect.xy + S * (canvas.mod + local + size / 2);   radius = S * size.x / 2
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Game.h"
#include "GearReader.h"   // TooltipSource, GameTooltip, ReadGameTooltip
#include "Offsets.h"
#include "TreeData.h"
#include "Types.h"

namespace bp {

enum class TreeProblem : uint8_t {
    None = 0,
    NotInGame,       // no character in an area
    NoTreeData,      // the plugin's tree data is not loaded (yet)
    NoUiRoot,        // the panel root is unreadable
    NoPanel,         // no child of the panel root looks like the passive tree
    NoCanvas,        // the panel has no canvas child
    NoNodes,         // the canvas holds no element that resolves as a passive
    DataMismatch,    // nodes resolve, but too few match the tree data: the data is for another game version
    ReadFailed       // reads failed half-way (the game is loading or closing); retried
};

// One node element of the open-or-closed tree.
struct TreeNodeUi {
    uint16_t id = 0;              // passive graph id: the BASE id (PassiveTree::BaseId of what the element names)
    uint32_t node = kNoIndex;     // index into TreeTable::tree->nodes (always valid in an accepted table)
    uint64_t element = 0;         // the UI element (the watcher re-reads its state byte through it)
    float localX = 0, localY = 0; // top-left inside the canvas, unscaled canvas units (local(node) above)
    float w = 0, h = 0;           // unscaled size
    bool visible = false;         // the element's own visible bit: hidden elements (other ascendancies,
                                  // stacked duplicates, locked kNodeConstrained nodes) are never drawn.
                                  // As of the table's build or its last visibility refresh (TreeReader::Tick)
};

// ---- the HUD over the tree ------------------------------------------------------------
// The HUD (control name "HUD", a child of the panel root drawn AFTER the tree panel) covers the bottom
// of the tree without being part of the tree panel. Its parts, found by control name on the watcher
// (UiFind, depth 1 each; the captures of 2026-11-16 at 800x600 and 1447x809):
struct TreeHud {
    uint64_t container = 0;            // "HUD"; 0 = not found (no HUD rectangles)
    uint64_t frameL = 0, frameR = 0;   // "HUDLeft", "HUDRight": the two side frames (the art); a side
                                       // without its frame gives no rectangle
    uint64_t orbL = 0, orbR = 0;       // "energy_shield_frame" / "spirit_bar_frame" in the frames (else
                                       // "life_orb" / "mana_orb" in the HUD); optional
    uint64_t xpBar = 0;                // "experience_bar"; optional
    uint64_t barTop = 0;               // "poise" in the left frame: the top edge of the bars; optional
    bool operator==(const TreeHud&) const = default;
};

inline constexpr size_t kHudParts = 6;          // frameL, frameR, orbL, orbR, xpBar, barTop (this order)
inline constexpr size_t kMaxHudOccluders = 5;
// Fallbacks when an optional part is missing, measured on the side frames at both client sizes: the
// orb's inner edge at this share of the frame's width (from its outer edge), the bars' top at this
// share of its height.
inline constexpr float kHudOrbShare = 0.4366f, kHudBarTopShare = 0.3256f;

// The rectangles that cover the tree, from the parts' rectangles (index order of kHudParts; ok ==
// false = missing or unreadable). Each side gives two, tight to the art, so that passives seen
// between the HUD parts keep their marks:
//   life orb    [frameL.x, orbL.right]  x [frameL.y, frameL.bottom]
//   left bars   [orbL.right, xpBar.x]   x [barTop.y, frameL.bottom]   flasks, charms, buttons, the statue
//   xp bar      the experience bar itself
//   right bars  [xpBar.right, orbR.x]   x [the left bars' top, mirrored, frameR.bottom]   the skill bar
//   mana orb    [orbR.x, frameR.right]  x [frameR.y, frameR.bottom]
// A part outside its frame counts as missing (the fallbacks above; without the xp bar the bars end at
// the frame's inner edge). Returns how many rectangles were written to `out` (0 .. kMaxHudOccluders).
// Pure.
int HudOccluderRects(const Rect (&parts)[kHudParts], Rect (&out)[kMaxHudOccluders]);

inline constexpr size_t kMaxPanelOccluders = 4;   // TreeTable::occluders
inline constexpr size_t kMaxTreeOccluders = kMaxPanelOccluders + kMaxHudOccluders + 3;   // TreePose::occluders (12)

// The node table. Immutable once published (shared_ptr<const TreeTable>); a new table replaces it
// when the UI was rebuilt (a new session, another character, another ascendancy shown).
struct TreeTable {
    TreeProblem problem = TreeProblem::NotInGame;
    bool found = false;                        // true = the fields below are valid (problem == None)
    std::shared_ptr<const PassiveTree> tree;   // the tree data the nodes were resolved against
    uint64_t uiRoot = 0, panel = 0, canvas = 0;
    float canvasW = 0, canvasH = 0;            // the canvas' unscaled size (constant, 37116 x 37139 on 0.5.5)
    int canvasChildren = 0;                    // revalidation: the canvas must keep this child count
    std::vector<TreeNodeUi> nodes;             // sorted by id, unique
    std::vector<uint32_t> uiOfNode;            // per tree node index: index into `nodes`, kNoIndex = no element
    // Elements of the tree panel that cover the tree (the points panel at the top right, the title);
    // ReadTreePose reads their rects every frame. At most kMaxPanelOccluders (kMaxTreeOccluders
    // also counts the HUD's rectangles).
    std::vector<uint64_t> occluders;
    std::string ascendancy;                    // Node::ascendancy of the ascendancy nodes present, "" none
    // Diagnostics:
    int candidates = 0;                        // elements under the groups that were tried
    int resolved = 0;                          // of those, how many resolved to a known id
    float coverage = 0.0f;                     // nodes.size() / AllocatableCount (0..1)
    bool viaShortcut = false;                  // found through Offsets::treePanelSlot (else structurally)
    uint64_t serial = 0;                       // a new value for every table built
    // Diagnostics: the game's own effective scale of the canvas
    // (Offsets::elemEffectiveScale) divided by the host's S (canvas rect width / canvas width), both
    // read when the table was built. 1 = the host's rectangles agree with the game; 0 = not measured.
    float scaleCheck = 0.0f;
    // ---- the HUD and the game's tooltip ----
    TreeHud hud;                               // the HUD's parts (found with the table, again every 1 s
                                               // while the tree is open and they are not)
    bool hudVisible = false;                   // the HUD element's own visible bit (refreshed every tick
                                               // while the tree is open); false = no HUD rectangles
    TooltipSource tooltipSource;               // for TreePose::tooltip (found with the table, checked
                                               // every 2 s while the tree is open)

    // The element of a tree node index, or nullptr when the game shows none for it.
    const TreeNodeUi* OfNode(uint32_t nodeIndex) const {
        if (nodeIndex >= uiOfNode.size() || uiOfNode[nodeIndex] == kNoIndex) return nullptr;
        return &nodes[uiOfNode[nodeIndex]];
    }
};

// What the watcher learns about the tree on one tick.
struct TreeWatch {
    std::shared_ptr<const TreeTable> table;    // never null (a table with found == false says why)
    bool open = false;                         // the tree panel is visible now
};

// WATCHER thread only. Holds references: `game` must outlive the reader.
class TreeReader {
public:
    TreeReader(IGame& game, const Offsets& offsets);

    // The tree data to resolve ids against (null = none: every Tick reports NoTreeData). A new
    // pointer drops the table and rediscovers.
    void SetTree(std::shared_ptr<const PassiveTree> tree);
    // Try discovery again on the next Tick, whatever the retry timer says.
    void Kick();
    // Forget everything (another game process, detach).
    void Reset();

    // One watcher tick. Not in game -> a table with NotInGame (the cache is kept for the same area
    // serial and dropped on a new one). With a valid cached table: cheap revalidation only (the
    // canvas element, its child count and a few nodes' id chains; a failed check rebuilds). Without
    // one: discovery when the retry timer allows it. A DataMismatch is not walked again while its UI
    // stands unchanged (see m_mismatch below; at most once a minute).
    // Visibility refresh: a node's visible bit can change after the table was built (the 200
    // kNodeConstrained nodes of 0_5 are hidden until their unlock node is allocated). When the panel
    // becomes visible, and once a second while it stays open, the visible bit of every table node
    // whose tree node is kNodeConstrained is read again (one UiRead each); when one differs, a COPY
    // of the table with the new bits and a new serial is published (tables stay immutable).
    TreeWatch Tick(const WatchTick& t);

private:
    IGame& m_game;
    Offsets m_off;
    std::shared_ptr<const PassiveTree> m_tree;
    std::shared_ptr<const TreeTable> m_table;
    RetryTimer m_retry;
    uint64_t m_areaSerial = 0;
    uint64_t m_nextSerial = 1;
    int64_t m_lastValidateMs = 0;
    int64_t m_lastVisibleMs = 0;
    bool m_wasOpen = false;
    // ---- implementation state ----
    std::shared_ptr<const TreeTable> m_problem;   // the problem table last published (reused while unchanged)
    std::vector<uint64_t> m_occluderCandidates;   // panel children that may cover the tree, largest first
    size_t m_probe = 0;                           // rotates the nodes revalidation re-resolves
    bool m_haveArea = false;                      // m_areaSerial holds a value
    bool m_forceValidate = true;                  // revalidate on the next Tick (Kick, area change)
    int64_t m_lastHudMs = 0;                      // the last HUD search
    int64_t m_lastSourceMs = 0;                   // the last tooltip source check
    // ---- the DataMismatch verdict's cache ----
    // The DataMismatch table of the last full walk (its uiRoot / panel / canvas / canvasChildren are the
    // key) and when that walk ran. While the same tree data meets the same canvas the verdict cannot
    // change, so a due retry checks the key (GameUiRoot + 2 UiReads) instead of walking thousands of
    // elements again; a full walk runs again when the key differs, after kMismatchRewalkMs, after
    // SetTree / Reset, or when the table is dropped. Null = no mismatch remembered.
    std::shared_ptr<const TreeTable> m_mismatch;
    int64_t m_mismatchMs = 0;
    friend struct TreeReaderImpl;   // helpers live in TreeReader.cpp
};

// The canvas' transform this frame: turns TreeNodeUi into client pixels.
struct TreePose {
    bool ok = false;               // false = nothing may be drawn (unreadable, or a degenerate scale)
    bool panelVisible = false;     // the tree panel's own visible bit this frame
    Rect canvasRect;               // the canvas' rectangle (host formula)
    float scale = 0.0f;            // S: client pixels per unscaled canvas unit
    float originX = 0, originY = 0;   // canvasRect.xy + S * canvas.mod: add S * (local + size / 2) for a centre
    // [0, occluderCount): first the rects of TreeTable::occluders (ok == false when unreadable /
    // hidden), then the HUD's rectangles (HudOccluderRects; at the BOTTOM of the client:
    // never move a card below one of them).
    Rect occluders[kMaxTreeOccluders];
    int occluderCount = 0;
    int hudOccluders = 0;             // how many of the occluders are the HUD's: [occluderCount - hudOccluders, occluderCount)
    // The tooltip the game shows now: any element's (GearReader.h ReadGameTooltip; e.g. an item's),
    // else the passive under the game's own mouse while the panel is visible: the node whose rectangle
    // (the projection below) contains the mouse and whose tooltip the game made (TreeNodeUi::element's
    // tooltip vector). Placement seen on live captures: centred over the node, bottom
    // 20 x 1.3 x clientH / 1600 px above the node's top, moved down to stay inside the client at the
    // top. The "Inspect / Compare" key hints right of the node are a separate element and not part of
    // it. Independent of `ok` (false when the table has no tooltip source or nothing shows; the node
    // test needs `ok`).
    GameTooltip tooltip;

    Vec2 Centre(const TreeNodeUi& n) const {
        return {originX + scale * (n.localX + n.w * 0.5f), originY + scale * (n.localY + n.h * 0.5f)};
    }
    float Radius(const TreeNodeUi& n) const { return scale * n.w * 0.5f; }
    // Is this client point under one of the occluders (the points panel, the title)?
    bool Occluded(Vec2 p) const {
        for (int i = 0; i < occluderCount; ++i)
            if (occluders[i].Contains(p)) return true;
        return false;
    }
};

// RENDER thread, once per frame while a table is found: UiRead(panel) for its visible bit,
// UiRead(canvas) for the modifier and size, one UiRects batch over the canvas, the occluders and the
// HUD's parts (when hudVisible). ok needs: both elements valid, the canvas rect ok, 1e-4 < S < 50,
// every value finite. Then the tooltip: ReadGameTooltip, and only when that finds none, the node
// test (no reads) and ReadTooltipOf for the node under the mouse.
TreePose ReadTreePose(IGame& game, const TreeTable& table);

} // namespace bp
