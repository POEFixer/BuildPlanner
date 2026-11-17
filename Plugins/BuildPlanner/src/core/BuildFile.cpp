// BuildPlanner - the game's own Build Planner .build file
// BuildFile.cpp - see BuildFile.h. The JSON shape follows GGG's Build Planner documentation
// (pathofexile.com/developer/docs/game, "Build Planner") and Path of Building (PoE2)'s writer
// (src/Modules/BuildExportPoE2.lua).
//
// Decisions the header leaves open:
//  - Keys in the order of GGG's example: name, author, ascendancy, passives, skills, inventory_slots;
//    "skills" and "inventory_slots" are always arrays (PoB forces them, the loader wants [] not {}),
//    "passives" too whenever the stage has a tree and tree data is loaded.
//  - Every passive is an object (also without weapon_set / additional_text), in passive-id order; a
//    variant id and its base id are one passive.
//  - "+5 to any Attribute" nodes for which the build picks an attribute get that choice as
//    additional_text ("<red>{+5 to Strength}", green Dexterity, blue Intelligence), after the author's
//    note when there is one: the game's planner shows nothing else that tells the choice.
//  - Item text as PoB's ItemAdditionalText: a bold rarity-coloured header (rare / unique: name, then
//    base on its own line), an empty line, then enchant, rune, implicit and explicit lines, each in
//    PoB's colour for its kind (<rgb(R, G, B)>{...}). Braces are removed from every text we write,
//    notes included: the markup uses them as delimiters.
//  - Skill and support ids are written as the build has them (both "Gem/" and "Gems/" load).
#include "BuildFile.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <climits>
#include <system_error>

#include <nlohmann/json.hpp>

#include "GearMatch.h"
#include "JsonFile.h"
#include "SkillMatch.h"

