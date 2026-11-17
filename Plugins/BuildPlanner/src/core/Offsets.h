#pragma once
// BuildPlanner - the private game offsets
// Offsets.h - the plugin's PRIVATE game offsets: only what no SDK call covers. Everything
// the host decodes (UI element fields, rects, inventories, item names) comes through IGame instead.
//
// Values: built-in defaults (this header) <- <plugin>/data/offsets.json <- <plugin>/config/offsets.json.
// The shipped data file equals the defaults (tests/TestOffsets.cpp pins that); the config file is the
// user's override after a game patch. File format: one JSON object, keys = the field names below,
// values "0x..." hex strings or non-negative integers up to kMaxOffsetValue (up to 0xFFFFFFFF in the
// "diagnostics" group); keys starting with '_' are comments. Unknown keys and bad values are reported
// and ignored, never fatal.
//
// Sources: memory snapshots of the 0.5.5 client of 2026-11-16. How each field was found, and how to
// find it again after a patch: docs/OFFSETS.md (section 4 per group; section 6 to ship new values).
//
// Threads: Offsets is a plain value. App loads it once in Start (render thread) and hands copies to
// the readers; nothing mutates a copy afterwards.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bp {

// X(name, default, group, comment). `group` only orders the Diagnostics table and the data file.
// Adding a field: add a line here and the same key to data/offsets.json (the test fails otherwise).
#define BP_OFFSET_FIELDS(X)                                                                                                    \
    /* ---- passive tree panel (docs/OFFSETS.md section 4.1) ---- */                                                           \
    X(treePanelSlot, 0x718, "tree", "UiRootStruct: -> passive tree panel. A shortcut only: the panel must pass the structural test") \
    X(canvasChildIndex, 2, "tree", "tree panel child index of the pan/zoom canvas (shortcut; the canvas is found by its child count)") \
    X(nodeSkillInfo, 0x2C8, "tree", "node element: -> SkillInfo")                                                              \
    X(skillInfoRow, 0x00, "tree", "SkillInfo: -> PassiveSkills.dat row (unaligned pointer)")                                   \
    X(rowGraphId, 0x30, "tree", "PassiveSkills row: u16 PassiveSkillGraphId (the Path of Building node id)")                   \
    X(rowIdString, 0x00, "tree", "PassiveSkills row: -> UTF-16 Id string (diagnostics only)")                                  \
    X(nodeState, 0x648, "tree", "node element: u8 allocation: 0 allocated, 1 Weapon Set I, 2 Weapon Set II, 4 not allocated")  \
    X(nodeKind, 0x640, "tree", "node element: u8 kind: 0 normal, 1 jewel socket, 3 mastery art, 4/7 hidden (optional)")        \
    X(nodeAttrIcon, 0x508, "tree", "node element: -> attribute-choice texture object, 0 unless an allocated attribute node (optional)") \
    X(attrIconPath, 0x08, "tree", "attribute texture object: -> texture path string (plusstrength / plusdexterity / plusintelligence)") \
    X(nodeTooltipAttach, 0x120, "tree", "node element: -> array of {UiElement* tooltip, float2 anchor, u64} (diagnostics; read as uiElemTooltips)") \
    X(elemEffectiveScale, 0xF8, "tree", "any UI element: float effective scale; on the canvas it must equal canvasRect.w / canvas.w") \
    /* ---- character passives (docs/OFFSETS.md section 4.2) ---- */                                                           \
    X(psdSlot, 0x310, "character", "UiRootStruct: -> PlayerServerData (PSD)")                                                  \
    X(serverDataSlot, 0x308, "character", "UiRootStruct: -> ServerData (cross-check holder of the PSD)")                       \
    X(serverDataPsdVector, 0x48, "character", "ServerData: std::vector<PSD*>; [0] must equal the PSD")                         \
    X(treePanelPsdA, 0x510, "character", "tree panel: -> PSD (cross-check holder)")                                            \
    X(treePanelPsdB, 0xC10, "character", "tree panel: -> PSD (cross-check holder)")                                            \
    X(passiveObjSlot, 0x7658, "character", "PSD: -> passive allocation object")                                                \
    X(passiveObjAlias, 0x7650, "character", "PSD: holds the passive object + objSelfBias")                                     \
    X(objSelfBias, 0x10, "character", "the alias and objSelfA hold the object's address plus this")                            \
    X(objSelfA, 0x18, "character", "passive object: == object + objSelfBias")                                                  \
    X(objSelfB, 0x20, "character", "passive object: == object")                                                                \
    X(objAllocated, 0x1B8, "character", "passive object: std::vector<u32> allocated: low16 graph id (ascending), byte 2 weapon set 0/1/2") \
    X(objAttributes, 0x1E8, "character", "passive object: std::vector<u32> attribute choices: low16 node id, high16 option id") \
    X(objGranted, 0x200, "character", "passive object: std::vector<u16> nodes granted by items (not paid with points)")        \
    X(objJewelSockets, 0x230, "character", "passive object: std::vector of jewel socket records")                              \
    X(jewelSocketStride, 0x18, "character", "jewel socket record size")                                                        \
    X(jewelSocketGraphId, 0x10, "character", "jewel socket record: u16 graph id")                                              \
    X(objLevel, 0x2D4, "character", "passive object: i32 character level")                                                     \
    X(objQuestPoints, 0x2D8, "character", "passive object: i32 passive points from quests")                                    \
    X(objWeaponSetPoints, 0x2DC, "character", "passive object: i32 weapon set points, the TOTAL per set")                      \
    X(objAscPointsA, 0x2E8, "character", "passive object: i32 ascendancy points (total or spent; the order is not settled)")   \
    X(objAscPointsB, 0x2EC, "character", "passive object: i32 ascendancy points (the other one)")                              \
    X(psdScanFrom, 0x200, "character", "fallback scan of UiRootStruct for the PSD slot: from")                                 \
    X(psdScanTo, 0x600, "character", "fallback scan of UiRootStruct for the PSD slot: to (exclusive)")                         \
    X(objScanFrom, 0x6000, "character", "fallback scan of the PSD for the passive object slot: from")                          \
    X(objScanTo, 0x9000, "character", "fallback scan of the PSD for the passive object slot: to (exclusive)")                  \
    X(vecScanFrom, 0x100, "character", "fallback scan of the passive object for the allocated vector: from")                   \
    X(vecScanTo, 0x300, "character", "fallback scan of the passive object for the allocated vector: to (exclusive)")           \
    /* ---- diagnostics only: shown, NEVER compared (they change with every build of the game) ---- */                         \
    X(nodeVtableRva, 0x30D13F8, "diagnostics", "node element vtable RVA on the reference client (never compared)")             \
    X(groupVtableRva, 0x2F962D0, "diagnostics", "group container vtable RVA on the reference client (never compared)")         \
    X(ascContainerVtableRva, 0x30C1378, "diagnostics", "ascendancy container vtable RVA on the reference client (never compared)") \
    X(passiveObjVtableRva, 0x33BA6F0, "diagnostics", "passive object vtable RVA on the reference client (never compared)")     \
    /* ======== gear / skills block (docs/OFFSETS.md sections 4.4 to 4.7) ======== */                                          \
    /* ---- UI manager and hover ---- */                                                                                       \
    X(uiElemSelf, 0x08, "hover", "any UI element: self pointer (validates a hovered pointer)")                                 \
    X(uiElemMgr, 0x90, "hover", "any UI element: -> UI manager")                                                               \
    X(mgrRoot, 0x688, "hover", "UI manager: -> true UI root (must equal the parent of the panel root)")                        \
    X(mgrHovered, 0x698, "hover", "UI manager: -> element under the mouse (may be stale: validate)")                           \
    X(mgrMouseX, 0x71C, "hover", "UI manager: i32 mouse x, client px")                                                         \
    X(mgrMouseY, 0x720, "hover", "UI manager: i32 mouse y, client px")                                                         \
    X(mgrTooltipOwner, 0x760, "hover", "UI manager: -> element whose item tooltip is shown, 0 none")                           \
    X(itemElEntity, 0x4E0, "hover", "inventory item element: -> item entity")                                                  \
    X(itemElTooltip, 0x598, "hover", "inventory item element: -> its tooltip element (ParentPtr == the item element)")         \
    X(uiElemTooltips, 0x120, "hover", "any UI element: std::vector of its tooltips {UiElement* tooltip, float2 anchor, u64} (tooltip ParentPtr == the element)") \
    X(uiTooltipStride, 0x18, "hover", "tooltip vector entry size (the tooltip element pointer at +0)")                          \
    /* ---- equipment slots and inventories ---- */                                                                            \
    X(equipContainerChildIndex, 5, "gear", "inventory panel child index of the equipment container (shortcut)")                \
    X(uiInventoryPtr, 0x6F8, "gear", "inventory-bound UI element: -> InventoryStruct (only valid on inventory elements)")      \
    X(uiSlotCell, 0x710, "gear", "inventory-bound UI element: four u32 col, row, w, h of the slot inside its inventory")       \
    X(invDatRow, 0x144, "gear", "InventoryStruct: u32 Inventories.dat row index (the reliable identity)")                      \
    X(invBoxes, 0x150, "gear", "InventoryStruct: two i32 TotalBoxes x, y")                                                     \
    X(invItems, 0x170, "gear", "InventoryStruct: std::vector<InventoryItemStruct*>, one entry per CELL (dedupe by pointer)")   \
    X(invRequestCounter, 0x1E8, "gear", "InventoryStruct: i32 server request counter (moves when the inventory changes)")      \
    X(invItemEntity, 0x00, "gear", "InventoryItemStruct: -> item entity")                                                      \
    X(invItemSlotStart, 0x08, "gear", "InventoryItemStruct: two i32 slot start x, y")                                          \
    X(invItemSlotEnd, 0x10, "gear", "InventoryItemStruct: two i32 slot end x, y (exclusive)")                                  \
    X(psdInventories, 0x320, "gear", "PSD: std::vector of player inventory records")                                           \
    X(psdInventoryStride, 0x18, "gear", "player inventory record size: i32 id, pad, -> InventoryStruct, -> second struct")     \
    X(psdInventoryPtr, 0x08, "gear", "player inventory record: -> InventoryStruct")                                            \
    X(invRowRing3, 101, "gear", "Inventories.dat row of Ring3 (the equipment row most likely to move with a patch)")            \
    /* ---- entities, components, gems ---- */                                                                                 \
    X(entityDetails, 0x08, "gems", "entity: -> EntityDetails")                                                                 \
    X(entityComponents, 0x10, "gems", "entity: std::vector<Component*>")                                                       \
    X(detailsPath, 0x08, "gems", "EntityDetails: std::wstring metadata path")                                                  \
    X(detailsLookup, 0x28, "gems", "EntityDetails: -> component lookup")                                                       \
    X(lookupBuckets, 0x28, "gems", "component lookup: std::vector of buckets")                                                 \
    X(lookupBucketStride, 0x10, "gems", "component lookup bucket size: char* name, i32 index, pad")                            \
    X(socketsOwner, 0x08, "gems", "Sockets component: -> owner entity")                                                        \
    X(socketsEntities, 0x30, "gems", "Sockets component: six entity pointers, 0 = empty socket")                               \
    X(socketsCounts, 0x60, "gems", "Sockets component: std::vector<u8>, size = socket count")                                  \
    X(modsGrantedSkills, 0x210, "gems", "Mods component of an item: std::vector<Entity*> of the skill gems it grants")         \
    X(skillGemLevel, 0x24, "gems", "SkillGem component: u32 the gem's own level")                                              \
    X(invRowSkillSlots, 62, "gems", "Inventories.dat row of SkillSlots1 (the character's skill gems)")                         \
    X(invRowAscendancySkills, 95, "gems", "Inventories.dat row of AscendancySkills1")                                          \
    /* ---- Skills panel ---- */                                                                                               \
    X(skillsPanelSlot, 0x740, "skills", "UiRootStruct: -> Skills panel (shortcut: validated by the structural recipe)")        \
    X(bigSocketRow, 0x310, "skills", "big socket element: u32 Inventories.dat row of the skill gem's inventory")               \
    X(bigSocketSlot, 0x314, "skills", "big socket element: u32 slot x of the skill gem")                                       \
    X(supSocketOwnerRow, 0x308, "skills", "support socket element: u32 owner gem's Inventories.dat row")                       \
    X(supSocketOwnerSlot, 0x30C, "skills", "support socket element: u32 owner gem's slot x")                                   \
    X(supSocketIndex, 0x314, "skills", "support socket element: u32 socket index 0..4")                                        \
    /* ======== end of the gear / skills block ======== */

