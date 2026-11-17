// BuildPlanner - settings.json
// Settings.cpp - see Settings.h: the JSON conversion of the settings, value by value. The file is
// untrusted: every reader checks the type and the range before it takes a value, and a value it
// refuses leaves the default in place and is named in the warnings.
#include "Settings.h"

#include <cmath>

#include <nlohmann/json.hpp>

#include "JsonFile.h"

namespace bp {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxSettingsFileBytes = 1u << 20;
constexpr size_t kMaxIdBytes = 64;            // a BuildStore id is 9 characters
constexpr size_t kMaxVersionBytes = 16;       // Settings::treeDownloadedVersion
constexpr size_t kMaxFileNameBytes = 400;     // "POEFixer - <120 characters>.build" in UTF-8

struct ModeName {
    SkillsWindowMode mode;
    const char* name;
};
constexpr ModeName kModeNames[] = {{SkillsWindowMode::Always, "always"},
                                   {SkillsWindowMode::WithSkillsPanel, "withSkillsPanel"},
                                   {SkillsWindowMode::Never, "never"}};

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void Warn(std::vector<std::string>* warnings, const char* key) {
    if (!warnings || warnings->size() >= 40) return;
    try {
        warnings->push_back(std::string("settings.json: \"") + key + "\" has a bad value, the default is used");
    } catch (...) {
    }
}

void WarnText(std::vector<std::string>* warnings, const std::string& text) {
    if (!warnings) return;
    try {
        warnings->push_back(text);
    } catch (...) {
    }
}

const json* Find(const json& j, const char* key) {
    const auto it = j.find(key);
    return it == j.end() ? nullptr : &*it;
}

void ReadBool(const json& j, const char* key, bool& v, std::vector<std::string>* w) {
    const json* p = Find(j, key);
    if (!p) return;
    if (p->is_boolean()) v = p->get<bool>();
    else Warn(w, key);
}

// Whole numbers only: 3.0 is refused like any other float.
void ReadInt(const json& j, const char* key, int& v, int lo, int hi, std::vector<std::string>* w) {
    const json* p = Find(j, key);
    if (!p) return;
    if (p->is_number_unsigned()) {
        const uint64_t u = p->get<uint64_t>();
        if (hi >= 0 && u <= static_cast<uint64_t>(hi) && (lo <= 0 || u >= static_cast<uint64_t>(lo))) {
            v = static_cast<int>(u);
            return;
        }
    } else if (p->is_number_integer()) {
        const int64_t n = p->get<int64_t>();
        if (n >= lo && n <= hi) {
            v = static_cast<int>(n);
            return;
        }
    }
    Warn(w, key);
}

void ReadFloat(const json& j, const char* key, float& v, float lo, float hi, std::vector<std::string>* w) {
    const json* p = Find(j, key);
    if (!p) return;
    if (p->is_number()) {
        const double d = p->get<double>();
        if (std::isfinite(d) && std::fabs(d) < 1e9) {
            // Compared as floats: the file's 0.3 is below the float limit 0.3f when both are doubles,
            // and a value written at a limit must read back.
            const float f = static_cast<float>(d);
            if (f >= lo && f <= hi) {
                v = f;
                return;
            }
        }
    }
    Warn(w, key);
}

// A string of at most maxBytes that `accept` takes; anything else keeps the default.
template <class Accept>
void ReadString(const json& j, const char* key, std::string& v, size_t maxBytes, std::vector<std::string>* w, Accept accept) {
    const json* p = Find(j, key);
    if (!p) return;
    if (p->is_string()) {
        const std::string& s = p->get_ref<const std::string&>();
        if (s.size() <= maxBytes && accept(s)) {
            v = s;
            return;
        }
    }
    Warn(w, key);
}

void ReadColor(const json& colors, const char* key, uint32_t& v, std::vector<std::string>* w) {
    const json* p = Find(colors, key);
    if (!p) return;
    uint32_t c = 0;
    if (p->is_string() && ColorFromHex(p->get_ref<const std::string&>(), c)) v = c;
    else Warn(w, key);
}

bool IsPrintableAscii(const std::string& s) {
    for (const char c : s)
        if (c < 0x20 || c > 0x7E) return false;
    return true;
}

bool IsVersionText(const std::string& s) {
    for (const char c : s)
        if (!((c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

// A bare file name: no folder part, no drive, no control character. (BuildFile.h checks the prefix
// and the extension again before anything is written or removed.)
bool IsBareFileName(const std::string& s) {
    for (const char c : s) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return false;
    }
    return s != "." && s != "..";
}

// A float as the file shows it: four decimals are more than any slider gives, and 0.92 is written
// as 0.92 instead of the float's 0.9200000166893005.
double Tidy(float v) { return std::round(static_cast<double>(v) * 10000.0) / 10000.0; }

json ToJson(const Settings& s) {
    json j = json::object();
    j["selectedBuild"] = s.selectedBuild;
    j["autoStageByLevel"] = s.autoStageByLevel;
    j["useTiers"] = s.useTiers;

    j["showTree"] = s.showTree;
    j["numbersShown"] = s.numbersShown;
    j["lineWidth"] = Tidy(s.lineWidth);
    j["ringScale"] = Tidy(s.ringScale);
    j["showOk"] = s.showOk;
    j["showWrong"] = s.showWrong;
    j["showAdjust"] = s.showAdjust;
    j["showLines"] = s.showLines;
    j["curvedLines"] = s.curvedLines;
    j["showSummary"] = s.showSummary;
    j["showTreeTooltip"] = s.showTreeTooltip;
    j["showAscendancy"] = s.showAscendancy;
    j["showSetBadges"] = s.showSetBadges;
    j["showAttrBadges"] = s.showAttrBadges;
    j["colors"] = {{"missing", ColorToHex(s.colorMissing)},
                   {"next", ColorToHex(s.colorNext)},
                   {"wrong", ColorToHex(s.colorWrong)},
                   {"adjust", ColorToHex(s.colorAdjust)},
                   {"ok", ColorToHex(s.colorOk)}};

    j["showGearCard"] = s.showGearCard;
    j["showSlotMarks"] = s.showSlotMarks;
    j["cardOpacity"] = Tidy(s.cardOpacity);

    j["showSkillMarks"] = s.showSkillMarks;
    j["showMissingSkills"] = s.showMissingSkills;
    j["showGrantedSkills"] = s.showGrantedSkills;
    const char* mode = kModeNames[1].name;
    for (const ModeName& m : kModeNames)
        if (m.mode == s.skillsWindowMode) mode = m.name;
    j["skillsWindowMode"] = mode;
    j["skillsWindowHotkey"] = s.skillsWindowHotkey;

    j["treeSource"] = s.treeSource;
    j["treeDownloadedVersion"] = s.treeDownloadedVersion;

    j["buildFileKeepUpdated"] = s.buildFileKeepUpdated;
    j["buildFileName"] = s.buildFileName;
    return j;
}

Settings FromJson(const json& j, std::vector<std::string>* w) {
    Settings s;
    ReadString(j, "selectedBuild", s.selectedBuild, kMaxIdBytes, w, IsPrintableAscii);
    ReadBool(j, "autoStageByLevel", s.autoStageByLevel, w);
    ReadBool(j, "useTiers", s.useTiers, w);

    ReadBool(j, "showTree", s.showTree, w);
    ReadInt(j, "numbersShown", s.numbersShown, Settings::kNumbersMin, Settings::kNumbersMax, w);
    ReadFloat(j, "lineWidth", s.lineWidth, Settings::kLineWidthMin, Settings::kLineWidthMax, w);
    ReadFloat(j, "ringScale", s.ringScale, Settings::kRingScaleMin, Settings::kRingScaleMax, w);
    ReadBool(j, "showOk", s.showOk, w);
    ReadBool(j, "showWrong", s.showWrong, w);
    ReadBool(j, "showAdjust", s.showAdjust, w);
    ReadBool(j, "showLines", s.showLines, w);
    ReadBool(j, "curvedLines", s.curvedLines, w);
    ReadBool(j, "showSummary", s.showSummary, w);
    ReadBool(j, "showTreeTooltip", s.showTreeTooltip, w);
    ReadBool(j, "showAscendancy", s.showAscendancy, w);
    ReadBool(j, "showSetBadges", s.showSetBadges, w);
    ReadBool(j, "showAttrBadges", s.showAttrBadges, w);
    if (const json* c = Find(j, "colors")) {
        if (c->is_object()) {
            ReadColor(*c, "missing", s.colorMissing, w);
            ReadColor(*c, "next", s.colorNext, w);
            ReadColor(*c, "wrong", s.colorWrong, w);
            ReadColor(*c, "adjust", s.colorAdjust, w);
            ReadColor(*c, "ok", s.colorOk, w);
        } else {
            Warn(w, "colors");
        }
    }

    ReadBool(j, "showGearCard", s.showGearCard, w);
    ReadBool(j, "showSlotMarks", s.showSlotMarks, w);
    ReadFloat(j, "cardOpacity", s.cardOpacity, Settings::kCardOpacityMin, Settings::kCardOpacityMax, w);

    ReadBool(j, "showSkillMarks", s.showSkillMarks, w);
    ReadBool(j, "showMissingSkills", s.showMissingSkills, w);
    ReadBool(j, "showGrantedSkills", s.showGrantedSkills, w);
    if (const json* m = Find(j, "skillsWindowMode")) {
        bool known = false;
        if (m->is_string()) {
            const std::string& name = m->get_ref<const std::string&>();
            for (const ModeName& row : kModeNames)
                if (name == row.name) {
                    s.skillsWindowMode = row.mode;
                    known = true;
                }
        }
        if (!known) Warn(w, "skillsWindowMode");
    }
    // A Windows virtual-key code; 0 = no hotkey.
    ReadInt(j, "skillsWindowHotkey", s.skillsWindowHotkey, 0, 255, w);

    ReadString(j, "treeSource", s.treeSource, 16, w,
               [](const std::string& v) { return v == "bundled" || v == "downloaded"; });
    ReadString(j, "treeDownloadedVersion", s.treeDownloadedVersion, kMaxVersionBytes, w, IsVersionText);

    ReadBool(j, "buildFileKeepUpdated", s.buildFileKeepUpdated, w);
    ReadString(j, "buildFileName", s.buildFileName, kMaxFileNameBytes, w, IsBareFileName);
    return s;
}

} // namespace

void ResetDisplaySettings(Settings& s) {
    Settings d;
    d.selectedBuild = std::move(s.selectedBuild);
    d.autoStageByLevel = s.autoStageByLevel;
    d.useTiers = s.useTiers;
    d.skillsWindowHotkey = s.skillsWindowHotkey;
    d.treeSource = std::move(s.treeSource);
    d.treeDownloadedVersion = std::move(s.treeDownloadedVersion);
    d.buildFileKeepUpdated = s.buildFileKeepUpdated;
    d.buildFileName = std::move(s.buildFileName);
    s = std::move(d);
}

std::string ColorToHex(uint32_t color) {
    static const char kHex[] = "0123456789ABCDEF";
    const uint32_t bytes[4] = {color & 0xFFu, (color >> 8) & 0xFFu, (color >> 16) & 0xFFu, (color >> 24) & 0xFFu};   // R G B A
    std::string s = "#";
    for (const uint32_t b : bytes) {
        s.push_back(kHex[b >> 4]);
        s.push_back(kHex[b & 0xFu]);
    }
    return s;
}

bool ColorFromHex(std::string_view text, uint32_t& out) {
    if ((text.size() != 7 && text.size() != 9) || text[0] != '#') return false;
    uint32_t bytes[4] = {0, 0, 0, 255};
    const size_t n = (text.size() - 1) / 2;
    for (size_t i = 0; i < n; ++i) {
        const int hi = HexDigit(text[1 + i * 2]), lo = HexDigit(text[2 + i * 2]);
        if (hi < 0 || lo < 0) return false;
        bytes[i] = static_cast<uint32_t>(hi * 16 + lo);
    }
    out = Rgba(bytes[0], bytes[1], bytes[2], bytes[3]);
    return true;
}

std::string SettingsToJsonText(const Settings& s) {
    try {
        // Invalid UTF-8 cannot come from FromJson, but a caller may have put anything into a string:
        // replace it instead of throwing.
        return ToJson(s).dump(2, ' ', false, json::error_handler_t::replace) + "\n";
    } catch (...) {
        return "{}\n";
    }
}

Settings SettingsFromJsonText(std::string_view json_, std::vector<std::string>* warnings) {
    try {
        const json j = json::parse(json_.begin(), json_.end(), nullptr, false);
        if (!j.is_object()) {
            WarnText(warnings, "settings.json is not a JSON object: the defaults are used");
            return Settings{};
        }
        return FromJson(j, warnings);
    } catch (...) {
        // Every reader checks the type first, so only running out of memory gets here.
        WarnText(warnings, "settings.json could not be read: the defaults are used");
        return Settings{};
    }
}

Settings LoadSettings(const std::filesystem::path& file, std::vector<std::string>* warnings) {
    try {
        const FileRead r = ReadFileCapped(file, kMaxSettingsFileBytes);
        if (r.status == FileStatus::Missing) return Settings{};
        if (!r.Ok()) {
            WarnText(warnings, r.status == FileStatus::TooLarge
                                   ? std::string("settings.json is larger than 1 MiB: the defaults are used")
                                   : "settings.json could not be read (" + r.error + "): the defaults are used");
            return Settings{};
        }
        return SettingsFromJsonText(r.bytes, warnings);
    } catch (...) {
        return Settings{};
    }
}

bool SaveSettings(const std::filesystem::path& file, const Settings& s, std::string* error) {
    if (error) error->clear();
    return WriteFileAtomic(file, SettingsToJsonText(s), error);
}

// ---------------------------------------------------------------------------------------------
// SkillsWindowState, WriteBehind
// ---------------------------------------------------------------------------------------------

bool SkillsWindowState::Update(SkillsWindowMode mode, bool skillsPanelOpen, bool hasBuild, bool toggle) {
    const bool natural = mode == SkillsWindowMode::Always || (mode == SkillsWindowMode::WithSkillsPanel && skillsPanelOpen);
    if (natural != m_natural) {
        m_natural = natural;
        m_flip = false;
    }
    m_hasBuild = hasBuild;
    if (toggle && hasBuild) m_flip = !m_flip;
    m_shown = hasBuild && (m_natural != m_flip);
    return m_shown;
}

void SkillsWindowState::Toggle() {
    if (!m_hasBuild) return;
    m_flip = !m_flip;
    m_shown = m_natural != m_flip;
}

void SkillsWindowState::Close() {
    if (m_shown) Toggle();
}

void WriteBehind::Changed(int64_t nowMs) {
    m_dirty = true;
    m_dueMs = nowMs + kSaveDebounceMs;
}

bool WriteBehind::Due(int64_t nowMs, bool force) const { return m_dirty && (force || nowMs >= m_dueMs); }

void WriteBehind::Saved() {
    m_dirty = false;
    m_error.clear();
}

void WriteBehind::Failed(int64_t nowMs, std::string why) {
    m_dueMs = nowMs + kSaveRetryMs;   // stays dirty
    m_error = why.empty() ? std::string("the file could not be written") : std::move(why);
}

} // namespace bp
