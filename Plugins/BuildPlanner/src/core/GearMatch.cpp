// BuildPlanner - equipped vs build item per slot
// GearMatch.cpp - see GearMatch.h.
//
// Decisions the header leaves open:
//  - Names are compared trimmed, ASCII case-insensitively and with every run of blanks read as one
//    space (the game joins a rare's name words with two).
//  - A unique (or relic) build item never matches an equipped non-unique, and a non-unique build item
//    never matches an equipped unique: both are definite differences in any game language (the rarity
//    is read from the game, not from a name).
//  - A name pair counts as comparable when the equipped side's ENGLISH name is known. Equal localized
//    names match (an English client); different localized names say nothing (another language) and
//    give Unknown.
//  - englishName of an equipped rare that equals its own name words is ignored: it is not a base type.
//  - Rings: first every build ring is compared with its own slot; a build ring that did not match
//    there takes another ring slot whose item matches it and that no other build ring took (two
//    copies of one ring in the build need two worn copies).
//  - The first slot of the set that names a GearSlot wins (PoB writes each slot once).
#include "GearMatch.h"

#include <string>
#include <string_view>

namespace bp {
namespace {

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

// Trimmed, every run of blanks one space, ASCII lower case ("Brood  Crack" -> "brood crack": the game
// joins a rare's name words with two spaces).
std::string Fold(std::string_view s) {
    s = Trim(s);
    std::string out;
    out.reserve(s.size());
    bool blank = false;
    for (const char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            blank = true;
            continue;
        }
        if (blank) out.push_back(' ');
        blank = false;
        out.push_back(Lower(c));
    }
    return out;
}

// Equal after Fold; never for two empty strings.
bool SameText(std::string_view a, std::string_view b) {
    const std::string fa = Fold(a);
    return !fa.empty() && fa == Fold(b);
}

// `needle` occurs in `hay` as whole words ("Sapphire Ring" in "Seething Sapphire Ring of the Lost").
bool ContainsWords(std::string_view hayText, std::string_view needleText) {
    const std::string hay = Fold(hayText), needle = Fold(needleText);
    if (needle.empty() || needle.size() > hay.size()) return false;
    for (size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + 1)) {
        const size_t end = at + needle.size();
        if ((at == 0 || hay[at - 1] == ' ') && (end == hay.size() || hay[end] == ' ')) return true;
    }
    return false;
}

bool UniqueLike(Rarity r) { return r == Rarity::Unique || r == Rarity::Relic; }

bool IsRing(GearSlot s) { return s == GearSlot::Ring1 || s == GearSlot::Ring2 || s == GearSlot::Ring3; }

int SlotItem(const BuildData& build, int itemSet, GearSlot slot) {
    if (itemSet < 0 || static_cast<size_t>(itemSet) >= build.itemSets.size()) return -1;
    for (const BuildItemSlot& s : build.itemSets[static_cast<size_t>(itemSet)].slots) {
        if (GearSlotFromPobName(s.name) != static_cast<int>(slot)) continue;
        if (s.itemIndex >= 0 && static_cast<size_t>(s.itemIndex) < build.items.size()) return s.itemIndex;
        return -1;   // the first slot of that name decides, an empty one included
    }
    return -1;
}

constexpr std::string_view kSuperiorPrefix = "Superior ";

} // namespace

