// BuildPlanner - Path of Building item text
// PobItem.cpp - see PobItem.h. A port of the parts of Path of Building's Item:ParseRaw
// (src/Classes/Item.lua) that do not need its base and mod tables: the header lines, the name / base
// lines, the variant selection and the mod lines with their {...} tags.
#include "PobItem.h"

#include <algorithm>
#include <array>
#include <vector>

#include "BuildStore.h"   // Utf8Clamp

namespace bp {
namespace {

constexpr size_t kMaxLines = 4096;      // lines read from one item text
constexpr size_t kMaxVariants = 256;    // "Variant:" lines counted

bool IsBlank(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

std::string_view Trim(std::string_view s) {
    while (!s.empty() && IsBlank(s.front())) s.remove_prefix(1);
    while (!s.empty() && IsBlank(s.back())) s.remove_suffix(1);
    return s;
}

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
bool IsAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool EqualsI(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}

bool StartsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }

// The first decimal number in `s` (an optional leading '+' or '-' is skipped), saturating at 100000;
// -1 when there is none.
int FirstNumber(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && !IsDigit(s[i])) ++i;
    if (i >= s.size()) return -1;
    int v = 0;
    while (i < s.size() && IsDigit(s[i])) {
        v = std::min(v * 10 + (s[i] - '0'), 100000);
        ++i;
    }
    return v;
}

int InRange(int v, int lo, int hi) { return (v >= lo && v <= hi) ? v : 0; }

// Path of Building's parseItemSpec: "Name: value" where the name holds letters, spaces and
// parentheses (optionally ending in ':'), or "Requires <Word> value".
bool SplitSpec(std::string_view line, std::string_view& name, std::string_view& value) {
    const size_t colon = line.find(": ");
    if (colon != std::string_view::npos && colon > 0) {
        bool ok = true;
        for (size_t i = 0; i < colon && ok; ++i) {
            const char c = line[i];
            ok = IsAlpha(c) || c == ' ' || c == '(' || c == ')' || (c == ':' && i + 1 == colon);
        }
        if (ok && colon + 2 < line.size()) {
            name = line.substr(0, colon);
            if (!name.empty() && name.back() == ':') name.remove_suffix(1);
            value = Trim(line.substr(colon + 2));
            if (!value.empty()) return true;
        }
    }
    if (StartsWith(line, "Requires ")) {
        const size_t word = line.find(' ', 9);
        if (word != std::string_view::npos && word > 9) {
            bool ok = true;
            for (size_t i = 9; i < word && ok; ++i) ok = IsAlpha(line[i]);
            if (ok) {
                name = line.substr(0, word);
                value = Trim(line.substr(word + 1));
                return !value.empty();
            }
        }
    }
    return false;
}

// "1,2,3" -> the ids listed (positive, at most 3 digits each).
std::vector<int> IdList(std::string_view s) {
    std::vector<int> out;
    size_t i = 0;
    while (i < s.size() && out.size() < 64) {
        while (i < s.size() && !IsDigit(s[i])) ++i;
        int v = 0, digits = 0;
        while (i < s.size() && IsDigit(s[i])) {
            if (digits < 4) v = v * 10 + (s[i] - '0');
            ++digits;
            ++i;
        }
        if (digits > 0 && digits <= 3) out.push_back(v);
    }
    return out;
}

enum : uint32_t { kFlagImplicitMarker = 1u << 31 };

uint32_t FlagOf(std::string_view word) {
    if (word == "crafted") return kModCrafted;
    if (word == "rune") return kModRune;
    if (word == "enchant") return kModEnchant;
    if (word == "desecrated") return kModDesecrated;
    if (word == "fractured") return kModFractured;
    if (word == "mutated") return kModMutated;
    if (word == "custom") return kModCustom;
    if (word == "implicit") return kFlagImplicitMarker;
    return 0;
}

// The words of Path of Building's lineFlags that carry no tag here: removed, nothing else.
bool IsQuietFlag(std::string_view word) {
    return word == "disabled" || word == "unscalable" || word == "prefix" || word == "suffix";
}

struct ModDraft {
    std::string text;
    uint32_t flags = 0;               // ModTag bits + kFlagImplicitMarker
    bool hasVariants = false;
    std::vector<int> variants;
};

// Strips the leading {...} groups and the trailing " (flag)" markers of one mod line.
ModDraft ReadModLine(std::string_view line) {
    ModDraft d;
    for (int guard = 0; guard < 32 && !line.empty() && line.front() == '{'; ++guard) {
        const size_t close = line.find('}');
        if (close == std::string_view::npos) break;
        const std::string_view inner = line.substr(1, close - 1);
        const size_t colon = inner.find(':');
        const std::string_view key = inner.substr(0, colon);
        bool letters = true;
        for (const char c : key) letters = letters && IsAlpha(c);
        if (!letters) break;   // not a tag: the line starts with literal braces
        const std::string_view value = colon == std::string_view::npos ? std::string_view() : inner.substr(colon + 1);
        if (key == "variant") {
            d.hasVariants = true;
            d.variants = IdList(value);
        } else {
            d.flags |= FlagOf(key);   // range, tags, version, base, group, ... and unknown keys: dropped
        }
        line.remove_prefix(close + 1);
    }
    line = Trim(line);
    for (int guard = 0; guard < 8; ++guard) {
        if (line.size() < 4 || line.back() != ')') break;
        const size_t open = line.rfind(" (");
        if (open == std::string_view::npos) break;
        const std::string_view word = line.substr(open + 2, line.size() - open - 3);
        const uint32_t f = FlagOf(word);
        if (f == 0 && !IsQuietFlag(word)) break;
        d.flags |= f;
        line = Trim(line.substr(0, open));
    }
    d.text = Utf8Clamp(line, kMaxModBytes);
    return d;
}

bool ListsAny(const std::vector<int>& ids, int a) { return a > 0 && std::find(ids.begin(), ids.end(), a) != ids.end(); }

} // namespace

