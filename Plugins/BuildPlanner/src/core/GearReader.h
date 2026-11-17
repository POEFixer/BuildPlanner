#pragma once
// BuildPlanner - equipment slots and equipped items (reader)
// GearReader.h - the equipment slots of the game's inventory panel: which UI element is which slot,
// what is equipped, and per frame where the slots and the game's item tooltip are. The offsets it
// reads, and how to find them again after a patch: docs/OFFSETS.md sections 4.4 and 4.5. No SDK:
// everything goes through IGame.
//
// Two halves, two threads:
//   WATCHER thread   GearReader::Tick: slot discovery (once per area / when the panel opens) and the
//                    equipped items. Publishes an immutable GearState.
//   RENDER thread    ReadGearFrame: one rect batch over the slot elements, the cursor test, and one
//                    more rect for the hovered item's tooltip.
//
// Discovery: the inventory panel is IGame::InventoryPanelRoot(); slot elements are the
// parents of the elements named "build_planner_indicator" below it (UiFind, depth 3, hidden ones
// too); each slot element's [+uiInventoryPtr] is its InventoryStruct and [+uiSlotCell] its
// (col, row, w, h) inside that inventory; the inventory's [+invDatRow] with the cell gives the
// GearSlot: GearSlotFromDatRow(row, col, cellRow, Offsets::invRowRing3). Only cell row 0 binds: the
// Flask1 inventory's hidden, locked second charm row (cells (2..4, 1)) maps to no slot, and when two
// elements still claim one GearSlot a hidden element never replaces a visible one. Accepted when at
// least kMinGearSlots (10) of the 18 slots bind. The slot's item element is its child
// that carries an item entity ([+itemElEntity] with a "Metadata/Items/" path); names and rarity come
// from IGame::ItemRead(entity): no inventory scan is needed.
// Never cache slot positions: the two weapon sets swap position and size when the player toggles the
// displayed set (the element <-> inventory binding is fixed).
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Game.h"
#include "Offsets.h"
#include "Types.h"

namespace bp {

// Discovery is accepted when at least this many GearSlots have an element.
inline constexpr int kMinGearSlots = 10;

// ---- the game's own tooltip, from ANY panel ---------------------------------------
// Shared by the three readers (each frame type carries a GameTooltip). Captures of 2026-11-16:
//   - Every UI element keeps the tooltips the game made for it in a std::vector at [+uiElemTooltips]:
//     entries of uiTooltipStride bytes, the tooltip element pointer first; that element's ParentPtr is
//     the element (the owner). Confirmed for inventory item elements (= their [+itemElTooltip]),
//     Skills panel support sockets and passive tree nodes. The game makes a tooltip on the owner's
//     first hover and KEEPS it (laid out where it last showed, visible bit set) after the hover ends,
//     and some owners carry tooltips that never showed (size 0): a tooltip element that exists is
//     never proof that it shows.
//   - What shows is told by the UI manager ([any element + uiElemMgr], validated by [+mgrRoot] ==
//     the parent of the panel root): [+mgrTooltipOwner] = the element whose item tooltip shows (0 when
//     none; confirmed for items) and [+mgrHovered] = the element under the mouse (confirmed for items
//     and Skills panel sockets; it can be stale: it keeps its last value while the mouse is over the
//     world), with the game's own mouse at [+mgrMouseX / Y] (client px).
//   - The rule (ReadGameTooltip): the owner, else the hovered element; it counts only when its own
//     visible bit is set, its rectangle contains the game's mouse and one of its tooltips has it as
//     parent, the visible bit and a sane rectangle. Item and gem tooltips size themselves; a passive's
//     own size is garbage, so its rectangle is the union of its children [0] (title) and [1] (body).
//   - Not covered by the rule (TreePose adds it): the passive under the mouse (the hovered pointer was
//     never seen on a node). Known gap: a Skills panel socket hovered while the panel is closed with a
//     key keeps its tooltip "shown" until the mouse moves off the socket's old place.

// What the render thread needs to find the game's tooltip; published by the watcher with each state.
// manager == 0: the UI manager was not found (or failed its check): no tooltip is ever reported.
struct TooltipSource {
    uint64_t manager = 0;
    uint32_t hovered = 0, owner = 0, mouseX = 0, mouseY = 0;   // Offsets::mgrHovered, mgrTooltipOwner, mgrMouseX / Y
    uint32_t tooltips = 0, stride = 0;                         // Offsets::uiElemTooltips, uiTooltipStride
    bool operator==(const TooltipSource&) const = default;
};

// WATCHER thread: the UI manager of the panel root `gameUiRoot` (3 reads). manager == 0 when the
// root, the manager pointer or its root check fails.
TooltipSource FindTooltipSource(IGame& game, const Offsets& offsets, uint64_t gameUiRoot);

// The tooltip the game shows now.
struct GameTooltip {
    bool ok = false;              // a tooltip shows and `rect` is its rectangle; false = none, or not known
    Rect rect;                    // client px (rect.ok == ok)
    uint64_t owner = 0;           // the element it belongs to (an item element, a socket, a passive node ...)
    uint64_t element = 0;         // the tooltip element (diagnostics)
};

// What ReadGameTooltip read from the UI manager this frame.
struct GameHover {
    bool ok = false;              // the manager's fields were read (false: the fields below are unset)
    Vec2 mouse;                   // the game's own mouse, client px
    uint64_t hovered = 0, owner = 0;
};

// RENDER thread: the rule above. Cost: one read of the manager's fields; per candidate owner (at most
// two) one vector read and one entry read; for a tooltip found two UiReads (the tooltip, the owner),
// UiChildren only for a passive's tooltip, and one UiRects batch (the owner and the tooltip or its two
// children). `hover` (optional) receives what was read from the manager. Never throws.
GameTooltip ReadGameTooltip(IGame& game, const TooltipSource& source, GameHover* hover = nullptr);

// RENDER thread: the tooltip of one given owner, under the same checks (visible owner whose rectangle
// contains `mouse`, a tooltip that is its child, sane rectangle). TreePose uses it for the passive
// under the mouse.
GameTooltip ReadTooltipOf(IGame& game, const TooltipSource& source, uint64_t owner, Vec2 mouse);

enum class GearProblem : uint8_t {
    None = 0,
    NotInGame,
    NoPanel,         // the inventory panel root is unreadable
    NoSlots,         // fewer than kMinGearSlots slot elements bind to equipment inventories (a patch
                     // moved an offset?)
    ReadFailed
};

struct EquippedItem {
    bool present = false;         // false = the slot is empty (the other fields are unset)
    uint64_t entity = 0;
    std::string path;             // BaseItemTypes Id
    std::string baseName;         // localized base type name
    std::string uniqueName;       // localized name words (a unique's name; a rare's two-word name)
    std::string englishName;      // IGame::ItemEnglishName, "" when the host cannot tell
    Rarity rarity = Rarity::Normal;

