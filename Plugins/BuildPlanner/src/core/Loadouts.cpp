// BuildPlanner - the stages of a build
// Loadouts.cpp - see Loadouts.h. The links follow Path of Building's buildMode:SyncLoadouts
// (src/Modules/Build.lua); ParseLevelRange and SpecForLevel are POE1Fixer PassiveTreePlanner's.
#include "Loadouts.h"

#include <algorithm>
#include <climits>
#include <map>
#include <string_view>

#include "TreeData.h"

namespace bp {
namespace {

std::string_view TrimView(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

bool IsTagChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ','; }

// Path of Building's "%{([%w,]+)%}": the FIRST brace group made of letters, digits and commas.
// `ids` gets its comma-separated parts; `rest` the title without the group, trimmed.
bool FindTag(std::string_view title, std::vector<std::string>* ids, std::string* rest) {
    for (size_t open = title.find('{'); open != std::string_view::npos; open = title.find('{', open + 1)) {
        size_t k = open + 1;
        while (k < title.size() && IsTagChar(title[k])) ++k;
        if (k == open + 1 || k >= title.size() || title[k] != '}') continue;
        const std::string_view inner = title.substr(open + 1, k - open - 1);
        if (ids) {
            ids->clear();
            size_t pos = 0;
            while (pos <= inner.size()) {
                size_t comma = inner.find(',', pos);
                if (comma == std::string_view::npos) comma = inner.size();
                if (comma > pos) ids->emplace_back(inner.substr(pos, comma - pos));
                pos = comma + 1;
            }
            if (ids->empty()) continue;   // "{,}" names no link
        }
        if (rest) {
            std::string r(title.substr(0, open));
            r += title.substr(k + 1);
            *rest = std::string(TrimView(r));
        }
        return true;
    }
    return false;
}

// What an untitled set is called when titles are compared (Path of Building's "Default").
std::string TitleKey(std::string_view title) {
    const std::string_view t = TrimView(title);
    return t.empty() ? std::string("Default") : std::string(t);
}

// The sets of one kind (skill sets or item sets) by link id and by title.
struct SetLinks {
    size_t count = 0;
    std::map<std::string, int> byTag;     // a later set with the same id wins (Path of Building)
    std::map<std::string, int> byTitle;   // the first set with that title

    template <typename Set>
    explicit SetLinks(const std::vector<Set>& sets) : count(sets.size()) {
        std::vector<std::string> ids;
        for (size_t i = 0; i < sets.size(); ++i) {
            if (FindTag(sets[i].title, &ids, nullptr)) {
                for (const std::string& id : ids) byTag[id] = static_cast<int>(i);
            } else {
                byTitle.emplace(TitleKey(sets[i].title), static_cast<int>(i));
            }
        }
    }

