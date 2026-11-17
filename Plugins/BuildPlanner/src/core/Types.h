#pragma once
// BuildPlanner - plain types every module shares
// Types.h - the plain types every module shares: geometry, small enums, the equipment slot table and
// the build data model. No SDK, no ImGui, no Windows; header-only.
//
// Threads: everything here is a value type. A StoredBuild lives in the BuildStore, which the render
// thread owns; other threads get copies (jobs) or immutable snapshots.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bp {

// ---------------------------------------------------------------------------------------------
// Geometry (game client pixels unless a field says otherwise)
// ---------------------------------------------------------------------------------------------

struct Vec2 {
    float x = 0.0f, y = 0.0f;
};

// Top-left + size. `ok` is false when the rectangle could not be computed (unreadable element):
// never use x/y/w/h of a rect that is not ok.
struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
    bool ok = false;

    float Right() const { return x + w; }
    float Bottom() const { return y + h; }
    bool Contains(float px, float py) const { return ok && px >= x && py >= y && px < x + w && py < y + h; }
    bool Contains(Vec2 p) const { return Contains(p.x, p.y); }
};

// "No index" for every uint32_t index in the plugin (tree node index, group index, ...).
inline constexpr uint32_t kNoIndex = 0xFFFFFFFFu;

// Colours are kept in ImGui's IM_COL32 layout (R in the low byte, A in the high byte), so ui/ can
// pass them to an ImDrawList unchanged. Files keep them as "#RRGGBBAA" (Settings.h).
constexpr uint32_t Rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) {
    return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
}

// ---------------------------------------------------------------------------------------------
// Small enums
// ---------------------------------------------------------------------------------------------

// A character attribute. Used for the choice on "+5 to any Attribute" passives (PoB option order
// 1 Str, 2 Dex, 3 Int) and for a gem's colour (None = white / no attribute).
enum class Attr : uint8_t { None = 0, Str = 1, Dex = 2, Int = 3 };

// Item rarity. The numeric values 0..3 equal the game's (SDK ReadItemRarity); Relic exists only in
// PoB item text ("Rarity: RELIC").
enum class Rarity : uint8_t { Normal = 0, Magic = 1, Rare = 2, Unique = 3, Relic = 4 };

// How a passive is allocated: with ordinary points, or with the points of Weapon Set I / II.
// The values equal the game's "set" byte (docs/OFFSETS.md sections 4.1 and 4.2) and PoB's allocMode.
enum class WeaponSet : uint8_t { None = 0, One = 1, Two = 2 };

// ---------------------------------------------------------------------------------------------
// Equipment slots
// ---------------------------------------------------------------------------------------------

// The slots the plugin gives gear hints for, in the order of kGearSlots. Path of Building names more
// slots (Arm / Leg limbs, "... Jewel Socket n", "Jewel <node>"): those have no GearSlot and are kept
// in the stored build only (BuildItemSlot::name).
enum class GearSlot : uint8_t {
    Weapon1 = 0,   // main hand, weapon set I
    Offhand1,      // off hand, weapon set I
    Weapon2,       // main hand, weapon set II ("Weapon 1 Swap")
    Offhand2,      // off hand, weapon set II ("Weapon 2 Swap")
    Helm,
    BodyArmour,
    Gloves,
    Boots,
    Belt,
    Amulet,
    Ring1,
    Ring2,
    Ring3,
    Flask1,
    Flask2,
    Charm1,
    Charm2,
    Charm3,
    Count
};
inline constexpr size_t kGearSlotCount = static_cast<size_t>(GearSlot::Count);
inline constexpr int kNoGearSlot = -1;