bool SameItem(const BuildItem& want, const EquippedItem& have, bool* comparable) {
    bool cmp = false;
    bool same = false;
    try {
        if (!have.present) {
            cmp = true;   // nothing is worn: certainly not the build's item
        } else if (UniqueLike(want.rarity) != (have.rarity == Rarity::Unique)) {
            cmp = true;   // a unique against a non-unique, either way
        } else if (UniqueLike(want.rarity)) {
            if (!Trim(want.name).empty()) {
                cmp = !Trim(have.englishName).empty();
                same = SameText(want.name, have.englishName) || SameText(want.name, have.uniqueName);
                if (same) cmp = true;
            }
        } else if (!Trim(want.base).empty()) {
            // The equipped item's English base: englishName unless it is the rare's own name words.
            std::string_view english = Trim(have.englishName);
            if (!english.empty() && SameText(english, have.uniqueName)) english = {};
            const bool magic = want.rarity == Rarity::Magic;
            // A normal item with quality is written "Superior <base>" by Path of Building: the prefix is
            // not part of the base type. Only that word is dropped (other first words name other bases).
            std::string_view wanted = Trim(want.base);
            if (want.rarity == Rarity::Normal && wanted.size() > kSuperiorPrefix.size() &&
                SameText(wanted.substr(0, kSuperiorPrefix.size()), kSuperiorPrefix))
                wanted = Trim(wanted.substr(kSuperiorPrefix.size()));
            auto matches = [&](std::string_view base) {
                return magic ? ContainsWords(want.base, base) : SameText(wanted, base);
            };
            cmp = !english.empty();
            same = (!english.empty() && matches(english)) || matches(have.baseName);
            if (same) cmp = true;
        }
    } catch (...) {
        cmp = false;
        same = false;
    }
    if (comparable) *comparable = cmp;
    return same;
}

int BuildItemOfSlot(const BuildData& build, int itemSet, GearSlot slot) {
    try {
        return SlotItem(build, itemSet, slot);
    } catch (...) {
        return -1;
    }
}

GearAdvice MatchGear(const BuildData& build, int itemSet, const GearState* gear) {
    GearAdvice a;
    try {
        if (itemSet < 0 || static_cast<size_t>(itemSet) >= build.itemSets.size()) return a;
        a.itemSet = itemSet;
        for (const GearSlotInfo& info : kGearSlots) a.slots[static_cast<size_t>(info.slot)].itemIndex = SlotItem(build, itemSet, info.slot);

        const bool known = gear && gear->itemsKnown;
        std::array<bool, kGearSlotCount> taken{};   // a worn ring already stands for a build ring
        for (const GearSlotInfo& info : kGearSlots) {
            SlotAdvice& s = a.slots[static_cast<size_t>(info.slot)];
            if (s.itemIndex < 0) continue;
            ++a.withItem;
            s.match = SlotMatch::Unknown;
            if (!known) continue;
            const GearSlotState& st = gear->Slot(info.slot);
            if (!st.known) continue;
            if (!st.item.present) {
                s.match = SlotMatch::Empty;
                continue;
            }
            s.equipped = st.item.DisplayName();
            bool cmp = false;
            if (SameItem(build.items[static_cast<size_t>(s.itemIndex)], st.item, &cmp)) {
                s.match = SlotMatch::Match;
                taken[static_cast<size_t>(info.slot)] = true;
            } else {
                s.match = cmp ? SlotMatch::Differs : SlotMatch::Unknown;
            }
        }
        // swapped rings
        if (known)
            for (const GearSlotInfo& info : kGearSlots) {
                SlotAdvice& s = a.slots[static_cast<size_t>(info.slot)];
                if (!IsRing(info.slot) || s.itemIndex < 0 || s.match == SlotMatch::Match) continue;
                for (const GearSlotInfo& other : kGearSlots) {
                    if (!IsRing(other.slot) || other.slot == info.slot || taken[static_cast<size_t>(other.slot)]) continue;
                    const GearSlotState& st = gear->Slot(other.slot);
                    if (!st.known || !st.item.present) continue;
                    if (SameItem(build.items[static_cast<size_t>(s.itemIndex)], st.item)) {
                        s.match = SlotMatch::Match;
                        taken[static_cast<size_t>(other.slot)] = true;
                        break;
                    }
                }
            }
        for (const SlotAdvice& s : a.slots)
            if (s.match == SlotMatch::Match) ++a.matching;
    } catch (...) {
        a = GearAdvice{};
    }
    return a;
}

} // namespace bp
