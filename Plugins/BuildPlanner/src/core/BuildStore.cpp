// BuildPlanner - builds.json, the imported builds
// BuildStore.cpp - see BuildStore.h. A port of POE1Fixer PassiveTreePlanner's BuildStore.cpp, with
// the PoE2 model of Types.h (weapon sets, attribute choices, sockets, notes, skill sets, item sets,
// items). One cleaner (CleanBuild) applies every cap and repair, both to an imported build and to a
// build read from builds.json, so the two can never disagree about what a valid build is.
#include "BuildStore.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>   // MoveFileExW: a builds.json the load could not use is moved aside, never overwritten

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <iterator>
#include <map>

#include <nlohmann/json.hpp>

#include "JsonFile.h"

namespace bp {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxWarnings = 20;
constexpr size_t kMaxReadFileBytes = 16u << 20;   // builds.json as read
constexpr size_t kMaxClassBytes = 64;
constexpr size_t kMaxIdBytes = 256;               // gem / skill / variant ids
constexpr size_t kMaxVersionBytes = 16;

struct Warn {
    std::vector<std::string>* out = nullptr;
    size_t count = 0;
    void operator()(std::string text) {
        if (!out) return;
        try {
            if (count < kMaxWarnings) out->push_back(std::move(text));
            ++count;
        } catch (...) {
        }
    }
    void Finish() {
        if (!out || count <= kMaxWarnings) return;
        try {
            out->push_back(std::to_string(count - kMaxWarnings) + " more warnings not shown");
        } catch (...) {
        }
    }
};

std::string Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return std::string(s.substr(a, b - a));
}

