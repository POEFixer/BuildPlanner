// BuildPlanner - the gem table (data/gems.json)
// GemData.cpp - see GemData.h. The file is shipped data, but it is still read defensively: a row
// that does not fit the format is skipped, never trusted half-way.
#include "GemData.h"

#include <unordered_set>

#include <nlohmann/json.hpp>

#include "JsonFile.h"

namespace bp {
namespace {

using json = nlohmann::json;

constexpr std::string_view kGemPrefix = "Metadata/Items/Gem/";
constexpr std::string_view kGemsPrefix = "Metadata/Items/Gems/";

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

std::string LowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = LowerAscii(c);
    return out;
}

// A string member of a row: false when present with another type or over the field limit.
bool Field(const json& row, const char* key, std::string& out, bool required) {
    out.clear();
    const auto it = row.find(key);
    if (it == row.end() || it->is_null()) return !required;
    if (!it->is_string()) return false;
    const std::string& s = it->get_ref<const std::string&>();
    if (s.size() > kMaxGemFieldBytes || (required && s.empty())) return false;
    out = s;
    return true;
}

Attr ColorFromWord(std::string_view w) {
    if (w == "str") return Attr::Str;
    if (w == "dex") return Attr::Dex;
    if (w == "int") return Attr::Int;
    return Attr::None;
}

bool StartsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }

} // namespace

int SplitGemTier(std::string_view name, std::string* base) {
    const auto trim = [](std::string_view t) {
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t')) t.remove_suffix(1);
        return t;
    };
    const std::string_view whole = trim(name);
    int tier = 0;
    std::string_view head = whole;
    const size_t space = whole.find_last_of(' ');
    if (space != std::string_view::npos) {
        static constexpr std::string_view kRoman[] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
        const std::string_view tail = whole.substr(space + 1);
        for (int i = 0; i < 10; ++i)
            if (tail == kRoman[i]) {
                tier = i + 1;
                head = trim(whole.substr(0, space));
                break;
            }
        if (head.empty()) {   // nothing but the numeral: not a tier
            tier = 0;
            head = whole;
        }
    }
    if (base) base->assign(head);
    return tier;
}

std::string NormalizeGemId(std::string_view gemId) {
    try {
        if (StartsWith(gemId, kGemPrefix)) return std::string(kGemsPrefix) + std::string(gemId.substr(kGemPrefix.size()));
        return std::string(gemId);
    } catch (...) {
        return {};
    }
}

bool GemData::Parse(std::string_view text, std::string* error) {
    m_gems.clear();
    m_byId.clear();
    m_byName.clear();
    m_bySkill.clear();
    m_source.clear();
    m_commit.clear();
    auto fail = [&](const char* why) {
        m_gems.clear();
        m_byId.clear();
        m_byName.clear();
        m_bySkill.clear();
        m_source.clear();
        m_commit.clear();
        if (error) *error = why;
        return false;
    };
    try {
        if (text.size() > kMaxGemFileBytes) return fail("the gem table is larger than 8 MiB");
        const json doc = json::parse(text.begin(), text.end(), nullptr, false);
        if (doc.is_discarded()) return fail("the gem table is not valid JSON");
        if (!doc.is_object()) return fail("the gem table is not a JSON object");
        if (const auto meta = doc.find("bp_meta"); meta != doc.end() && meta->is_object()) {
            std::string s;
            if (Field(*meta, "source", s, false)) m_source = s;
            if (Field(*meta, "commit", s, false)) m_commit = s;
        }
        const auto list = doc.find("gems");
        if (list == doc.end() || !list->is_array()) return fail("the gem table has no \"gems\" list");
        m_gems.reserve(std::min(list->size(), kMaxGemRows));
        for (const json& row : *list) {
            if (m_gems.size() >= kMaxGemRows) break;
            if (!row.is_object()) continue;
            GemInfo g;
            std::string color;
            if (!Field(row, "id", g.gameId, true) || !StartsWith(g.gameId, "Metadata/")) continue;
            if (!Field(row, "name", g.name, true) || !Field(row, "variant", g.variantId, false) ||
                !Field(row, "skill", g.skillId, false) || !Field(row, "family", g.family, false) ||
                !Field(row, "color", color, false))
                continue;
            const auto support = row.find("support");
            if (support != row.end() && !support->is_boolean()) continue;
            g.support = support != row.end() && support->get<bool>();
            g.activeSkills = g.support ? 0 : 1;
            if (const auto actives = row.find("actives"); actives != row.end()) {
                if (!actives->is_number_integer()) continue;
                const int64_t n = actives->get<int64_t>();
                if (n < 0 || n > kMaxGemActives) continue;
                g.activeSkills = static_cast<int>(n);
            }
            g.color = ColorFromWord(color);
            g.id = NormalizeGemId(g.gameId);
            g.tier = SplitGemTier(g.name, &g.tierBase);
            const uint32_t index = static_cast<uint32_t>(m_gems.size());
            m_byId[g.id].push_back(index);
            const std::string lower = LowerAscii(g.name);
            const auto byName = m_byName.find(lower);
            if (byName == m_byName.end()) m_byName.emplace(lower, index);
            else byName->second = kNoIndex;   // two gems of one name: a lookup by name is ambiguous
            if (!g.skillId.empty()) {
                const auto bySkill = m_bySkill.find(g.skillId);
                if (bySkill == m_bySkill.end()) m_bySkill.emplace(g.skillId, index);
                else bySkill->second = kNoIndex;
            }
            m_gems.push_back(std::move(g));
        }
        if (m_gems.empty()) return fail("the gem table holds no gem");
        // A first tier without a numeral: "Living Lightning" is tier 1 when "Living Lightning II" exists.
        std::unordered_set<std::string> secondTiers;
        for (const GemInfo& g : m_gems)
            if (g.tier == 2) secondTiers.insert(g.tierBase);
        for (GemInfo& g : m_gems)
            if (g.tier == 0 && !g.tierBase.empty() && secondTiers.count(g.tierBase) != 0) g.tier = 1;
        if (error) error->clear();
        return true;
    } catch (const std::bad_alloc&) {
        return fail("out of memory while reading the gem table");
    } catch (...) {
        return fail("the gem table could not be read");
    }
}

