#pragma once
// BuildPlanner - how gear is worded. PURE: no ImGui, no plugin SDK, so that the unit tests include it
// (tests/TestGearShown.cpp); unlike the other files of src/ui it carries no test-exclusion marker.
// GearShown.h - how a build's gear is put into words: the rules the Gear tab (TabGear.cpp) and the
// build item card over the game (OverlayGear.cpp) share, so that one item reads the same in both
// places. Header-only, stateless, never throws; the texts themselves are the callers' (I18n keys).
#include <cstdint>
#include <string_view>

#include "../core/GearMatch.h"
#include "../core/Types.h"

namespace bp::shown {

// GearMatch matches a rare, magic or normal build item by its BASE TYPE (GearMatch.h): a Match then
// says "an item of the build's base type", not "the build's item". Saying "You wear this item" beside
// another name would read as a mistake, so a Match is "the same base only" when the build's item is no
// unique or relic (those match by name) and what the character wears there has another name.
// `equipped` = SlotAdvice::equipped ("" = nothing known: false). Not a Match: false.
inline bool SameBaseOnly(const BuildItem& item, SlotMatch match, std::string_view equipped) {
    if (match != SlotMatch::Match || equipped.empty()) return false;
    if (item.rarity == Rarity::Unique || item.rarity == Rarity::Relic) return false;
    return item.DisplayName() != equipped;
}

// How many sockets Path of Building's "Sockets:" text names ("S S S", "S-S", "S"): one per token,
// tokens separated by spaces, tabs or dashes. "" -> 0. The game shows a count, never PoB's notation.
inline int CountSockets(std::string_view sockets) {
    int count = 0;
    bool inToken = false;
    for (const char c : sockets) {
        const bool space = c == ' ' || c == '\t' || c == '-';
        if (!space && !inToken) ++count;
        inToken = !space;
    }
    return count;
}

// A slot of Path of Building's item set that has no equipment slot in the game (GearSlotFromPobName
// gives kNoGearSlot), so that the Gear tab names it in the program language instead of showing PoB's
// identifier (ItemsTab.lua builds these names):
//   "Jewel <node id>"           a jewel socket of the passive tree            -> TreeJewel, number = node id
//   "<slot> Jewel Socket <n>"   a jewel socketed in an item ("Gloves Jewel Socket 1", "Weapon 1 Swap
//                               Jewel Socket 2"; <slot> one of kGearSlots' PoB names) -> ItemJewel, slot, n
//   "Arm <n>", "Leg <n>"        a limb slot                                   -> Arm / Leg, number = n
// Anything else, or a number out of range (n 1..99, node id 1..65535, digits only): Raw, and the
// caller shows PoB's name as it is.
struct PobSlotName {
    enum class Kind : uint8_t { Raw = 0, TreeJewel, ItemJewel, Arm, Leg };
    Kind kind = Kind::Raw;
    GearSlot slot = GearSlot::Count;   // ItemJewel: the item's slot; else Count
    int number = 0;                    // see above; 0 with Raw
};

namespace detail {
// `text` as a whole decimal number in 1..max (digits only, no sign, no blanks); 0 when it is none.
inline int Number(std::string_view text, int max) {
    if (text.empty() || text.size() > 5) return 0;
    int n = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return 0;
        n = n * 10 + (c - '0');
    }
    return n >= 1 && n <= max ? n : 0;
}
} // namespace detail

inline PobSlotName ParsePobSlotName(std::string_view name) {
    PobSlotName out;
    constexpr std::string_view kTree = "Jewel ", kArm = "Arm ", kLeg = "Leg ", kSocket = " Jewel Socket ";
    const auto after = [&name](std::string_view prefix) {
        return name.size() > prefix.size() && name.substr(0, prefix.size()) == prefix ? name.substr(prefix.size()) : std::string_view();
    };
    if (const int id = detail::Number(after(kTree), 65535)) {
        out.kind = PobSlotName::Kind::TreeJewel;
        out.number = id;
        return out;
    }
    if (const int n = detail::Number(after(kArm), 99)) {
        out.kind = PobSlotName::Kind::Arm;
        out.number = n;
        return out;
    }
    if (const int n = detail::Number(after(kLeg), 99)) {
        out.kind = PobSlotName::Kind::Leg;
        out.number = n;
        return out;
    }
    const size_t at = name.rfind(kSocket);
    if (at != std::string_view::npos && at > 0) {
        const int slot = GearSlotFromPobName(name.substr(0, at));
        const int n = detail::Number(name.substr(at + kSocket.size()), 99);
        if (slot != kNoGearSlot && n > 0) {
            out.kind = PobSlotName::Kind::ItemJewel;
            out.slot = static_cast<GearSlot>(slot);
            out.number = n;
        }
    }
    return out;
}

} // namespace bp::shown
