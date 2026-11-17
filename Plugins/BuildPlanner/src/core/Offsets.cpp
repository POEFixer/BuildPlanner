// BuildPlanner - the private game offsets
// Offsets.cpp - see Offsets.h: the field table, the JSON overlay and the three-layer load.
#include "Offsets.h"

#include <cstdio>

#include <nlohmann/json.hpp>

#include "JsonFile.h"

namespace bp {
namespace {

using json = nlohmann::json;

constexpr size_t kMaxOffsetsFileBytes = 256u << 10;

const OffsetField kFields[] = {
#define BP_OFFSET_ROW(name, value, group, comment) {#name, &Offsets::name, static_cast<uint32_t>(value), group, comment},
    BP_OFFSET_FIELDS(BP_OFFSET_ROW)
#undef BP_OFFSET_ROW
};

// A "0x..." hex string (1..8 digits), a decimal string, or a non-negative integer up to 0xFFFFFFFF.
bool OffsetValue(const json& v, uint32_t& out) {
    if (v.is_number_unsigned()) {
        const uint64_t n = v.get<uint64_t>();
        if (n > 0xFFFFFFFFull) return false;
        out = static_cast<uint32_t>(n);
        return true;
    }
    if (v.is_number_integer()) {
        const int64_t n = v.get<int64_t>();
        if (n < 0 || n > 0xFFFFFFFFll) return false;
        out = static_cast<uint32_t>(n);
        return true;
    }
    if (!v.is_string()) return false;
    const std::string& s = v.get_ref<const std::string&>();
    size_t i = 0;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        i = 2;
        base = 16;
    }
    if (i >= s.size()) return false;
    uint64_t n = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        n = n * static_cast<uint64_t>(base) + static_cast<uint64_t>(digit);
        if (n > 0xFFFFFFFFull) return false;
    }
    out = static_cast<uint32_t>(n);
    return true;
}

void Warn(std::vector<std::string>* warnings, std::string text) {
    if (warnings) warnings->push_back(std::move(text));
}

// Applies one file as a layer. Returns true when the file was a JSON object.
bool ApplyFile(const std::filesystem::path& file, const char* label, OffsetSource source, OffsetsInfo& info) {
    const FileRead r = ReadFileCapped(file, kMaxOffsetsFileBytes);
    if (r.status == FileStatus::Missing) return false;
    if (!r.Ok()) {
        info.warnings.push_back(std::string("offsets: ") + label + " could not be read: " + r.error);
        return false;
    }
    std::vector<uint8_t> touched;
    std::vector<std::string> warnings;
    if (!ParseOffsets(r.bytes, info.values, &warnings, &touched)) {
        info.warnings.push_back(std::string("offsets: ") + label + " is not a JSON object; ignored");
        return false;
    }
    for (std::string& w : warnings) info.warnings.push_back(std::string(label) + ": " + w);
    for (size_t i = 0; i < touched.size() && i < info.source.size(); ++i)
        if (touched[i]) info.source[i] = source;
    return true;
}

} // namespace

std::span<const OffsetField> OffsetFields() { return std::span<const OffsetField>(kFields); }

std::string OffsetHex(uint32_t value) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", value);
    return buf;
}

bool ParseOffsets(std::string_view jsonText, Offsets& o, std::vector<std::string>* warnings, std::vector<uint8_t>* touched) {
    try {
        const json d = json::parse(jsonText.begin(), jsonText.end(), nullptr, false);
        if (d.is_discarded() || !d.is_object()) return false;
        Offsets next = o;
        std::vector<uint8_t> set(std::size(kFields), 0);
        for (auto it = d.begin(); it != d.end(); ++it) {
            const std::string& key = it.key();
            if (!key.empty() && key[0] == '_') continue;   // a comment
            size_t index = std::size(kFields);
            for (size_t i = 0; i < std::size(kFields); ++i)
                if (key == kFields[i].name) {
                    index = i;
                    break;
                }
            if (index == std::size(kFields)) {
                Warn(warnings, "offsets: unknown key \"" + key + "\" ignored");
                continue;
            }
            uint32_t v = 0;
            if (!OffsetValue(it.value(), v)) {
                Warn(warnings, "offsets: \"" + key + "\" needs a \"0x...\" hex string or a non-negative integer up to 0xFFFFFFFF; kept " +
                                   OffsetHex(next.*(kFields[index].member)));
                continue;
            }
            // No game offset is that large: refused before a reader's arithmetic can wrap on it.
            if (v > kMaxOffsetValue && std::string_view(kFields[index].group) != "diagnostics") {
                Warn(warnings, "offsets: \"" + key + "\" " + OffsetHex(v) + " is above " + OffsetHex(kMaxOffsetValue) +
                                   " (no game offset is that large); kept " + OffsetHex(next.*(kFields[index].member)));
                continue;
            }
            next.*(kFields[index].member) = v;
            set[index] = 1;
        }
        o = next;
        if (touched) *touched = std::move(set);
        return true;
    } catch (...) {
        return false;
    }
}

OffsetsInfo LoadOffsets(const std::filesystem::path& dataDir, const std::filesystem::path& configDir) {
    OffsetsInfo info;
    try {
        info.source.assign(std::size(kFields), OffsetSource::BuiltIn);
        info.dataFileRead = ApplyFile(dataDir / L"offsets.json", "data/offsets.json", OffsetSource::DataFile, info);
        info.configFileRead = ApplyFile(configDir / L"offsets.json", "config/offsets.json", OffsetSource::ConfigFile, info);
    } catch (...) {
        info.warnings.push_back("offsets: loading failed; the built-in values are in use where a file was not applied");
    }
    return info;
}

} // namespace bp