struct Offsets {
#define BP_OFFSET_MEMBER(name, value, group, comment) uint32_t name = value;
    BP_OFFSET_FIELDS(BP_OFFSET_MEMBER)
#undef BP_OFFSET_MEMBER

    bool operator==(const Offsets&) const = default;
};

// One row of the field table (the order of BP_OFFSET_FIELDS).
struct OffsetField {
    const char* name;              // the JSON key and the member name
    uint32_t Offsets::* member;
    uint32_t builtIn;              // the default compiled in
    const char* group;
    const char* comment;           // English, one line
};
std::span<const OffsetField> OffsetFields();

// Where a field's value in use came from (the later file wins).
enum class OffsetSource : uint8_t { BuiltIn = 0, DataFile = 1, ConfigFile = 2 };

// The largest value a field outside the "diagnostics" group may take. Every offset,
// index and row of the reference client is far below it (the largest, objScanTo, is 0x9000); a larger
// value is a typo or a "disable" placeholder such as 0xFFFFFFFF, and the readers' bounds arithmetic is
// written for plausible offsets only. The diagnostics group holds module RVAs (0x33BA6F0) and is
// never used to read.
inline constexpr uint32_t kMaxOffsetValue = 0x100000;

// Applies a JSON object text onto `o`. Unknown keys, bad values, values over 0xFFFFFFFF and values
// over kMaxOffsetValue outside the "diagnostics" group are skipped (the field keeps its value) and
// named in `warnings` (English, "offsets: ..."). `touched` (optional) is resized to
// OffsetFields().size() and gets 1 for every field the text set. Returns false, leaving `o` and
// `touched` untouched, only when the text is not a JSON object. Never throws.
bool ParseOffsets(std::string_view jsonText, Offsets& o, std::vector<std::string>* warnings,
                  std::vector<uint8_t>* touched = nullptr);

// The values in use and how they came about (the Diagnostics card shows it).
struct OffsetsInfo {
    Offsets values;
    std::vector<OffsetSource> source;     // parallel to OffsetFields()
    bool dataFileRead = false;            // <plugin>/data/offsets.json was found and was a JSON object
    bool configFileRead = false;          // <plugin>/config/offsets.json likewise
    std::vector<std::string> warnings;    // English; empty = clean
};

// Built-in defaults <- dataDir/offsets.json <- configDir/offsets.json. A missing file is not a
// warning; an unreadable or malformed one is. Files over 256 KiB are refused. Never throws.
OffsetsInfo LoadOffsets(const std::filesystem::path& dataDir, const std::filesystem::path& configDir);

// "0x718".
std::string OffsetHex(uint32_t value);

} // namespace bp