bool RarityFromPob(std::string_view word, Rarity& out) {
    word = Trim(word);
    if (EqualsI(word, "NORMAL")) out = Rarity::Normal;
    else if (EqualsI(word, "MAGIC")) out = Rarity::Magic;
    else if (EqualsI(word, "RARE")) out = Rarity::Rare;
    else if (EqualsI(word, "UNIQUE")) out = Rarity::Unique;
    else if (EqualsI(word, "RELIC")) out = Rarity::Relic;
    else return false;
    return true;
}

bool ParsePobItem(std::string_view text, BuildItem& out) { return ParsePobItem(text, out, PobItemVariants{}); }

bool ParsePobItem(std::string_view text, BuildItem& out, const PobItemVariants& fromXml) {
    out = BuildItem{};
    try {
        std::vector<std::string_view> lines;
        for (size_t pos = 0; pos < text.size() && lines.size() < kMaxLines;) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string_view::npos) nl = text.size();
            const std::string_view line = Trim(text.substr(pos, nl - pos));
            if (!line.empty()) lines.push_back(line);
            pos = nl + 1;
        }
        BuildItem item;
        size_t l = 0;
        if (l < lines.size() && StartsWith(lines[l], "Item Class:")) ++l;
        bool haveRarity = false;
        item.rarity = Rarity::Unique;   // Path of Building's default for text without a rarity line
        if (l < lines.size() && StartsWith(lines[l], "Rarity:")) {
            Rarity r = Rarity::Normal;
            if (RarityFromPob(lines[l].substr(7), r)) item.rarity = r;
            haveRarity = true;
            ++l;
        }
        if (item.rarity == Rarity::Unique)
            for (const std::string_view line : lines)
                if (line.find("Foil Unique") != std::string_view::npos) item.rarity = Rarity::Relic;
        while (l < lines.size() && lines[l] == "--------") ++l;
        const bool haveName = l < lines.size();
        if (!haveRarity && !haveName) return false;
        if (haveName) {
            if (item.rarity == Rarity::Normal || item.rarity == Rarity::Magic) {
                item.base = Utf8Clamp(lines[l++], kMaxNameBytes);
            } else {
                item.name = Utf8Clamp(lines[l++], kMaxNameBytes);
                if (l < lines.size() && lines[l] != "--------") item.base = Utf8Clamp(lines[l++], kMaxNameBytes);
            }
        }

        // Variant selection first: the mod lines below are filtered by it.
        int variantCount = 0;
        int selected = 0;
        std::array<int, 5> alt = {0, 0, 0, 0, 0};
        std::array<bool, 5> hasAlt = {false, false, false, false, false};
        static constexpr std::string_view kAltNames[5] = {"Alt Variant", "Alt Variant Two", "Alt Variant Three", "Alt Variant Four",
                                                          "Alt Variant Five"};
        for (size_t k = l; k < lines.size(); ++k) {
            std::string_view name, value;
            if (!SplitSpec(lines[k], name, value)) continue;
            if (name == "Variant" && variantCount < static_cast<int>(kMaxVariants)) ++variantCount;
            else if (name == "Selected Variant") selected = std::max(FirstNumber(value), 0);
            for (size_t a = 0; a < 5; ++a) {
                if (name == std::string("Has ") + std::string(kAltNames[a])) hasAlt[a] = value == "true";
                else if (name == std::string("Selected ") + std::string(kAltNames[a])) alt[a] = std::max(FirstNumber(value), 0);
            }
        }
        if (selected == 0) selected = fromXml.variant > 0 ? fromXml.variant : variantCount;
        if (variantCount > 0) selected = std::clamp(selected, 1, variantCount);
        for (size_t a = 0; a < 5; ++a) {
            if (fromXml.alt[a] > 0) hasAlt[a] = true;
            if (!hasAlt[a]) {
                alt[a] = 0;
                continue;
            }
            if (alt[a] == 0) alt[a] = fromXml.alt[a] > 0 ? fromXml.alt[a] : variantCount;
            if (variantCount > 0) alt[a] = std::clamp(alt[a], 1, variantCount);
        }
        const auto keep = [&](const ModDraft& d) {
            if (!d.hasVariants) return true;
            if (ListsAny(d.variants, selected)) return true;
            for (size_t a = 0; a < 5; ++a)
                if (hasAlt[a] && ListsAny(d.variants, alt[a])) return true;
            return false;
        };

        bool modSection = false;   // after "Implicits: N" every line is a mod line
        int implicitLines = 0, implicitSeen = 0;
        bool inReminder = false;
        for (; l < lines.size(); ++l) {
            const std::string_view line = lines[l];
            if (inReminder) {
                if (line.back() == ')') inReminder = false;
                continue;
            }
            if (line == "--------") continue;
            if (line == "Corrupted" || line == "Twice Corrupted") {
                item.corrupted = true;
                continue;
            }
            if (line == "Mirrored" || line == "Sanctified" || line == "Unidentified" || line == "Desecrated Prefix" ||
                line == "Desecrated Suffix" || line == "Requirements:" || line == "Prefixes:" || line == "Suffixes:" ||
                line == "Foil Unique")
                continue;
            if (line.size() >= 2 && line[0] == '(' && IsAlpha(line[1])) {   // reminder text, maybe over several lines
                inReminder = line.back() != ')';
                continue;
            }
            if (StartsWith(line, "{ ")) continue;   // the game's advanced-copy headers
            std::string_view name, value;
            const bool spec = SplitSpec(line, name, value);
            if (spec && (name == "Requires Class" || name == "Class")) continue;
            if (!modSection) {
                if (StartsWith(line, "Requires Level ") || StartsWith(line, "Requires: Level ")) {
                    item.levelReq = InRange(FirstNumber(line), 0, 100);
                    continue;
                }
                if (spec) {
                    if (name == "Implicits") {
                        implicitLines = std::clamp(FirstNumber(value), 0, 1000);
                        modSection = true;
                    } else if (name == "Quality") {
                        item.quality = InRange(FirstNumber(value), 0, 100);
                    } else if (name == "Sockets") {
                        item.sockets = Utf8Clamp(value, kMaxNameBytes);
                    } else if (name == "Rune") {
                        if (value != "None" && item.runes.size() < kMaxRunes) item.runes.push_back(Utf8Clamp(value, kMaxNameBytes));
                    } else if (name == "LevelReq" || name == "Requires Level") {
                        item.levelReq = InRange(FirstNumber(value), 0, 100);
                    } else if (name == "Item Level") {
                        item.itemLevel = InRange(FirstNumber(value), 0, 100);
                    }
                    continue;   // every other "Name: value" line before the mods is a header
                }
            }
            const ModDraft d = ReadModLine(line);
            if (d.text.empty()) continue;
            const uint32_t tags = d.flags & ~static_cast<uint32_t>(kFlagImplicitMarker);
            const bool implicit =
                (d.flags & (kModRune | kModEnchant | kFlagImplicitMarker)) != 0 || (modSection && implicitSeen < implicitLines);
            if (implicit) ++implicitSeen;
            if (!keep(d)) continue;
            std::vector<ItemModLine>& list = implicit ? item.implicits : item.explicits;
            if (list.size() >= kMaxModLines) continue;
            list.push_back(ItemModLine{d.text, tags | (implicit ? static_cast<uint32_t>(kModImplicit) : 0u)});
        }
        item.raw = Utf8Clamp(Trim(text), kMaxRawItemBytes);
        out = std::move(item);
        return true;
    } catch (...) {
        out = BuildItem{};
        return false;
    }
}

} // namespace bp