    // What to show for it: the unique / rare name when it has one, else the base.
    const std::string& DisplayName() const { return uniqueName.empty() ? baseName : uniqueName; }
};

struct GearSlotState {
    bool known = false;           // a slot element was found for this GearSlot
    uint64_t element = 0;         // the slot element (rect source)
    bool visible = false;         // its own visible bit: false = a locked / hidden slot (Ring 3, charm row 2)
    uint64_t itemElement = 0;     // the item element inside it, 0 = empty slot
    uint64_t tooltipElement = 0;  // the game's item tooltip for that item: [+itemElTooltip], accepted only
                                  // when its parent is the item element. The game creates it on the FIRST
                                  // hover and keeps it; 0 = not created yet (or the offset moved)
    EquippedItem item;
};

// The equipment as of one watcher tick. Immutable once published.
struct GearState {
    GearProblem problem = GearProblem::NotInGame;
    bool found = false;                     // the slots are known (problem == None)
    bool panelOpen = false;                 // the inventory panel is visible
    uint64_t panel = 0;
    std::array<GearSlotState, kGearSlotCount> slots;   // index = GearSlot
    bool itemsKnown = false;                // `item` of every known slot was read at least once this area
    int boundSlots = 0;                     // diagnostics: how many slots bound
    uint64_t serial = 0;                    // moves when anything above changes
    uint64_t itemsSerial = 0;               // moves only when an equipped item changes (the App's match key)
    // ---- the game's tooltip ----
    TooltipSource tooltipSource;            // for ReadGearFrame's `tooltip` (found per area, checked every 2 s)

    const GearSlotState& Slot(GearSlot s) const { return slots[static_cast<size_t>(s)]; }
};

// WATCHER thread only. Holds references: `game` must outlive the reader.
class GearReader {
public:
    GearReader(IGame& game, const Offsets& offsets);

    void Kick();    // rediscover the slots now (the panel opened, the area changed)
    void Reset();