namespace bp {
namespace {

using ojson = nlohmann::ordered_json;

constexpr size_t kMaxBuildFileBytes = 4u << 20;
constexpr size_t kMaxNameCodePoints = 120;

std::string NoBraces(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s)
        if (c != '{' && c != '}') out.push_back(c);
    return out;
}

std::string Rgb(uint32_t rgb) {
    return "<rgb(" + std::to_string((rgb >> 16) & 0xFF) + ", " + std::to_string((rgb >> 8) & 0xFF) + ", " +
           std::to_string(rgb & 0xFF) + ")>";
}

// PoB's colorCodes (src/Data/Global.lua).
uint32_t RarityColour(Rarity r) {
    switch (r) {
        case Rarity::Magic: return 0x8888FF;
        case Rarity::Rare: return 0xFFFF77;
        case Rarity::Unique: return 0xAF6025;
        case Rarity::Relic: return 0x60C060;
        case Rarity::Normal:
        default: return 0xC8C8C8;
    }
}

uint32_t ModColour(uint32_t tags) {
    if (tags & (kModRune | kModEnchant | kModCrafted)) return 0xB8DAF1;   // ENCHANTED / CRAFTED
    if (tags & kModCustom) return 0x5CF0BB;
    if (tags & kModFractured) return 0xA29160;
    if (tags & kModMutated) return 0xAE2E3B;
    return 0x8888FF;                                                     // MAGIC
}

std::string ItemText(const BuildItem& item) {
    std::string title;
    const bool named = (item.rarity == Rarity::Rare || item.rarity == Rarity::Unique || item.rarity == Rarity::Relic) &&
                       !item.name.empty();
    if (named) title = item.name + (item.base.empty() ? std::string() : "\n" + item.base);
    else title = item.DisplayName();
    std::string text = Rgb(RarityColour(item.rarity)) + "{<b>{" + NoBraces(title) + "}}\n";
    auto add = [&text](const ItemModLine& line) {
        if (line.text.empty()) return;
        text += "\n" + Rgb(ModColour(line.tags)) + "{" + NoBraces(line.text) + "}";
    };
    for (const ItemModLine& l : item.implicits)
        if ((l.tags & kModEnchant) && !(l.tags & kModRune)) add(l);
    for (const ItemModLine& l : item.implicits)
        if (l.tags & kModRune) add(l);
    for (const ItemModLine& l : item.implicits)
        if (!(l.tags & (kModEnchant | kModRune))) add(l);
    for (const ItemModLine& l : item.explicits) add(l);
    return text;
}

std::string AttrHint(Attr a) {
    switch (a) {
        case Attr::Str: return "<red>{+5 to Strength}";
        case Attr::Dex: return "<green>{+5 to Dexterity}";
        case Attr::Int: return "<blue>{+5 to Intelligence}";
        case Attr::None:
        default: return {};
    }
}

bool Listed(const PassiveTree& tree, const std::vector<uint16_t>& ids, uint32_t node) {
    for (const uint16_t id : ids)
        if (tree.IndexOf(id) == node) return true;
    return false;
}

ojson Passives(const BuildSpec& spec, const PassiveTree& tree) {
    ojson out = ojson::array();
    std::vector<uint32_t> done;
    std::vector<uint16_t> ids = spec.nodes;
    std::sort(ids.begin(), ids.end());
    for (const uint16_t id : ids) {
        const uint32_t i = tree.IndexOf(id);
        if (i >= tree.nodes.size() || std::find(done.begin(), done.end(), i) != done.end()) continue;
        const Node& node = tree.nodes[i];
        if (!node.Allocatable() || node.stringId.empty()) continue;
        done.push_back(i);
        ojson e;
        e["id"] = node.stringId;
        if (Listed(tree, spec.weaponSet1, i)) e["weapon_set"] = 1;
        else if (Listed(tree, spec.weaponSet2, i)) e["weapon_set"] = 2;
        std::string text;
        for (const SpecNote& n : spec.notes)
            if (tree.IndexOf(n.nodeId) == i && !n.text.empty()) {
                text = NoBraces(n.text);
                break;
            }
        if (node.Is(kNodeAttribute)) {
            const Attr a = Listed(tree, spec.attrStr, i) ? Attr::Str
                         : Listed(tree, spec.attrDex, i) ? Attr::Dex
                         : Listed(tree, spec.attrInt, i) ? Attr::Int : Attr::None;
            const std::string hint = AttrHint(a);
            if (!hint.empty()) text = text.empty() ? hint : text + "\n" + hint;
        }
        if (!text.empty()) e["additional_text"] = text;
        out.push_back(std::move(e));
    }
    return out;
}

ojson Skills(const StoredBuild& build, int skillSet) {
    ojson out = ojson::array();
    const SkillAdvice adv = MatchSkills(build, skillSet, nullptr, nullptr, false);
    if (adv.skillSet < 0) return out;
    const BuildSkillSet& set = build.skillSets[static_cast<size_t>(adv.skillSet)];
    for (const GroupAdvice& ga : adv.groups) {
        const BuildSkillGroup& g = set.groups[static_cast<size_t>(ga.group)];
        const BuildGem& main = g.gems[static_cast<size_t>(ga.mainGem)];
        if (main.gemId.empty()) continue;
        ojson e;
        e["id"] = main.gemId;
        if (main.level > 0)
            e["additional_text"] = "Level " + std::to_string(main.level) +
                                   (main.quality > 0 ? ", " + std::to_string(main.quality) + "% Quality" : std::string());
        ojson supports = ojson::array();
        for (const SupportAdvice& s : ga.supports) {
            const BuildGem& gem = g.gems[static_cast<size_t>(s.gem)];
            if (!gem.gemId.empty()) supports.push_back(gem.gemId);
        }
        if (!supports.empty()) e["support_skills"] = std::move(supports);
        out.push_back(std::move(e));
    }
    return out;
}

ojson Slots(const StoredBuild& build, int itemSet) {
    ojson out = ojson::array();
    for (const GearSlotInfo& info : kGearSlots) {
        const int idx = BuildItemOfSlot(build, itemSet, info.slot);
        if (idx < 0 || static_cast<size_t>(idx) >= build.items.size()) continue;
        const BuildItem& item = build.items[static_cast<size_t>(idx)];
        ojson e;
        e["inventory_id"] = info.inventoryId;
        if (info.cellX != 0) e["slot_x"] = info.cellX;
        if (item.rarity == Rarity::Unique && !item.name.empty()) e["unique_name"] = NoBraces(item.name);
        e["additional_text"] = ItemText(item);
        out.push_back(std::move(e));
    }
    return out;
}

// UTF-8 -> UTF-16; false for invalid UTF-8.
bool Widen(std::string_view s, std::wstring& out) {
    out.clear();
    if (s.empty()) return true;
    if (s.size() > static_cast<size_t>(INT_MAX)) return false;
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), out.data(), n) == n;
}