    int For(const BuildSpec& spec) const {
        if (count == 1) return 0;   // a single set belongs to every stage
        std::vector<std::string> ids;
        if (FindTag(spec.title, &ids, nullptr)) {
            for (const std::string& id : ids)
                if (const auto it = byTag.find(id); it != byTag.end()) return it->second;
            return -1;
        }
        const auto it = byTitle.find(TitleKey(spec.title));
        return it == byTitle.end() ? -1 : it->second;
    }
};

bool Listed(const std::vector<uint16_t>* sorted, uint16_t id) {
    return sorted && std::binary_search(sorted->begin(), sorted->end(), id);
}

// ---- level ranges (POE1 PassiveTreePlanner) --------------------------------------------------------

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool IsAlpha(char c) {
    c = Lower(c);
    return c >= 'a' && c <= 'z';
}
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsHex(char c) {
    c = Lower(c);
    return IsDigit(c) || (c >= 'a' && c <= 'f');
}

// PoB colour codes: ^0..^9 and ^xRRGGBB. Removed so "^xFF0000Lvl 30" still starts a word at "Lvl".
std::string StripColours(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '^' && i + 1 < s.size()) {
            if (IsDigit(s[i + 1])) {
                ++i;
                continue;
            }
            if ((s[i + 1] == 'x' || s[i + 1] == 'X') && i + 8 <= s.size()) {
                bool hex = true;
                for (size_t k = i + 2; hex && k < i + 8; ++k) hex = IsHex(s[k]);
                if (hex) {
                    i += 7;
                    continue;
                }
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

bool MatchWord(std::string_view s, size_t i, std::string_view word) {
    if (s.size() - i < word.size()) return false;
    for (size_t k = 0; k < word.size(); ++k)
        if (Lower(s[i + k]) != word[k]) return false;
    return true;
}

// One or more digits at `i`; the value saturates at 1000 (clamped to 1..100 later anyway).
bool ReadNumber(std::string_view s, size_t& i, int& value) {
    if (i >= s.size() || !IsDigit(s[i])) return false;
    value = 0;
    while (i < s.size() && IsDigit(s[i])) {
        value = std::min(value * 10 + (s[i] - '0'), 1000);
        ++i;
    }
    return true;
}

size_t SkipBlanks(std::string_view s, size_t i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i;
}

// '-', '~', or the UTF-8 en / em dash a title typed outside PoB may carry. Its byte length, 0 none.
size_t DashAt(std::string_view s, size_t i) {
    if (i < s.size() && (s[i] == '-' || s[i] == '~')) return 1;
    if (i + 2 < s.size() && static_cast<uint8_t>(s[i]) == 0xE2 && static_cast<uint8_t>(s[i + 1]) == 0x80 &&
        (static_cast<uint8_t>(s[i + 2]) == 0x93 || static_cast<uint8_t>(s[i + 2]) == 0x94))
        return 3;
    return 0;
}

bool ParseLevelRangeImpl(std::string_view s, int& lo, int& hi) {
    static constexpr std::string_view kPrefixes[] = {"level", "lvl", "lv"};
    for (size_t i = 0; i < s.size(); ++i) {
        if (i > 0 && IsAlpha(s[i - 1])) continue;   // a prefix starts a word
        size_t len = 0;
        for (std::string_view p : kPrefixes)
            if (MatchWord(s, i, p)) {
                len = p.size();
                break;
            }
        if (len == 0) continue;
        size_t j = i + len;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '.')) ++j;
        int a = 0;
        if (!ReadNumber(s, j, a)) continue;
        int b = a;
        const size_t k = SkipBlanks(s, j);
        if (k < s.size() && s[k] == '+') {
            b = 100;
        } else if (const size_t dash = DashAt(s, k); dash > 0) {
            size_t m = SkipBlanks(s, k + dash);
            int v = 0;
            if (ReadNumber(s, m, v)) b = v;
        }
        a = std::clamp(a, 1, 100);
        b = std::clamp(b, 1, 100);
        if (a > b) std::swap(a, b);
        lo = a;
        hi = b;
        return true;
    }
    return false;
}

} // namespace

std::vector<Stage> BuildStages(const BuildData& build, const std::vector<uint16_t>* neverListed, const PassiveTree* tree) {
    std::vector<Stage> stages;
    try {
        const SetLinks skills(build.skillSets), items(build.itemSets);
        stages.reserve(build.specs.size());
        for (size_t i = 0; i < build.specs.size(); ++i) {
            const BuildSpec& spec = build.specs[i];
            Stage s;
            s.spec = static_cast<int>(i);
            if (!FindTag(spec.title, nullptr, &s.title)) s.title = std::string(TrimView(spec.title));
            s.hasLevels = ParseLevelRange(spec.title, s.levelLo, s.levelHi);
            if (!s.hasLevels) s.levelLo = s.levelHi = 0;
            for (const uint16_t id : spec.nodes) {
                if (Listed(neverListed, id)) continue;
                if (!tree) {
                    ++s.points;
                    continue;
                }
                // Points as the player spends them: an ascendancy node is an ascendancy
                // point, a start, a free node, a choice option or an id the data does not know none.
                const Node* node = tree->Find(id);
                if (!node || !node->CostsPoint()) continue;
                ++(node->IsAscendancy() ? s.ascPoints : s.points);
            }
            s.skillSet = skills.For(spec);
            s.itemSet = items.For(spec);
            stages.push_back(std::move(s));
        }
    } catch (...) {
        stages.clear();
    }
    return stages;
}

std::vector<uint16_t> NeverListedIds(const PassiveTree& tree, const BuildData& build) {
    std::vector<uint16_t> ids;
    try {
        if (tree.Empty()) return ids;   // no data: an id cannot be judged unknown
        ids = tree.StartNodeIds();
        for (const BuildSpec& spec : build.specs) {
            // Only a tree made for an OLDER version than the data names passives that are gone for good.
            // With the same or a newer version (or none) an id the data lacks is a passive the game may
            // well have (the data is behind): the character can list it, so the stage waits for it.
            if (spec.treeVersion.empty() || tree.treeVersion.empty() || CompareTreeVersions(spec.treeVersion, tree.treeVersion) >= 0)
                continue;
            for (const uint16_t id : spec.nodes)
                if (tree.IndexOf(id) == kNoIndex) ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    } catch (...) {
        ids.clear();
    }
    return ids;
}

bool ParseLevelRange(const std::string& title, int& lo, int& hi) {
    try {
        return ParseLevelRangeImpl(StripColours(title), lo, hi);
    } catch (...) {
        return false;
    }
}

int SpecForLevel(const std::vector<std::string>& titles, int level) {
    int best = -1, bestWidth = INT_MAX;    // the NARROWEST range that contains the level, first on ties
    int outgrown = -1;                     // the LAST title whose range ends below the level
    int lowest = -1, lowestLo = INT_MAX;   // the lowest lo, first on ties
    const size_t count = std::min(titles.size(), static_cast<size_t>(INT_MAX));
    for (size_t i = 0; i < count; ++i) {
        int lo = 0, hi = 0;
        if (!ParseLevelRange(titles[i], lo, hi)) continue;
        if (lo <= level && level <= hi && hi - lo < bestWidth) {
            bestWidth = hi - lo;
            best = static_cast<int>(i);
        }
        if (hi < level) outgrown = static_cast<int>(i);
        if (lo < lowestLo) {
            lowestLo = lo;
            lowest = static_cast<int>(i);
        }
    }
    if (best >= 0) return best;
    // None contains the level. Past a range: the title the author put after the last outgrown one (an
    // untitled "Endgame", or the upcoming range); the outgrown one itself only when nothing follows it.
    if (outgrown >= 0) return static_cast<size_t>(outgrown) + 1 < count ? outgrown + 1 : outgrown;
    // Below every range: the earliest range.
    return lowest;
}

StageChoice ChooseStage(const StoredBuild& build, const std::vector<Stage>& stages, const StageContext& ctx) {
    StageChoice c;
    try {
        const int n = static_cast<int>(std::min(stages.size(), build.specs.size()));
        if (n <= 0) return c;
        if (build.selectedSpec >= 0 && build.selectedSpec < n) return StageChoice{build.selectedSpec, StageHow::Pinned};
        if (ctx.followLevel && ctx.level >= 1 && ctx.level <= 100) {
            std::vector<std::string> titles;
            int levelTitles = 0;
            bool reached = false;   // the level is inside or past a titled range
            for (int i = 0; i < n; ++i) {
                const Stage& s = stages[static_cast<size_t>(i)];
                titles.push_back(build.specs[static_cast<size_t>(i)].title);
                if (!s.hasLevels) continue;
                ++levelTitles;
                reached = reached || s.levelLo <= ctx.level;
            }
            if (levelTitles >= 2 || (levelTitles == 1 && reached)) {
                const int s = SpecForLevel(titles, ctx.level);
                if (s >= 0 && s < n) return StageChoice{s, StageHow::ByLevel};
            }
        }
        if (ctx.allocated) {
            for (int i = 0; i < n; ++i) {
                for (const uint16_t id : build.specs[static_cast<size_t>(i)].nodes)
                    if (!Listed(ctx.neverListed, id) && !Listed(ctx.allocated, id)) return StageChoice{i, StageHow::FirstUnfinished};
            }
            return StageChoice{n - 1, StageHow::FirstUnfinished};   // every tree is finished: the last
        }
        if (build.activeSpec >= 0 && build.activeSpec < n) return StageChoice{build.activeSpec, StageHow::PobActive};
        return StageChoice{n - 1, StageHow::Last};
    } catch (...) {
        return StageChoice{};
    }
}

ActiveSets ResolveSets(const StoredBuild& build, const std::vector<Stage>& stages, int stage) {
    ActiveSets a;
    const bool valid = stage >= 0 && static_cast<size_t>(stage) < stages.size() && static_cast<size_t>(stage) < build.specs.size();
    if (valid) a.spec = stage;
    const auto pick = [&](int pinned, int linked, int active, size_t count, bool& isPinned) {
        isPinned = false;
        const auto in = [count](int v) { return v >= 0 && static_cast<size_t>(v) < count; };
        if (in(pinned)) {
            isPinned = true;
            return pinned;
        }
        if (in(linked)) return linked;
        if (in(active)) return active;
        return count > 0 ? 0 : -1;
    };
    const Stage* st = valid ? &stages[static_cast<size_t>(stage)] : nullptr;
    a.skillSet = pick(build.selectedSkillSet, st ? st->skillSet : -1, build.activeSkillSet, build.skillSets.size(), a.skillSetPinned);
    a.itemSet = pick(build.selectedItemSet, st ? st->itemSet : -1, build.activeItemSet, build.itemSets.size(), a.itemSetPinned);
    return a;
}

} // namespace bp