// One row per GearSlot (index = the enum value).
//   pobName      Path of Building's <Slot name> (exact, case-sensitive).
//   inventoryId  the game's Inventories.dat Id: the `inventory_id` of the game's own .build format.
//   datRow       the Inventories.dat row index: the only reliable identity of an inventory in memory
//                (InventoryStruct + Offsets::invDatRow, docs/OFFSETS.md section 4.5).
//   hostId       the host / SDK inventory id (ctx()->Inventory): row + 1 for every slot here except
//                Ring3, whose id differs per client build (0 = resolve it by datRow, never by id).
//   cellX        the item's column inside that inventory: 0 for everything but the Flask1 inventory,
//                which holds two flasks (x 0, 1) and three charms (x 2, 3, 4). Every slot here is in
//                cell ROW 0: the Flask1 inventory's row 1 (cells 2..4) is the game's hidden, locked
//                second charm row and is no GearSlot.
// Ring3's datRow (101) is the one most likely to move with a game patch: readers take it from
// Offsets::invRowRing3 and pass it to GearSlotFromDatRow; the table's value is the reference client's.
struct GearSlotInfo {
    GearSlot slot;
    const char* pobName;
    const char* inventoryId;
    int datRow;
    int hostId;
    int cellX;
};

inline constexpr std::array<GearSlotInfo, kGearSlotCount> kGearSlots = {{
    {GearSlot::Weapon1, "Weapon 1", "Weapon1", 2, 3, 0},
    {GearSlot::Offhand1, "Weapon 2", "Offhand1", 3, 4, 0},
    {GearSlot::Weapon2, "Weapon 1 Swap", "Weapon2", 14, 15, 0},
    {GearSlot::Offhand2, "Weapon 2 Swap", "Offhand2", 15, 16, 0},
    {GearSlot::Helm, "Helmet", "Helm1", 4, 5, 0},
    {GearSlot::BodyArmour, "Body Armour", "BodyArmour1", 1, 2, 0},
    {GearSlot::Gloves, "Gloves", "Gloves1", 8, 9, 0},
    {GearSlot::Boots, "Boots", "Boots1", 9, 10, 0},
    {GearSlot::Belt, "Belt", "Belt1", 10, 11, 0},
    {GearSlot::Amulet, "Amulet", "Amulet1", 5, 6, 0},
    {GearSlot::Ring1, "Ring 1", "Ring1", 6, 7, 0},
    {GearSlot::Ring2, "Ring 2", "Ring2", 7, 8, 0},
    {GearSlot::Ring3, "Ring 3", "Ring3", 101, 0, 0},
    {GearSlot::Flask1, "Flask 1", "Flask1", 11, 12, 0},
    {GearSlot::Flask2, "Flask 2", "Flask1", 11, 12, 1},
    {GearSlot::Charm1, "Charm 1", "Flask1", 11, 12, 2},
    {GearSlot::Charm2, "Charm 2", "Flask1", 11, 12, 3},
    {GearSlot::Charm3, "Charm 3", "Flask1", 11, 12, 4},
}};

inline const GearSlotInfo& InfoOf(GearSlot s) { return kGearSlots[static_cast<size_t>(s)]; }

// The GearSlot index of a PoB slot name, or kNoGearSlot for a name the plugin has no slot for.
inline int GearSlotFromPobName(std::string_view pobName) {
    for (const GearSlotInfo& g : kGearSlots)
        if (pobName == g.pobName) return static_cast<int>(g.slot);
    return kNoGearSlot;
}

// The GearSlot index of a slot element's binding (Inventories.dat row of its inventory, and the
// column and row of its cell there), or kNoGearSlot. Only cell row 0 binds: the hidden second charm
// row (Flask1 cells (2..4, 1)) must never take the place of Charm 1..3. `ring3Row` is the
// Inventories.dat row of Ring3 in use (Offsets::invRowRing3).
inline int GearSlotFromDatRow(int datRow, int cellX, int cellY, int ring3Row = 101) {
    if (cellY != 0) return kNoGearSlot;
    for (const GearSlotInfo& g : kGearSlots) {
        const int row = g.slot == GearSlot::Ring3 ? ring3Row : g.datRow;
        if (row == datRow && g.cellX == cellX) return static_cast<int>(g.slot);
    }
    return kNoGearSlot;
}

// ---------------------------------------------------------------------------------------------
// The build data model (builds.json version 1)
// ---------------------------------------------------------------------------------------------
// Every index into a sibling list is 0-based and -1 means "none". Every text is UTF-8 with Path of
// Building colour codes already stripped. Lists are capped at import and at load (kMax*), so code that
// walks them needs no further size checks.