const GemInfo* GemData::Find(std::string_view gemId, std::string_view variantId) const {
    try {
        if (gemId.empty()) return nullptr;
        const auto it = m_byId.find(NormalizeGemId(gemId));
        if (it == m_byId.end() || it->second.empty()) return nullptr;
        if (!variantId.empty())
            for (const uint32_t i : it->second)
                if (m_gems[i].variantId == variantId) return &m_gems[i];
        return &m_gems[it->second.front()];
    } catch (...) {
        return nullptr;
    }
}

const GemInfo* GemData::FindByName(std::string_view name) const {
    try {
        if (name.empty()) return nullptr;
        const auto it = m_byName.find(LowerAscii(name));
        return (it == m_byName.end() || it->second == kNoIndex) ? nullptr : &m_gems[it->second];
    } catch (...) {
        return nullptr;
    }
}

const GemInfo* GemData::FindBySkillId(std::string_view skillId) const {
    try {
        if (skillId.empty()) return nullptr;
        const auto it = m_bySkill.find(std::string(skillId));
        return (it == m_bySkill.end() || it->second == kNoIndex) ? nullptr : &m_gems[it->second];
    } catch (...) {
        return nullptr;
    }
}

bool LoadGemFile(const std::filesystem::path& path, GemData& out, std::string* error) {
    const FileRead r = ReadFileCapped(path, kMaxGemFileBytes);
    if (!r.Ok()) {
        (void)out.Parse({}, nullptr);   // leaves the table empty
        if (error) {
            if (r.status == FileStatus::Missing) *error = "the gem table " + PathUtf8(path) + " is missing";
            else *error = "the gem table " + PathUtf8(path) + " could not be read: " + r.error;
        }
        return false;
    }
    return out.Parse(r.bytes, error);
}

bool IsSupportGem(const GemData* data, std::string_view gemId, std::string_view variantId, std::string_view skillId) {
    if (data && !data->Empty()) {
        if (const GemInfo* g = data->Find(gemId, variantId)) return g->support;
        if (const GemInfo* g = data->FindBySkillId(skillId)) return g->support;
    }
    return gemId.find("Support") != std::string_view::npos || StartsWith(skillId, "Support");
}

Attr GemColorOf(const GemData* data, std::string_view gemId, std::string_view name) {
    if (!data || data->Empty()) return Attr::None;
    if (const GemInfo* g = data->Find(gemId)) return g->color;
    if (const GemInfo* g = data->FindByName(name)) return g->color;
    return Attr::None;
}

} // namespace bp