// The length of the valid UTF-8 sequence at `i` (strict: no overlong forms, no surrogates, at most
// U+10FFFF, no NUL), 0 when the bytes there are not one.
size_t Utf8SeqLen(std::string_view s, size_t i) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    const uint8_t c = p[i];
    if (c < 0x80) return c == 0 ? 0 : 1;
    size_t len = 0;
    uint32_t cp = 0;
    if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        cp = c & 0x1Fu;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        cp = c & 0x0Fu;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        cp = c & 0x07u;
    } else {
        return 0;
    }
    if (s.size() - i < len) return 0;
    for (size_t k = 1; k < len; ++k) {
        const uint8_t cc = p[i + k];
        if ((cc & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (cc & 0x3Fu);
    }
    if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

// ---- reading JSON values --------------------------------------------------------------------------

const json* JFind(const json& j, const char* key) {
    if (!j.is_object()) return nullptr;
    const auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}

// A string member, clamped to `maxBytes` (invalid UTF-8 removed); "" when absent or not a string.
std::string Str(const json& j, const char* key, size_t maxBytes) {
    const json* p = JFind(j, key);
    if (!p || !p->is_string()) return {};
    return Utf8Clamp(p->get_ref<const std::string&>(), maxBytes);
}

bool AsInt(const json& v, long long& n) {
    if (v.is_number_unsigned()) {
        const unsigned long long u = v.get<unsigned long long>();
        if (u > 0x7FFFFFFFFFFFFFFFull) return false;
        n = static_cast<long long>(u);
        return true;
    }
    if (v.is_number_integer()) {
        n = v.get<long long>();
        return true;
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (!std::isfinite(d) || d != std::floor(d) || d < -9.0e15 || d > 9.0e15) return false;
        n = static_cast<long long>(d);
        return true;
    }
    return false;
}

// An integer member in [lo, hi], else `def`.
long long Int(const json& j, const char* key, long long def, long long lo, long long hi) {
    const json* p = JFind(j, key);
    long long n = 0;
    if (!p || !AsInt(*p, n)) return def;
    return (n >= lo && n <= hi) ? n : def;
}

bool Bool(const json& j, const char* key, bool def) {
    const json* p = JFind(j, key);
    return (p && p->is_boolean()) ? p->get<bool>() : def;
}

// The members of an array member, at most `cap` of them read (the rest are counted in *over).
template <typename F>
void Each(const json& j, const char* key, size_t cap, size_t* over, F&& f) {
    const json* p = JFind(j, key);
    if (!p || !p->is_array()) return;
    size_t n = 0;
    for (const json& v : *p) {
        if (n++ >= cap) {
            if (over) *over = p->size() - cap;
            break;
        }
        f(v);
    }
}

std::vector<uint16_t> U16List(const json& j, const char* key, size_t cap) {
    std::vector<uint16_t> out;
    Each(j, key, cap, nullptr, [&](const json& v) {
        long long n = 0;
        if (AsInt(v, n) && n >= 0 && n <= 0xFFFF) out.push_back(static_cast<uint16_t>(n));
    });
    return out;
}

// ---- the cleaner ---------------------------------------------------------------------------------

std::vector<uint16_t> SortedUnique(std::vector<uint16_t> v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

std::vector<uint16_t> Intersect(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    std::vector<uint16_t> out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

std::vector<uint16_t> Minus(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    std::vector<uint16_t> out;
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}

bool IsVersionText(std::string_view v) {
    const size_t us = v.find('_');
    if (v.empty() || v.size() > kMaxVersionBytes || us == std::string_view::npos || us == 0 || us + 1 >= v.size()) return false;
    for (size_t i = 0; i < v.size(); ++i)
        if (i != us && !(v[i] >= '0' && v[i] <= '9')) return false;
    return true;
}

bool IsAscendancyIdText(std::string_view s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

int IndexOr(int v, size_t size) { return (v >= 0 && static_cast<size_t>(v) < size) ? v : -1; }

template <typename T>
void Cap(std::vector<T>& v, size_t cap, const std::string& what, Warn& warn) {
    if (v.size() <= cap) return;
    warn(what + ": " + std::to_string(v.size() - cap) + " over the limit of " + std::to_string(cap) + " dropped");
    v.resize(cap);
}

void CleanSpec(BuildSpec& s, size_t itemCount, const std::string& where, Warn& warn) {
    s.title = Utf8Clamp(s.title, kMaxTitleBytes);
    if (!IsVersionText(s.treeVersion)) s.treeVersion.clear();
    if (s.classId < -1 || s.classId > 255) s.classId = -1;
    if (!s.ascendancyId.empty() && !IsAscendancyIdText(s.ascendancyId)) s.ascendancyId.clear();
    s.nodes = SortedUnique(std::move(s.nodes));
    Cap(s.nodes, kMaxNodes, where + " passives", warn);
    s.weaponSet1 = Intersect(SortedUnique(std::move(s.weaponSet1)), s.nodes);
    s.weaponSet2 = Minus(Intersect(SortedUnique(std::move(s.weaponSet2)), s.nodes), s.weaponSet1);
    s.attrInt = Intersect(SortedUnique(std::move(s.attrInt)), s.nodes);
    s.attrDex = Minus(Intersect(SortedUnique(std::move(s.attrDex)), s.nodes), s.attrInt);
    s.attrStr = Minus(Minus(Intersect(SortedUnique(std::move(s.attrStr)), s.nodes), s.attrInt), s.attrDex);

    std::map<uint16_t, int> sockets;
    for (const SpecSocket& k : s.sockets) {
        int idx = k.itemIndex;
        if (idx != -1 && IndexOr(idx, itemCount) < 0) {
            warn(where + ": the jewel of socket " + std::to_string(k.nodeId) + " points outside the items");
            idx = -1;
        }
        sockets[k.nodeId] = idx;
    }
    s.sockets.clear();
    for (const auto& [node, idx] : sockets) s.sockets.push_back(SpecSocket{node, idx});
    Cap(s.sockets, kMaxSpecSockets, where + " jewel sockets", warn);

    std::map<uint16_t, std::string> notes;
    for (SpecNote& n : s.notes) {
        std::string t = Trim(Utf8Clamp(n.text, kMaxNoteBytes));
        if (!t.empty()) notes[n.nodeId] = std::move(t);
    }
    s.notes.clear();
    for (auto& [node, text] : notes) s.notes.push_back(SpecNote{node, std::move(text)});
    Cap(s.notes, kMaxSpecNotes, where + " node notes", warn);
}

void CleanSkillSet(BuildSkillSet& set, const std::string& where, Warn& warn) {
    set.title = Utf8Clamp(set.title, kMaxTitleBytes);
    Cap(set.groups, kMaxGroups, where + " skill groups", warn);
    for (BuildSkillGroup& g : set.groups) {
        g.label = Utf8Clamp(g.label, kMaxNameBytes);
        if (static_cast<uint8_t>(g.source) > static_cast<uint8_t>(GroupSource::DefaultAttack)) g.source = GroupSource::Gems;
        Cap(g.gems, kMaxGems, where + " gems of a group", warn);
        std::vector<BuildGem> kept;
        int actives = 0;
        for (BuildGem& gem : g.gems) {
            gem.name = Trim(Utf8Clamp(gem.name, kMaxNameBytes));
            if (gem.name.empty()) continue;   // a gem is never nameless
            gem.gemId = Utf8Clamp(gem.gemId, kMaxIdBytes);
            gem.skillId = Utf8Clamp(gem.skillId, kMaxIdBytes);
            gem.variantId = Utf8Clamp(gem.variantId, kMaxIdBytes);
            gem.level = std::clamp(gem.level, 0, 100);
            gem.quality = std::clamp(gem.quality, 0, 100);
            if (!gem.support) ++actives;
            kept.push_back(std::move(gem));
        }
        g.gems = std::move(kept);
        if (g.mainActive < 0 || g.mainActive >= actives) g.mainActive = 0;
    }
}

void CleanItem(BuildItem& it) {
    if (static_cast<uint8_t>(it.rarity) > static_cast<uint8_t>(Rarity::Relic)) it.rarity = Rarity::Normal;
    it.name = Utf8Clamp(it.name, kMaxNameBytes);
    it.base = Utf8Clamp(it.base, kMaxNameBytes);
    it.quality = std::clamp(it.quality, 0, 100);
    it.sockets = Utf8Clamp(it.sockets, kMaxNameBytes);
    if (it.runes.size() > kMaxRunes) it.runes.resize(kMaxRunes);
    for (std::string& r : it.runes) r = Utf8Clamp(r, kMaxNameBytes);
    it.runes.erase(std::remove(it.runes.begin(), it.runes.end(), std::string()), it.runes.end());
    it.levelReq = std::clamp(it.levelReq, 0, 100);
    it.itemLevel = std::clamp(it.itemLevel, 0, 100);
    constexpr uint32_t kKnownTags = kModCrafted | kModRune | kModEnchant | kModDesecrated | kModFractured | kModMutated | kModCustom | kModImplicit;
    for (int list = 0; list < 2; ++list) {
        std::vector<ItemModLine>& mods = list == 0 ? it.implicits : it.explicits;
        if (mods.size() > kMaxModLines) mods.resize(kMaxModLines);
        std::vector<ItemModLine> kept;
        for (ItemModLine& m : mods) {
            m.text = Utf8Clamp(m.text, kMaxModBytes);
            if (m.text.empty()) continue;
            m.tags &= kKnownTags;
            if (list == 0) m.tags |= kModImplicit;
            else m.tags &= ~static_cast<uint32_t>(kModImplicit);
            kept.push_back(std::move(m));
        }
        mods = std::move(kept);
    }
    it.raw = Utf8Clamp(it.raw, kMaxRawItemBytes);
}

void CleanItemSet(BuildItemSet& set, size_t itemCount, const std::string& where, Warn& warn) {
    set.title = Utf8Clamp(set.title, kMaxTitleBytes);
    std::vector<BuildItemSlot> kept;
    for (BuildItemSlot& slot : set.slots) {
        slot.name = Utf8Clamp(slot.name, kMaxNameBytes);
        if (slot.name.empty()) continue;
        if (slot.itemIndex != -1 && IndexOr(slot.itemIndex, itemCount) < 0) {
            warn(where + ": the item of the slot " + slot.name + " points outside the items");
            slot.itemIndex = -1;
        }
        if (slot.itemIndex < 0 && GearSlotFromPobName(slot.name) == kNoGearSlot) continue;
        if (std::any_of(kept.begin(), kept.end(), [&](const BuildItemSlot& k) { return k.name == slot.name; })) continue;
        kept.push_back(std::move(slot));
    }
    set.slots = std::move(kept);
    Cap(set.slots, kMaxSetSlots, where + " slots", warn);
}

// Every cap and repair of BuildData (Types.h). Items first: the trees and sets refer to them.
void CleanData(BuildData& b, const std::string& where, Warn& warn) {
    b.className = Utf8Clamp(Trim(b.className), kMaxClassBytes);
    b.ascendName = Utf8Clamp(Trim(b.ascendName), kMaxClassBytes);
    if (b.ascendName == "None") b.ascendName.clear();
    if (b.level < 0 || b.level > 100) b.level = 0;
    Cap(b.items, kMaxItems, where + " items", warn);
    for (BuildItem& it : b.items) CleanItem(it);
    Cap(b.specs, kMaxSpecs, where + " trees", warn);
    for (size_t i = 0; i < b.specs.size(); ++i) CleanSpec(b.specs[i], b.items.size(), where + " tree " + std::to_string(i + 1), warn);
    Cap(b.skillSets, kMaxSkillSets, where + " skill sets", warn);
    for (size_t i = 0; i < b.skillSets.size(); ++i) CleanSkillSet(b.skillSets[i], where + " skill set " + std::to_string(i + 1), warn);
    Cap(b.itemSets, kMaxItemSets, where + " item sets", warn);
    for (size_t i = 0; i < b.itemSets.size(); ++i)
        CleanItemSet(b.itemSets[i], b.items.size(), where + " item set " + std::to_string(i + 1), warn);
    b.activeSpec = IndexOr(b.activeSpec, b.specs.size());
    b.activeSkillSet = IndexOr(b.activeSkillSet, b.skillSets.size());
    b.activeItemSet = IndexOr(b.activeItemSet, b.itemSets.size());
}

uint64_t SplitMix(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// ---- JSON <-> model ----------------------------------------------------------------------------

constexpr const char* kRarityNames[] = {"normal", "magic", "rare", "unique", "relic"};
constexpr const char* kSourceNames[] = {"gems", "item", "tree", "defaultAttack"};

json SpecJson(const BuildSpec& s) {
    json sockets = json::array(), notes = json::array();
    for (const SpecSocket& k : s.sockets) sockets.push_back({{"nodeId", k.nodeId}, {"itemIndex", k.itemIndex}});
    for (const SpecNote& n : s.notes) notes.push_back({{"nodeId", n.nodeId}, {"text", n.text}});
    return {{"title", s.title},           {"treeVersion", s.treeVersion}, {"classId", s.classId}, {"ascendancyId", s.ascendancyId},
            {"nodes", s.nodes},           {"weaponSet1", s.weaponSet1},   {"weaponSet2", s.weaponSet2},
            {"attrStr", s.attrStr},       {"attrDex", s.attrDex},         {"attrInt", s.attrInt},
            {"sockets", std::move(sockets)}, {"notes", std::move(notes)}};
}

json ModsJson(const std::vector<ItemModLine>& mods) {
    json a = json::array();
    for (const ItemModLine& m : mods) a.push_back({{"text", m.text}, {"tags", m.tags}});
    return a;
}

json BuildJson(const StoredBuild& b) {
    json specs = json::array(), skillSets = json::array(), itemSets = json::array(), items = json::array();
    for (const BuildSpec& s : b.specs) specs.push_back(SpecJson(s));
    for (const BuildSkillSet& set : b.skillSets) {
        json groups = json::array();
        for (const BuildSkillGroup& g : set.groups) {
            json gems = json::array();
            for (const BuildGem& gem : g.gems)
                gems.push_back({{"name", gem.name},
                                {"gemId", gem.gemId},
                                {"skillId", gem.skillId},
                                {"variantId", gem.variantId},
                                {"level", gem.level},
                                {"quality", gem.quality},
                                {"enabled", gem.enabled},
                                {"support", gem.support}});
            groups.push_back({{"label", g.label},
                              {"enabled", g.enabled},
                              {"mainActive", g.mainActive},
                              {"source", kSourceNames[std::min<size_t>(static_cast<size_t>(g.source), 3)]},
                              {"gems", std::move(gems)}});
        }
        skillSets.push_back({{"title", set.title}, {"groups", std::move(groups)}});
    }
    for (const BuildItemSet& set : b.itemSets) {
        json slots = json::array();
        for (const BuildItemSlot& s : set.slots) slots.push_back({{"name", s.name}, {"itemIndex", s.itemIndex}});
        itemSets.push_back({{"title", set.title}, {"useSecondWeaponSet", set.useSecondWeaponSet}, {"slots", std::move(slots)}});
    }
    for (const BuildItem& it : b.items)
        items.push_back({{"rarity", kRarityNames[std::min<size_t>(static_cast<size_t>(it.rarity), 4)]},
                         {"name", it.name},
                         {"base", it.base},
                         {"quality", it.quality},
                         {"sockets", it.sockets},
                         {"runes", it.runes},
                         {"levelReq", it.levelReq},
                         {"itemLevel", it.itemLevel},
                         {"corrupted", it.corrupted},
                         {"implicits", ModsJson(it.implicits)},
                         {"explicits", ModsJson(it.explicits)},
                         {"raw", it.raw}});
    return {{"id", b.id},
            {"name", b.name},
            {"source", b.source},
            {"importedAt", b.importedAt},
            {"className", b.className},
            {"ascendName", b.ascendName},
            {"level", b.level},
            {"activeSpec", b.activeSpec},
            {"activeSkillSet", b.activeSkillSet},
            {"activeItemSet", b.activeItemSet},
            {"selectedSpec", b.selectedSpec},
            {"selectedSkillSet", b.selectedSkillSet},
            {"selectedItemSet", b.selectedItemSet},
            {"specs", std::move(specs)},
            {"skillSets", std::move(skillSets)},
            {"itemSets", std::move(itemSets)},
            {"items", std::move(items)}};
}

json StoreJson(const std::vector<StoredBuild>& builds) {
    json arr = json::array();
    for (const StoredBuild& b : builds) arr.push_back(BuildJson(b));
    return json{{"version", 1}, {"builds", std::move(arr)}};
}

std::vector<ItemModLine> ModsFrom(const json& o, const char* key, size_t& over) {
    std::vector<ItemModLine> mods;
    Each(o, key, kMaxModLines, &over, [&](const json& m) {
        if (!m.is_object()) return;
        ItemModLine line;
        line.text = Str(m, "text", kMaxModBytes);
        line.tags = static_cast<uint32_t>(Int(m, "tags", 0, 0, 0xFFFFFFFFll));
        mods.push_back(std::move(line));
    });
    return mods;
}

// One build of builds.json, before the cleaner. Lists are read up to a little over their cap, so
// a huge array costs nothing; what is past the cap is counted for the warning.
StoredBuild BuildFrom(const json& o, const std::string& where, Warn& warn) {
    StoredBuild b;
    size_t over = 0;
    const auto overflow = [&](const char* what) {
        if (over) warn(where + ": " + std::to_string(over) + " " + what + " over the limit dropped");
        over = 0;
    };
    b.name = Trim(Str(o, "name", kMaxNameBytes));
    b.source = Str(o, "source", kMaxSourceBytes);
    b.importedAt = Int(o, "importedAt", 0, 0, 0x7FFFFFFFFFFFll);
    b.className = Str(o, "className", kMaxClassBytes);
    b.ascendName = Str(o, "ascendName", kMaxClassBytes);
    b.level = static_cast<int>(Int(o, "level", 0, 0, 100));
    b.activeSpec = static_cast<int>(Int(o, "activeSpec", -1, -1, INT_MAX));
    b.activeSkillSet = static_cast<int>(Int(o, "activeSkillSet", -1, -1, INT_MAX));
    b.activeItemSet = static_cast<int>(Int(o, "activeItemSet", -1, -1, INT_MAX));
    b.selectedSpec = static_cast<int>(Int(o, "selectedSpec", -1, -1, INT_MAX));
    b.selectedSkillSet = static_cast<int>(Int(o, "selectedSkillSet", -1, -1, INT_MAX));
    b.selectedItemSet = static_cast<int>(Int(o, "selectedItemSet", -1, -1, INT_MAX));

    Each(o, "items", kMaxItems, &over, [&](const json& io) {
        BuildItem it;
        if (io.is_object()) {
            const std::string r = Str(io, "rarity", 16);
            for (size_t k = 0; k < std::size(kRarityNames); ++k)
                if (r == kRarityNames[k]) it.rarity = static_cast<Rarity>(k);
            it.name = Str(io, "name", kMaxNameBytes);
            it.base = Str(io, "base", kMaxNameBytes);
            it.quality = static_cast<int>(Int(io, "quality", 0, 0, 100));
            it.sockets = Str(io, "sockets", kMaxNameBytes);
            Each(io, "runes", kMaxRunes, nullptr, [&](const json& r2) {
                if (r2.is_string()) it.runes.push_back(Utf8Clamp(r2.get_ref<const std::string&>(), kMaxNameBytes));
            });
            it.levelReq = static_cast<int>(Int(io, "levelReq", 0, 0, 100));
            it.itemLevel = static_cast<int>(Int(io, "itemLevel", 0, 0, 100));
            it.corrupted = Bool(io, "corrupted", false);
            size_t modsOver = 0;
            it.implicits = ModsFrom(io, "implicits", modsOver);
            it.explicits = ModsFrom(io, "explicits", modsOver);
            it.raw = Str(io, "raw", kMaxRawItemBytes);
        }
        b.items.push_back(std::move(it));   // a damaged item keeps its place: sets refer to it by index
    });
    overflow("items");

    Each(o, "specs", kMaxSpecs, &over, [&](const json& so) {
        if (!so.is_object()) return;
        BuildSpec s;
        s.title = Str(so, "title", kMaxTitleBytes);
        s.treeVersion = Str(so, "treeVersion", kMaxVersionBytes);
        s.classId = static_cast<int>(Int(so, "classId", -1, -1, 255));
        s.ascendancyId = Str(so, "ascendancyId", 32);
        s.nodes = U16List(so, "nodes", kMaxNodes * 2);
        s.weaponSet1 = U16List(so, "weaponSet1", kMaxNodes * 2);
        s.weaponSet2 = U16List(so, "weaponSet2", kMaxNodes * 2);
        s.attrStr = U16List(so, "attrStr", kMaxNodes * 2);
        s.attrDex = U16List(so, "attrDex", kMaxNodes * 2);
        s.attrInt = U16List(so, "attrInt", kMaxNodes * 2);
        Each(so, "sockets", kMaxSpecSockets * 2, nullptr, [&](const json& k) {
            const long long node = Int(k, "nodeId", -1, 0, 0xFFFF);
            if (node >= 0) s.sockets.push_back(SpecSocket{static_cast<uint16_t>(node), static_cast<int>(Int(k, "itemIndex", -1, -1, INT_MAX))});
        });
        Each(so, "notes", kMaxSpecNotes * 2, nullptr, [&](const json& n) {
            const long long node = Int(n, "nodeId", -1, 0, 0xFFFF);
            if (node >= 0) s.notes.push_back(SpecNote{static_cast<uint16_t>(node), Str(n, "text", kMaxNoteBytes)});
        });
        b.specs.push_back(std::move(s));
    });
    overflow("trees");

    Each(o, "skillSets", kMaxSkillSets, &over, [&](const json& setJ) {
        BuildSkillSet set;
        if (setJ.is_object()) {
            set.title = Str(setJ, "title", kMaxTitleBytes);
            Each(setJ, "groups", kMaxGroups, nullptr, [&](const json& gj) {
                if (!gj.is_object()) return;
                BuildSkillGroup g;
                g.label = Str(gj, "label", kMaxNameBytes);
                g.enabled = Bool(gj, "enabled", true);
                g.mainActive = static_cast<int>(Int(gj, "mainActive", 0, 0, INT_MAX));
                const std::string src = Str(gj, "source", 32);
                for (size_t k = 0; k < std::size(kSourceNames); ++k)
                    if (src == kSourceNames[k]) g.source = static_cast<GroupSource>(k);
                Each(gj, "gems", kMaxGems, nullptr, [&](const json& gem) {
                    if (!gem.is_object()) return;
                    BuildGem x;
                    x.name = Str(gem, "name", kMaxNameBytes);
                    x.gemId = Str(gem, "gemId", kMaxIdBytes);
                    x.skillId = Str(gem, "skillId", kMaxIdBytes);
                    x.variantId = Str(gem, "variantId", kMaxIdBytes);
                    x.level = static_cast<int>(Int(gem, "level", 0, 0, 100));
                    x.quality = static_cast<int>(Int(gem, "quality", 0, 0, 100));
                    x.enabled = Bool(gem, "enabled", true);
                    x.support = Bool(gem, "support", false);
                    g.gems.push_back(std::move(x));
                });
                set.groups.push_back(std::move(g));
            });
        }
        b.skillSets.push_back(std::move(set));   // keeps its place: the user's pin is an index
    });
    overflow("skill sets");

    Each(o, "itemSets", kMaxItemSets, &over, [&](const json& setJ) {
        BuildItemSet set;
        if (setJ.is_object()) {
            set.title = Str(setJ, "title", kMaxTitleBytes);
            set.useSecondWeaponSet = Bool(setJ, "useSecondWeaponSet", false);
            Each(setJ, "slots", kMaxSetSlots, nullptr, [&](const json& sj) {
                BuildItemSlot slot;
                slot.name = Str(sj, "name", kMaxNameBytes);
                slot.itemIndex = static_cast<int>(Int(sj, "itemIndex", -1, -1, INT_MAX));
                set.slots.push_back(std::move(slot));
            });
        }
        b.itemSets.push_back(std::move(set));
    });
    overflow("item sets");
    return b;
}

} // namespace

std::string Utf8Clamp(std::string_view text, size_t maxBytes) {
    try {
        std::string out;
        out.reserve(std::min(text.size(), maxBytes));
        size_t i = 0;
        while (i < text.size()) {
            const size_t len = Utf8SeqLen(text, i);
            if (len == 0) {   // an invalid byte: removed
                ++i;
                continue;
            }
            if (out.size() + len > maxBytes) break;
            out.append(text.data() + i, len);
            i += len;
        }
        return out;
    } catch (...) {
        return {};
    }
}

std::string Utf8Cut(std::string_view text, size_t maxChars) {
    try {
        size_t i = 0, chars = 0;
        while (i < text.size() && chars < maxChars) {
            const size_t len = Utf8SeqLen(text, i);
            i += len == 0 ? 1 : len;
            ++chars;
        }
        std::string out = Utf8Clamp(text.substr(0, i), i);
        if (i < text.size()) out += "...";
        return out;
    } catch (...) {
        return {};
    }
}

BuildStore::BuildStore() {
    m_rng = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
            static_cast<uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()) ^ reinterpret_cast<uintptr_t>(this);
}

bool BuildStore::ValidId(std::string_view id) {
    if (id.size() != 9 || id[0] != 'b') return false;
    for (size_t i = 1; i < id.size(); ++i) {
        const char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

std::string BuildStore::NewId() {
    static const char kHex[] = "0123456789abcdef";
    const auto make = [](uint32_t v) {
        std::string id = "b";
        for (int shift = 28; shift >= 0; shift -= 4) id.push_back(kHex[(v >> shift) & 0xFu]);
        return id;
    };
    for (int attempt = 0; attempt < 10000; ++attempt) {
        std::string id = make(static_cast<uint32_t>(SplitMix(m_rng) >> 32));
        if (!Find(id)) return id;
    }
    // Practically unreachable (at most kMaxBuilds ids in a space of 2^32): count upwards instead.
    for (uint32_t v = 0;; ++v) {
        std::string id = make(v);
        if (!Find(id)) return id;
    }
}

StoredBuild* BuildStore::Find(std::string_view id) {
    if (id.empty()) return nullptr;
    for (StoredBuild& b : builds)
        if (b.id == id) return &b;
    return nullptr;
}

const StoredBuild* BuildStore::Find(std::string_view id) const {
    if (id.empty()) return nullptr;
    for (const StoredBuild& b : builds)
        if (b.id == id) return &b;
    return nullptr;
}

int BuildStore::IndexOf(std::string_view id) const {
    if (id.empty()) return -1;
    for (size_t i = 0; i < builds.size(); ++i)
        if (builds[i].id == id) return static_cast<int>(i);
    return -1;
}

std::string BuildStore::DefaultName(const BuildData& build, const std::string& source) {
    try {
        std::string name = Trim(build.className);
        if (!name.empty()) {
            const std::string asc = Trim(build.ascendName);
            if (!asc.empty() && asc != "None") name += " " + asc;
            if (build.level > 0 && build.level <= 100) name += " L" + std::to_string(build.level);
            return Utf8Clamp(name, kMaxNameBytes);
        }
        const std::string src = Trim(source);
        return src.empty() ? std::string("Build") : Utf8Cut(src, 80);
    } catch (...) {
        return "Build";
    }
}

std::string BuildStore::Add(const BuildData& build, const std::string& source, int64_t importedAt, std::vector<std::string>* warnings,
                            const std::string& name) {
    Warn warn;
    warn.out = warnings;
    try {
        if (builds.size() >= kMaxBuilds) {
            warn("the build list is full (" + std::to_string(kMaxBuilds) + " builds): delete one first");
            return {};
        }
        if (build.specs.empty()) {
            warn("the build has no passive tree");
            return {};
        }
        StoredBuild b;
        static_cast<BuildData&>(b) = build;
        const std::string trimmed = Trim(name);
        b.id = NewId();
        b.source = Utf8Clamp(source, kMaxSourceBytes);
        b.name = trimmed.empty() ? DefaultName(build, b.source) : Utf8Clamp(trimmed, kMaxNameBytes);
        if (b.name.empty()) b.name = "Build";
        CleanData(b, b.name, warn);
        b.importedAt = importedAt > 0 ? importedAt : 0;
        b.selectedSpec = b.selectedSkillSet = b.selectedItemSet = -1;
        builds.push_back(std::move(b));
        warn.Finish();
        return builds.back().id;
    } catch (...) {
        warn("the build could not be stored (out of memory?)");
        return {};
    }
}

std::string BuildStore::AddWithinLimit(const BuildData& build, const std::string& source, int64_t importedAt,
                                       std::vector<std::string>* warnings, bool* tooLarge, size_t maxFileBytes) {
    if (tooLarge) *tooLarge = false;
    std::string id;
    try {
        id = Add(build, source, importedAt, warnings);
        if (id.empty()) return {};
        if (FileBytes() <= maxFileBytes) return id;
        Remove(id);
        if (tooLarge) *tooLarge = true;
        if (warnings) warnings->push_back("builds.json would grow over " + std::to_string(maxFileBytes) + " bytes with this build: not added");
        return {};
    } catch (...) {
        if (!id.empty()) Remove(id);
        return {};
    }
}

size_t BuildStore::FileBytes() const noexcept {
    try {
        return StoreJson(builds).dump(2, ' ', false, json::error_handler_t::replace).size() + 1;
    } catch (...) {
        return SIZE_MAX;
    }
}

std::string BuildStore::ToJsonText() const {
    try {
        return StoreJson(builds).dump(2, ' ', false, json::error_handler_t::replace) + "\n";
    } catch (...) {
        return {};
    }
}

bool BuildStore::FromJsonText(std::string_view text, std::vector<std::string>* warnings) {
    StoreFileVerdict verdict = StoreFileVerdict::Accepted;
    return Parse(text, warnings, verdict);
}

bool BuildStore::Parse(std::string_view text, std::vector<std::string>* warnings, StoreFileVerdict& verdict) {
    builds.clear();
    verdict = StoreFileVerdict::Accepted;
    Warn warn;
    warn.out = warnings;
    try {
        const json j = json::parse(text.begin(), text.end(), nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            verdict = StoreFileVerdict::Unusable;
            warn(std::string("builds.json is not ") + (j.is_discarded() ? "valid JSON" : "a JSON object") + ": no builds loaded");
            warn.Finish();
            return false;
        }
        if (const long long v = Int(j, "version", 1, LLONG_MIN, LLONG_MAX); v > kStoreFileVersion) {
            verdict = StoreFileVerdict::Newer;
            warn("builds.json has version " + std::to_string(v) + ", written by a newer Build Planner: no builds loaded");
            warn.Finish();
            return false;
        } else if (v != kStoreFileVersion) {
            warn("builds.json has version " + std::to_string(v) + "; read as version " + std::to_string(kStoreFileVersion));
        }
        const json* list = JFind(j, "builds");
        if (!list) return true;
        if (!list->is_array()) {
            verdict = StoreFileVerdict::Unusable;
            warn("builds.json: \"builds\" is not a list: no builds loaded");
            warn.Finish();
            return true;
        }
        size_t index = 0;
        for (const json& o : *list) {
            ++index;
            if (builds.size() >= kMaxBuilds) {
                warn("builds.json: " + std::to_string(list->size() - (index - 1)) + " builds over the limit of " + std::to_string(kMaxBuilds) +
                     " dropped");
                break;
            }
            const std::string where = "builds.json build " + std::to_string(index);
            if (!o.is_object()) {
                warn(where + " is not an object: dropped");
                continue;
            }
            StoredBuild b = BuildFrom(o, where, warn);
            if (b.specs.empty()) {
                warn(where + " has no passive tree: dropped");
                continue;
            }
            const int selSpec = b.selectedSpec, selSkill = b.selectedSkillSet, selItem = b.selectedItemSet;
            const int actSpec = b.activeSpec, actSkill = b.activeSkillSet, actItem = b.activeItemSet;
            CleanData(b, where, warn);
            b.selectedSpec = IndexOr(selSpec, b.specs.size());
            b.selectedSkillSet = IndexOr(selSkill, b.skillSets.size());
            b.selectedItemSet = IndexOr(selItem, b.itemSets.size());
            if (selSpec != b.selectedSpec || selSkill != b.selectedSkillSet || selItem != b.selectedItemSet || actSpec != b.activeSpec ||
                actSkill != b.activeSkillSet || actItem != b.activeItemSet)
                warn(where + ": a tree or set index outside its list was reset");
            const std::string id = Str(o, "id", 16);
            if (ValidId(id) && !Find(id)) {
                b.id = id;
            } else {
                b.id = NewId();
                warn(where + ": " + (id.empty() ? std::string("no id") : "the id \"" + id + "\" is invalid or taken") + ", given " + b.id);
            }
            if (b.name.empty()) b.name = DefaultName(b, b.source);
            builds.push_back(std::move(b));
        }
        warn.Finish();
        return true;
    } catch (...) {
        verdict = StoreFileVerdict::Unusable;
        warn("builds.json could not be read completely");
        warn.Finish();
        return true;
    }
}

bool BuildStore::Remove(std::string_view id) {
    for (auto it = builds.begin(); it != builds.end(); ++it)
        if (it->id == id) {
            builds.erase(it);
            return true;
        }
    return false;
}

bool BuildStore::Rename(std::string_view id, std::string_view name) {
    StoredBuild* b = Find(id);
    if (!b) return false;
    try {
        std::string n = Trim(name);
        if (n.empty() || n.size() > kMaxNameBytes || Utf8Clamp(n, kMaxNameBytes) != n) return false;
        b->name = std::move(n);
        return true;
    } catch (...) {
        return false;
    }
}

void LoadBuildStore(const std::filesystem::path& file, BuildStore& store, std::vector<std::string>* warnings) {
    store.builds.clear();
    store.m_fileVerdict = StoreFileVerdict::Accepted;
    const auto note = [&](const std::string& text) {
        if (!warnings) return;
        try {
            warnings->push_back(text);
        } catch (...) {
        }
    };
    // A file another program holds for a moment (a scanner, a backup or sync tool) is not a bad file:
    // wait for it before giving up, or the next save would put a good builds.json aside.
    FileRead r = ReadFileCapped(file, kMaxReadFileBytes);
    for (int attempt = 0; attempt < kLoadRetries && r.status == FileStatus::IoError; ++attempt) {
        Sleep(kLoadRetryMs);
        r = ReadFileCapped(file, kMaxReadFileBytes);
    }
    if (r.status == FileStatus::Missing) return;
    StoreFileVerdict verdict = StoreFileVerdict::Unusable;
    if (r.Ok()) {
        store.Parse(r.bytes, warnings, verdict);
    } else {
        note("builds.json could not be read (" + r.error + "): no builds loaded");
    }
    store.m_fileVerdict = verdict;
    if (verdict == StoreFileVerdict::Accepted) return;
    try {
        std::filesystem::path aside = file;
        aside += verdict == StoreFileVerdict::Newer ? L".newer" : L".corrupt";
        note(PathUtf8(file) + " is kept: it is moved to " + PathUtf8(aside.filename()) +
             " in the same folder before the plugin saves its builds again");
    } catch (...) {
    }
}

bool SaveBuildStore(const std::filesystem::path& file, const BuildStore& store, std::string* error) {
    const std::string text = store.ToJsonText();
    if (text.empty()) {
        if (error) *error = "out of memory while writing builds.json";
        return false;
    }
    if (store.m_fileVerdict != StoreFileVerdict::Accepted) {
        // The file the load could not use goes aside first; without that, nothing is written.
        try {
            const bool newer = store.m_fileVerdict == StoreFileVerdict::Newer;
            std::filesystem::path aside = file;
            aside += newer ? L".newer" : L".corrupt";
            const auto refuse = [&](DWORD code) {
                if (error)
                    *error = PathUtf8(file) + (newer ? " was written by a newer Build Planner" : " could not be read when the plugin started") +
                             " and could not be moved to " + PathUtf8(aside.filename()) + " (Windows error " + std::to_string(code) +
                             "), so it was not overwritten. Close any program that has it open, or move the file out of that folder "
                             "yourself, then import or change a build again to save.";
                return false;
            };
            const auto gone = [](DWORD code) { return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND; };
            const DWORD attrs = GetFileAttributesW(file.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                const DWORD e = GetLastError();
                if (!gone(e)) return refuse(e);
                store.m_fileVerdict = StoreFileVerdict::Accepted;   // nothing left to keep
            } else if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                // A copy set aside earlier is kept for one more generation ("<aside>.old"): two bad loads
                // in a row never cost the user the first file. Best effort: when that older copy cannot
                // be moved (its ".old" name is a folder, or held by another program) the save goes on
                // and the newer file takes its name, as it must not be refused for an old copy's sake.
                const DWORD asideAttrs = GetFileAttributesW(aside.c_str());
                if (asideAttrs != INVALID_FILE_ATTRIBUTES && (asideAttrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    std::filesystem::path older = aside;
                    older += L".old";
                    MoveFileExW(aside.c_str(), older.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                }
                if (!MoveFileExW(file.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    const DWORD e = GetLastError();
                    if (!gone(e)) return refuse(e);
                }
                store.m_fileVerdict = StoreFileVerdict::Accepted;   // kept as `aside` (or gone meanwhile)
            }
            // A folder of that name: WriteFileAtomic refuses it on its own and the verdict stays.
        } catch (...) {
            if (error) *error = "builds.json was not saved: the file the plugin could not read could not be moved aside";
            return false;
        }
    }
    return WriteFileAtomic(file, text, error);
}

} // namespace bp