inline constexpr size_t kMaxBuilds = 200;
inline constexpr size_t kMaxSpecs = 64;            // trees per build
inline constexpr size_t kMaxNodes = 400;           // nodes per tree
inline constexpr size_t kMaxSpecSockets = 64;      // jewel sockets with a jewel per tree
inline constexpr size_t kMaxSpecNotes = 128;       // node notes per tree
inline constexpr size_t kMaxSkillSets = 64;
inline constexpr size_t kMaxGroups = 64;           // skill groups per skill set
inline constexpr size_t kMaxGems = 16;             // gems per group
inline constexpr size_t kMaxItemSets = 64;
inline constexpr size_t kMaxSetSlots = 64;         // slots per item set
inline constexpr size_t kMaxItems = 512;
inline constexpr size_t kMaxModLines = 64;         // implicit + explicit lines per item, each list
inline constexpr size_t kMaxRunes = 8;
inline constexpr size_t kMaxNameBytes = 200;       // build name, item name, gem name, labels
inline constexpr size_t kMaxSourceBytes = 2048;
inline constexpr size_t kMaxTitleBytes = 256;      // tree / skill set / item set titles
inline constexpr size_t kMaxNoteBytes = 2048;      // one node note
inline constexpr size_t kMaxModBytes = 512;        // one mod line
inline constexpr size_t kMaxRawItemBytes = 4096;   // BuildItem::raw
inline constexpr size_t kMaxStoreFileBytes = 12u << 20;   // builds.json as written (file guard)

struct SpecSocket {
    uint16_t nodeId = 0;       // the jewel socket's passive id
    int itemIndex = -1;        // index into BuildData::items, -1 = the jewel is not in the build's items
};

struct SpecNote {
    uint16_t nodeId = 0;
    std::string text;          // the author's note for this node (<= kMaxNoteBytes)
};

// One passive tree of the build ("spec" in Path of Building; a stage of a leveling build).
struct BuildSpec {
    std::string title;              // "" when PoB had none
    std::string treeVersion;        // "0_5" ("" unknown): the tree data version the ids belong to
    int classId = -1;               // the class's integerId of the tree data (1 Witch .. 11 Druid), -1 unknown
    std::string ascendancyId;       // the ascendancy's internalId ("Sorceress3"), "" = none / unknown
    // Every allocated passive id, sorted ascending, unique. As Path of Building writes it: the class
    // start node and the ascendancy start node are INCLUDED (the game's own list has neither).
    std::vector<uint16_t> nodes;
    // Subsets of `nodes` (sorted, unique): allocated with Weapon Set I / II points. A node in neither
    // is allocated with ordinary points.
    std::vector<uint16_t> weaponSet1, weaponSet2;
    // "+5 to any Attribute" nodes by the attribute the build picks (subsets of `nodes`, sorted,
    // pairwise disjoint). An attribute node in none of them: the build does not say.
    std::vector<uint16_t> attrStr, attrDex, attrInt;
    std::vector<SpecSocket> sockets;   // sorted by nodeId
    std::vector<SpecNote> notes;       // sorted by nodeId
};

// Where a skill group comes from.
enum class GroupSource : uint8_t {
    Gems = 0,        // the player's own gems (the groups the hints are about)
    Item,            // source="Item:..." (a skill granted by an item)
    Tree,            // source="Tree:..." (a skill granted by a passive)
    DefaultAttack    // source="Default Attack", and PoB's other generated groups ("Explode", "Thorns")
};

struct BuildGem {
    std::string name;        // display name as PoB has it (English), never ""
    std::string gemId;       // BaseItemTypes Id ("Metadata/Items/Gems/SkillGemFireball"), "" unknown.
                             // Byte-identical to the path of the gem's item entity in the game.
                             // PobImport fills an absent one from GemData (by skillId, then by name),
                             // so "" means the gem table does not know the gem either.
    std::string skillId;     // granted effect id ("FireballPlayer"), "" unknown
    std::string variantId;   // "" none
    int level = 0;           // 0 unknown
    int quality = 0;
    bool enabled = true;
    bool support = false;    // resolved at import (GemData, else the id heuristic of IsSupportGem)
};