// A file name the plugin may write or remove: our prefix and extension, something between them, and
// nothing that leaves the folder or that Windows refuses.
bool OurFileName(std::string_view name, std::wstring& wide) {
    if (name.size() <= kBuildFilePrefix.size() + kBuildFileExt.size() || name.size() > 1024) return false;
    if (name.substr(0, kBuildFilePrefix.size()) != kBuildFilePrefix) return false;
    if (name.substr(name.size() - kBuildFileExt.size()) != kBuildFileExt) return false;
    for (const char c : name)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos)
            return false;   // no separator: the name cannot leave the folder
    return Widen(name, wide);
}

bool Exists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

// Appends one decoded code point to `out` as UTF-8.
void PutUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Decodes the code point at s[i] (strict UTF-8); 0xFFFFFFFF and one byte consumed when invalid.
uint32_t NextCodePoint(std::string_view s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t len = 0;
    uint32_t cp = 0, min = 0;
    if (c < 0x80) {
        ++i;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1F;
        min = 0x80;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0F;
        min = 0x800;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07;
        min = 0x10000;
    } else {
        ++i;
        return 0xFFFFFFFFu;
    }
    if (i + len > s.size()) {
        ++i;
        return 0xFFFFFFFFu;
    }
    for (size_t k = 1; k < len; ++k) {
        const unsigned char b = static_cast<unsigned char>(s[i + k]);
        if ((b & 0xC0) != 0x80) {
            ++i;
            return 0xFFFFFFFFu;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        ++i;
        return 0xFFFFFFFFu;
    }
    i += len;
    return cp;
}

} // namespace

std::string BuildFileName(std::string_view buildName) {
    std::string name;
    try {
        size_t count = 0;
        for (size_t i = 0; i < buildName.size() && count < kMaxNameCodePoints;) {
            const uint32_t cp = NextCodePoint(buildName, i);
            if (cp == 0xFFFFFFFFu) {
                name.push_back('_');
            } else if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) {
                continue;   // control characters are dropped
            } else if (cp < 0x80 && std::string_view("<>:\"/\\|?*").find(static_cast<char>(cp)) != std::string_view::npos) {
                name.push_back('_');
            } else {
                PutUtf8(name, cp);
            }
            ++count;
        }
        const size_t first = name.find_first_not_of(' ');
        name = first == std::string::npos ? std::string() : name.substr(first);
        while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    } catch (...) {
        name.clear();
    }
    if (name.empty()) name = "Build";
    return std::string(kBuildFilePrefix) + name + std::string(kBuildFileExt);
}

std::string BuildFileJson(const StoredBuild& build, const ActiveSets& sets, const PassiveTree* tree,
                          std::string_view stageTitle) {
    try {
        const BuildSpec* spec = (sets.spec >= 0 && static_cast<size_t>(sets.spec) < build.specs.size())
                                    ? &build.specs[static_cast<size_t>(sets.spec)] : nullptr;
        ojson skills = Skills(build, sets.skillSet);
        ojson slots = Slots(build, sets.itemSet);
        if (!spec && skills.empty() && slots.empty()) return {};
        ojson root;
        std::string name = build.name.empty() ? std::string("Build") : build.name;
        if (!stageTitle.empty()) name += " - " + std::string(stageTitle);
        root["name"] = name;
        root["author"] = kBuildFileAuthor;
        if (spec && !spec->ascendancyId.empty()) root["ascendancy"] = spec->ascendancyId;
        if (spec && tree && !tree->Empty()) root["passives"] = Passives(*spec, *tree);
        root["skills"] = std::move(skills);
        root["inventory_slots"] = std::move(slots);
        return root.dump(2, ' ', false, ojson::error_handler_t::replace) + "\n";
    } catch (...) {
        return {};
    }
}

