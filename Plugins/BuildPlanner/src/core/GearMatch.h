#pragma once
// BuildPlanner - equipped vs build item per slot
// GearMatch.h - the build's gear set against what the character wears: which build item
// belongs to which GearSlot, and per slot "you wear this item" or not. Pure, deterministic, no SDK,
// never throws. The slot table itself (PoB name <-> GearSlot <-> game inventory) is kGearSlots in
// Types.h.
//
// "The same item": a unique matches by its name; a rare / magic / normal build item
// matches an equipped item of the same base type. Path of Building's names are English; the game's
// are in the game language, so the comparison tries, in order: EquippedItem::englishName, then
// uniqueName / baseName (equal on an English client). Comparison is case-insensitive and ignores
// leading / trailing spaces. When neither side can be compared the result is Unknown, never a guess.
// Base types:
//   - a rare or normal build item: BuildItem::base IS the base type; it must equal the equipped base.
//     A normal item's leading "Superior " (Path of Building's word for one with quality) is not part
//     of the base type and is ignored.
//   - a MAGIC build item: BuildItem::base is the whole "<prefix> <base> <suffix>" line and never
//     equals a base type. It matches when the equipped item's English base type is CONTAINED in that
//     line as whole words ("Sapphire Ring" in "Seething Sapphire Ring of the Lost").
//   - the equipped side: EquippedItem::englishName is a unique's name or else the base type; what the
//     host answers for a RARE is not verified (it may be "" or the base). So for an equipped
//     non-unique compare its base (englishName when it is not empty and the item is not unique, else
//     baseName), and never treat englishName of a rare as an item name.
// Rings: a build ring worn in another ring slot counts as worn (swapped rings are not a difference).
//
// Threads: stateless; the App calls MatchGear on the render thread when the selected gear set or the
// equipped items (GearState::itemsSerial) change.
#include <array>
#include <string>

#include "GearReader.h"
#include "Types.h"

namespace bp {

enum class SlotMatch : uint8_t {
    NoBuildItem = 0,   // the gear set has no item for this slot: no mark, no card
    Unknown,           // the build has an item, but what is equipped is not known (not in game, panel never open)
    Match,             // the equipped item is the build's
    Differs,           // another item is equipped
    Empty              // nothing is equipped
};

struct SlotAdvice {
    int itemIndex = -1;            // index into BuildData::items, -1 = no build item
    SlotMatch match = SlotMatch::NoBuildItem;
    std::string equipped;          // display name of what is equipped ("" = nothing / unknown)
};

struct GearAdvice {
    int itemSet = -1;              // the gear set matched, -1 = the build has none (every slot NoBuildItem)
    std::array<SlotAdvice, kGearSlotCount> slots;   // index = GearSlot
    int withItem = 0;              // slots the build has an item for
    int matching = 0;              // of those, how many are Match

    const SlotAdvice& Slot(GearSlot s) const { return slots[static_cast<size_t>(s)]; }
};

// `itemSet` out of range (or -1) gives an empty advice. `gear` null or !gear->itemsKnown gives
// Unknown for every slot with a build item. Slots of the set whose PoB name has no GearSlot (limbs,
// jewels) are ignored here (the Gear tab lists them from the build itself).
GearAdvice MatchGear(const BuildData& build, int itemSet, const GearState* gear);

// Is `have` the item the build wants? See the rules above. `have.present` false -> false.
// *comparable (optional) is set to false when no name pair could be compared.
bool SameItem(const BuildItem& want, const EquippedItem& have, bool* comparable = nullptr);

// The build item of a GearSlot in a gear set: index into BuildData::items, -1 none.
int BuildItemOfSlot(const BuildData& build, int itemSet, GearSlot slot);

} // namespace bp