// One skill group: the gems that work together. The group's main skill is the mainActive-th
// non-support gem; its supports are the group's enabled support gems.
struct BuildSkillGroup {
    std::string label;       // "" when the author gave none
    bool enabled = true;
    int mainActive = 0;      // 0-based index among the group's NON-support gems (clamped at import)
    GroupSource source = GroupSource::Gems;
    std::vector<BuildGem> gems;
};

struct BuildSkillSet {
    std::string title;
    std::vector<BuildSkillGroup> groups;
};

struct BuildItemSlot {
    std::string name;        // PoB's slot name ("Weapon 1", "Helmet", "Charm 2", "Jewel 26725", ...)
    int itemIndex = -1;      // index into BuildData::items, -1 = the slot is empty in this set
};

struct BuildItemSet {
    std::string title;
    bool useSecondWeaponSet = false;
    std::vector<BuildItemSlot> slots;   // only slots with an item or a known GearSlot name; PoB order
};

// Tags of one mod line (the {...} prefixes of PoB item text).
enum ModTag : uint32_t {
    kModCrafted = 1u << 0,
    kModRune = 1u << 1,        // a line a socketed rune / soul core gives
    kModEnchant = 1u << 2,
    kModDesecrated = 1u << 3,
    kModFractured = 1u << 4,
    kModMutated = 1u << 5,
    kModCustom = 1u << 6,
    kModImplicit = 1u << 7     // set on every line of BuildItem::implicits
};

struct ItemModLine {
    std::string text;        // one line, no {...} prefix, no "(implicit)" suffix (<= kMaxModBytes)
    uint32_t tags = 0;       // ModTag bits
};

struct BuildItem {
    Rarity rarity = Rarity::Normal;
    std::string name;        // rare / unique: the item's own name; normal / magic: "" (see base)
    std::string base;        // the base type ("Grand Regalia"); for a MAGIC item the whole name line
                             // ("<prefix> <base> <suffix>"): it CONTAINS the base type, never equals it
    int quality = 0;
    std::string sockets;     // PoB's "Sockets:" text ("S S"), "" none
    std::vector<std::string> runes;   // socketed rune / soul core names, "None" left out
    int levelReq = 0;        // 0 unknown
    int itemLevel = 0;       // 0 unknown
    bool corrupted = false;
    // Rune, enchant and implicit lines in PoB's order (each tagged), then the explicit mods.
    std::vector<ItemModLine> implicits, explicits;
    std::string raw;         // the PoB item text, capped at kMaxRawItemBytes (diagnostics only)

    // What to call the item in one line: its name when it has one, else the base.
    const std::string& DisplayName() const { return name.empty() ? base : name; }
};

// A decoded Path of Building build: what an import produces and what a StoredBuild keeps. Never the
// PoB code itself.
struct BuildData {
    std::string className;       // display, from <Build> ("Sorceress"), "" unknown
    std::string ascendName;      // display ("Disciple of Varashta"), "" none / unknown
    int level = 0;               // 1..100, 0 unknown
    // Path of Building's own active tree / skill set / item set (0-based, -1 unknown).
    int activeSpec = -1, activeSkillSet = -1, activeItemSet = -1;
    std::vector<BuildSpec> specs;          // PoB order (authors list leveling trees early -> late)
    std::vector<BuildSkillSet> skillSets;
    std::vector<BuildItemSet> itemSets;
    std::vector<BuildItem> items;          // referenced by index from itemSets and specs[].sockets
};

struct StoredBuild : BuildData {
    std::string id;              // "b" + 8 lower-case hex digits, unique in the store
    std::string name;            // the user's name for it (never empty)
    std::string source;          // the link the user pasted, or "PoB code" / "PoB XML"
    int64_t importedAt = 0;      // unix seconds
    // The user's pins (-1 = automatic: Loadouts.h decides).
    int selectedSpec = -1, selectedSkillSet = -1, selectedItemSet = -1;
};

} // namespace bp