std::filesystem::path BuildPlannerFolder() {
    try {
        PWSTR docs = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &docs);
        std::filesystem::path out;
        if (SUCCEEDED(hr) && docs && docs[0] != L'\0')
            out = std::filesystem::path(docs) / L"My Games" / L"Path of Exile 2" / L"BuildPlanner";
        if (docs) CoTaskMemFree(docs);
        return out;
    } catch (...) {
        return {};
    }
}

bool IsOurBuildFile(const std::filesystem::path& file) {
    try {
        std::wstring unused;
        if (!OurFileName(PathUtf8(file.filename()), unused)) return false;
        const FileRead f = ReadFileCapped(file, kMaxBuildFileBytes);
        if (!f.Ok()) return false;
        const nlohmann::json d = nlohmann::json::parse(f.bytes, nullptr, false);
        if (d.is_discarded() || !d.is_object()) return false;
        const auto a = d.find("author");
        return a != d.end() && a->is_string() && a->get_ref<const std::string&>() == kBuildFileAuthor;
    } catch (...) {
        return false;
    }
}

BuildFileResult WriteBuildFile(const std::filesystem::path& folder, std::string_view fileName, std::string_view json,
                               std::string_view previousFileName) {
    BuildFileResult r;
    try {
        std::wstring wide;
        if (folder.empty()) {
            r.error = "the game's Build Planner folder is not known (Documents could not be resolved)";
            return r;
        }
        if (!OurFileName(fileName, wide)) {
            r.error = "refused to write \"" + std::string(fileName) + "\": not a POEFixer build file name";
            return r;
        }
        if (json.empty()) {
            r.error = "there is nothing to write for this build";
            return r;
        }
        const std::filesystem::path target = folder / wide;
        if (Exists(target) && !IsOurBuildFile(target)) {
            r.error = "\"" + std::string(fileName) + "\" exists and was not written by POEFixer; it was left alone";
            return r;
        }
        std::string err;
        if (!WriteFileAtomic(target, json, &err)) {
            r.error = "writing " + PathUtf8(target) + " failed: " + err;
            return r;
        }
        r.ok = true;
        r.file = target;
        std::wstring previousWide;
        if (!previousFileName.empty() && previousFileName != fileName && OurFileName(previousFileName, previousWide)) {
            const std::filesystem::path previous = folder / previousWide;
            std::error_code ec;
            const bool sameFile = std::filesystem::equivalent(previous, target, ec);   // a rename that only changed case
            if (!ec && !sameFile && IsOurBuildFile(previous)) RemoveFile(previous, nullptr);
        }
        return r;
    } catch (...) {
        BuildFileResult fail;
        fail.error = "writing the build file failed";
        return fail;
    }
}

BuildFileResult RemoveBuildFile(const std::filesystem::path& folder, std::string_view fileName) {
    BuildFileResult r;
    try {
        std::wstring wide;
        if (folder.empty()) {
            r.error = "the game's Build Planner folder is not known (Documents could not be resolved)";
            return r;
        }
        if (!OurFileName(fileName, wide)) {
            r.error = "refused to remove \"" + std::string(fileName) + "\": not a POEFixer build file name";
            return r;
        }
        const std::filesystem::path target = folder / wide;
        if (!Exists(target)) {
            r.ok = true;
            return r;
        }
        if (!IsOurBuildFile(target)) {
            r.error = "\"" + std::string(fileName) + "\" was not written by POEFixer; it was left alone";
            return r;
        }
        std::string err;
        if (!RemoveFile(target, &err)) {
            r.error = "removing " + PathUtf8(target) + " failed: " + err;
            return r;
        }
        r.ok = true;
        r.file = target;
        return r;
    } catch (...) {
        BuildFileResult fail;
        fail.error = "removing the build file failed";
        return fail;
    }
}

} // namespace bp