    // One watcher tick: the panel's visible bit every time; discovery when needed and allowed by the
    // retry timer; equipped items when the panel opens and then at most every 250 ms while it is
    // open (items change rarely). The tooltip pointers are NOT on that cadence: while the panel is
    // open, tooltipElement of every occupied slot that has none yet is read on EVERY tick (16 ms; one
    // pointer read per such slot, plus one UiRead for the parent check when it turns non-zero), so
    // the game's tooltip is known within a frame or two of the first hover. Returns the previous
    // pointer when nothing changed; never null.
    std::shared_ptr<const GearState> Tick(const WatchTick& t);

private:
    IGame& m_game;
    Offsets m_off;
    std::shared_ptr<const GearState> m_state;
    RetryTimer m_retry;
    uint64_t m_areaSerial = 0;
    int64_t m_lastItemsMs = 0;
    bool m_wasOpen = false;
    uint64_t m_nextSerial = 1;
    friend struct GearReaderImpl;   // helpers live in GearReader.cpp
    // ---- implementation state (defined in GearReader.cpp) ----
    std::shared_ptr<struct GearWork> m_work;
};

// ---------------------------------------------------------------------------------------------
// What GearReader.cpp adds to the contract above:
//   - The equipped entity of a slot comes from the slot's InventoryStruct ([+invItems], one entry
//     per cell, row-major; the item whose span covers the slot's cell), never from the UI alone: a
//     slot element of the weapon set that is not displayed draws the other set's items ghosted.
//     The item element is the slot's child whose [+itemElEntity] IS that entity.
//   - Inventory data exists before the inventory panel was ever opened (the item elements, and so
//     the tooltips, do not): the items are also read at discovery and every 2 s while the panel is
//     closed, so `itemsKnown` does not wait for the first open.
//   - A slot binds only when its cell fits the inventory's TotalBoxes and, for slots with a host id,
//     the host's InvGet(id).address (when it publishes one) is that very InventoryStruct.

// One item of an InventoryStruct (cells, exclusive end = x + w).
struct InvCellItem {
    uint64_t itemStruct = 0;
    uint64_t entity = 0;
    int x = 0, y = 0, w = 0, h = 0;
};

// The largest inventory side the readers accept (equipment 1..5, skill gems 15).
inline constexpr int kMaxInventorySide = 32;

// The distinct items of the InventoryStruct `inv` (TotalBoxes boxesX x boxesY), in cell order (y, x).
// false when the item vector or an item struct is malformed (more entries than cells, an entry
// outside the plausible range, a span outside the boxes); an item struct whose entity is 0 is left
// out. Reads: the vector header, its entries in one read, three small reads per distinct item.
// Used by GearReader and SkillsReader (watcher thread).
bool ReadInventoryItems(IGame& game, const Offsets& o, uint64_t inv, int boxesX, int boxesY, std::vector<InvCellItem>& out);

// The slots on screen this frame.
struct GearFrame {
    bool ok = false;                        // false = nothing may be drawn
    std::array<Rect, kGearSlotCount> slotRect;   // ok == false for unknown, hidden or unreadable slots
    int hovered = kNoGearSlot;              // the slot under the cursor (our own rect test), kNoGearSlot none
    bool hoveredHasItem = false;            // the hovered slot holds an item: the game shows its tooltip
    // The game's item tooltip for the hovered slot (it sits LEFT of the item, top clamped to 0);
    // ok == false when the slot is empty or the tooltip element is not known yet.
    // Where the build item card goes (never over the game's tooltip):
    //   gameTooltip.ok                      left of the tooltip when there is room, else below it;
    //   !gameTooltip.ok && hoveredHasItem   BELOW the slot: the game's tooltip is opening left of the
    //                                       item right now, so "left of the slot" would cover it;
    //   !hoveredHasItem (an empty slot)     left of the slot, top-aligned with it.
    // Always clamped to the client.
    Rect gameTooltip;
    // ---- the inventory panel and the game's tooltip ----
    Rect panel;                             // the inventory panel (GearState::panel), from the slots' batch;
                                            // ok == false when unreadable
    // The tooltip the game shows now, whatever it belongs to (ReadGameTooltip): an equipped item (it
    // opens left of the item, top clamped), any other item element (the backpack's are the same element
    // class; not captured), a Skills panel gem (it opens right of its socket, over the inventory panel),
    // any element the game made a tooltip for. Nothing of ours may be drawn on it. Read whenever ok's
    // prerequisites hold (state.found, panel open), even when no slot rect is usable.
    GameTooltip tooltip;
};

// RENDER thread, once per frame while the inventory panel is open and state.found: one UiRects batch
// over the panel and the visible slot elements, then the cursor test (`mouseValid` false = no hover),
// then one UiRect for the hovered slot's tooltip element when it is known, then ReadGameTooltip.
GearFrame ReadGearFrame(IGame& game, const GearState& state, Vec2 mouse, bool mouseValid);

} // namespace bp
