// BuildPlanner - Path of Building import
// PobImport.cpp - see PobImport.h. The decoder half (base64, inflate, the XML scanner, entity
// decoding, the node-list parser) is POE1Fixer PassiveTreePlanner's PobImport.cpp; the link table,
// the PoE2 sections (<Skills>, <Items>, the <Spec> children) and the poe.ninja character page are new.
//
// Everything here treats its input as untrusted (a paste, a download): every size is bounded before
// it is used, the XML scanner is iterative with a depth cap, and inflate streams into a buffer that
// can never grow past 16 MiB. No function lets an exception escape.
#include "PobImport.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <iterator>
#include <map>
#include <memory>
#include <new>
#include <optional>

#include <nlohmann/json.hpp>

#include "BuildStore.h"   // Utf8Clamp
#include "GemData.h"
#include "PobItem.h"
#include "miniz/miniz.h"

namespace bp {
namespace {

constexpr size_t kMaxCodeChars = 16u << 20;    // a PoB code as text
constexpr size_t kMaxLinkChars = 16u << 10;
constexpr size_t kMaxDepth = 256;              // XML nesting (PoB writes 6)
constexpr size_t kMaxAttrsPerTag = 64;         // kept per tag (the rest are scanned and dropped)
constexpr size_t kInflateChunk = 64u << 10;
constexpr size_t kMaxItemTextBytes = 64u << 10;   // one <Item> body (PoB writes a few KiB)
constexpr size_t kMaxUrlTextBytes = 16u << 10;    // one <URL> body
constexpr size_t kMaxWarnings = 20;
constexpr int kMaxItemId = 1000000;            // <Item id> / Slot itemId / Socket itemId
constexpr size_t kMaxItemDrafts = 4 * kMaxItems;  // <Item>s kept while scanning (the rest only counted)
constexpr size_t kMaxListsPerSpec = 8;         // <WeaponSetN> / AttributeOverride lists kept per <Spec>

constexpr char kNinjaIndexUrl[] = "https://poe.ninja/poe2/api/data/index-state";

bool Fail(std::string* error, const std::string& what) {
    if (error) *error = what;
    return false;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

std::string_view Trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) ++a;
    while (b > a && IsSpace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string_view StripBom(std::string_view s) {
    if (s.size() >= 3 && static_cast<uint8_t>(s[0]) == 0xEF && static_cast<uint8_t>(s[1]) == 0xBB &&
        static_cast<uint8_t>(s[2]) == 0xBF)
        s.remove_prefix(3);
    return s;
}

char LowerAscii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualsI(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
    return true;
}

bool StartsWithI(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && EqualsI(s.substr(0, prefix.size()), prefix);
}

bool StartsWith(std::string_view s, std::string_view p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }

// Quotes untrusted text in an error message: at most `max` characters, printable ASCII only (so an
// error text is always valid UTF-8, whatever the input held).
std::string Clip(std::string_view s, size_t max = 40) {
    std::string out;
    for (size_t i = 0; i < s.size() && i < max; ++i) {
        const char c = s[i];
        out.push_back((c >= 0x20 && c <= 0x7E) ? c : '?');
    }
    if (s.size() > max) out += "...";
    return out;
}

int B64Value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

// Decimal digits only (surrounding whitespace allowed), at most 18 digits.
bool ParseUInt(std::string_view s, uint64_t& out) {
    s = Trim(s);
    if (s.empty() || s.size() > 18) return false;
    uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint64_t>(c - '0');
    }
    out = v;
    return true;
}

// Strict UTF-8 (no overlong forms, no surrogates, at most U+10FFFF) without NUL: PoB writes UTF-8
// XML, and a NUL never appears in it.
bool IsUtf8Text(std::string_view s) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    const size_t n = s.size();
    size_t i = 0;
    while (i < n) {
        const uint8_t c = p[i];
        if (c < 0x80) {
            if (c == 0) return false;
            ++i;
            continue;
        }
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
            return false;
        }
        if (n - i < len) return false;
        for (size_t k = 1; k < len; ++k) {
            const uint8_t cc = p[i + k];
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
        i += len;
    }
    return true;
}

void AppendUtf8(std::string& out, uint32_t cp) {
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

// ---- base64 ------------------------------------------------------------------------------------

// BadChar: a character outside the alphabet, data after padding or more than two '='. BadPadding:
// padding that does not complete the last group. Cut: the text ends one character into a group with
// no padding, which no whole encoding does: the text was cut off there.
enum class B64Status { Ok, BadChar, BadPadding, Cut };

B64Status DecodeB64(std::string_view text, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(text.size() / 4 * 3 + 3);
    uint32_t acc = 0;
    int bits = 0;
    size_t data = 0, pad = 0;
    for (const char ch : text) {
        if (IsSpace(ch)) continue;
        if (ch == '=') {
            if (++pad > 2) {
                out.clear();
                return B64Status::BadChar;
            }
            continue;
        }
        if (pad) {   // data after padding
            out.clear();
            return B64Status::BadChar;
        }
        const int v = B64Value(static_cast<unsigned char>(ch));
        if (v < 0) {
            out.clear();
            return B64Status::BadChar;
        }
        acc = ((acc << 6) | static_cast<uint32_t>(v)) & 0xFFFFFFu;
        bits += 6;
        ++data;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFFu));
        }
    }
    if (data % 4 == 1) {
        out.clear();
        return pad == 0 ? B64Status::Cut : B64Status::BadPadding;
    }
    if (pad > 0 && (data + pad) % 4 != 0) {
        out.clear();
        return B64Status::BadPadding;
    }
    return B64Status::Ok;
}

// ---- inflate -----------------------------------------------------------------------------------

enum class InflateStatus { Ok, TooLarge, Truncated, Corrupt, NoMemory };

// RFC 1950 header: deflate, window <= 32 K, check bits right, no preset dictionary.
bool LooksLikeZlib(const std::vector<uint8_t>& b) {
    if (b.size() < 2) return false;
    const unsigned cmf = b[0], flg = b[1];
    return (cmf & 0x0Fu) == 8 && (cmf >> 4) <= 7 && ((cmf << 8) | flg) % 31 == 0 && (flg & 0x20u) == 0;
}

// Streams the whole input through mz_inflate in 64 KiB steps. The output string grows by doubling
// and never past kMaxXmlBytes: the step that would pass it ends the decode with TooLarge.
InflateStatus Inflate(const std::vector<uint8_t>& in, int windowBits, std::string& out) {
    out.clear();
    if (in.size() > UINT_MAX) return InflateStatus::Corrupt;
    mz_stream s{};
    if (mz_inflateInit2(&s, windowBits) != MZ_OK) return InflateStatus::NoMemory;
    struct End {
        mz_stream* s;
        ~End() { mz_inflateEnd(s); }
    } end{&s};
    std::unique_ptr<unsigned char[]> chunk(new (std::nothrow) unsigned char[kInflateChunk]);
    if (!chunk) return InflateStatus::NoMemory;
    s.next_in = in.data();
    s.avail_in = static_cast<unsigned int>(in.size());
    int stalls = 0;
    for (;;) {
        s.next_out = chunk.get();
        s.avail_out = static_cast<unsigned int>(kInflateChunk);
        const unsigned int inBefore = s.avail_in;
        const int st = mz_inflate(&s, MZ_NO_FLUSH);
        const size_t produced = kInflateChunk - s.avail_out;
        if (produced > 0) {
            if (produced > kMaxXmlBytes - out.size()) return InflateStatus::TooLarge;
            if (out.capacity() - out.size() < produced) {
                const size_t grow = std::max(out.size() + produced, std::max(out.capacity() * 2, kInflateChunk));
                out.reserve(std::min(kMaxXmlBytes, grow));
            }
            out.append(reinterpret_cast<const char*>(chunk.get()), produced);
        }
        if (st == MZ_STREAM_END) return InflateStatus::Ok;
        if (st == MZ_BUF_ERROR) return InflateStatus::Truncated;   // the input ended before the stream did
        if (st == MZ_MEM_ERROR) return InflateStatus::NoMemory;
        if (st != MZ_OK) return InflateStatus::Corrupt;
        if (produced == 0 && s.avail_in == inBefore) {
            if (++stalls > 4) return InflateStatus::Truncated;
        } else {
            stalls = 0;
        }
    }
}

// ---- XML ---------------------------------------------------------------------------------------

struct XmlAttr {
    std::string_view name;
    std::string_view value;   // raw: entities not decoded yet
};

// An iterative, non-validating scanner for the XML subset PoB writes (and the usual extras: the
// declaration, comments, CDATA (skipped), DOCTYPE (skipped), either quote style).
class XmlScanner {
public:
    enum class Kind { Eof, Start, End, Text, Error };

    explicit XmlScanner(std::string_view s) : s_(s) {}

    std::string_view name;    // Start / End
    std::string_view text;    // Text (raw)
    bool selfClosing = false;
    std::vector<XmlAttr> attrs;
    std::string error;

    const XmlAttr* Find(std::string_view attr) const {
        for (const XmlAttr& a : attrs)
            if (a.name == attr) return &a;
        return nullptr;
    }

    Kind Next() {
        const size_t n = s_.size();
        for (;;) {
            if (i_ >= n) return Kind::Eof;
            if (s_[i_] != '<') {
                size_t lt = s_.find('<', i_);
                if (lt == std::string_view::npos) lt = n;
                text = s_.substr(i_, lt - i_);
                i_ = lt;
                return Kind::Text;
            }
            if (Has("<?")) {
                const size_t e = s_.find("?>", i_ + 2);
                if (e == std::string_view::npos) return Err("an unterminated <? ?> declaration");
                i_ = e + 2;
                continue;
            }
            if (Has("<!--")) {
                const size_t e = s_.find("-->", i_ + 4);
                if (e == std::string_view::npos) return Err("an unterminated comment");
                i_ = e + 3;
                continue;
            }
            if (Has("<![CDATA[")) {
                const size_t e = s_.find("]]>", i_ + 9);
                if (e == std::string_view::npos) return Err("an unterminated CDATA section");
                i_ = e + 3;
                continue;
            }
            if (Has("<!")) {   // <!DOCTYPE ...> with an optional [internal subset]
                size_t j = i_ + 2;
                size_t brackets = 0;
                char quote = 0;
                for (; j < n; ++j) {
                    const char c = s_[j];
                    if (quote) {
                        if (c == quote) quote = 0;
                    } else if (c == '"' || c == '\'') {
                        quote = c;
                    } else if (c == '[') {
                        ++brackets;
                    } else if (c == ']') {
                        if (brackets > 0) --brackets;
                    } else if (c == '>' && brackets == 0) {
                        break;
                    }
                }
                if (j >= n) return Err("an unterminated <! declaration");
                i_ = j + 1;
                continue;
            }
            if (Has("</")) {
                size_t j = i_ + 2;
                name = ReadName(j);
                if (name.empty()) return Err("a closing tag without a name");
                SkipSpace(j);
                if (j >= n || s_[j] != '>') return Err("an unterminated closing tag </" + Clip(name) + ">");
                i_ = j + 1;
                return Kind::End;
            }
            // a start tag
            size_t j = i_ + 1;
            name = ReadName(j);
            if (name.empty()) return Err("a '<' that starts no tag");
            attrs.clear();
            selfClosing = false;
            for (;;) {
                SkipSpace(j);
                if (j >= n) return Err("an unterminated tag <" + Clip(name) + ">");
                if (s_[j] == '>') {
                    ++j;
                    break;
                }
                if (s_[j] == '/') {
                    if (j + 1 < n && s_[j + 1] == '>') {
                        selfClosing = true;
                        j += 2;
                        break;
                    }
                    return Err("a stray '/' in the tag <" + Clip(name) + ">");
                }
                const std::string_view attrName = ReadName(j);
                if (attrName.empty()) return Err("a malformed attribute in the tag <" + Clip(name) + ">");
                SkipSpace(j);
                if (j >= n || s_[j] != '=') return Err("the attribute " + Clip(attrName) + " has no value");
                ++j;
                SkipSpace(j);
                if (j >= n || (s_[j] != '"' && s_[j] != '\''))
                    return Err("the value of the attribute " + Clip(attrName) + " is not quoted");
                const char q = s_[j];
                const size_t close = s_.find(q, j + 1);
                if (close == std::string_view::npos) return Err("the value of the attribute " + Clip(attrName) + " is unterminated");
                if (attrs.size() < kMaxAttrsPerTag) attrs.push_back({attrName, s_.substr(j + 1, close - j - 1)});
                j = close + 1;
            }
            i_ = j;
            return Kind::Start;
        }
    }

private:
    bool Has(std::string_view p) const { return s_.compare(i_, p.size(), p) == 0; }

    void SkipSpace(size_t& j) const {
        while (j < s_.size() && IsSpace(s_[j])) ++j;
    }

    std::string_view ReadName(size_t& j) const {
        const size_t start = j;
        while (j < s_.size()) {
            const char c = s_[j];
            if (IsSpace(c) || c == '/' || c == '>' || c == '<' || c == '=' || c == '"' || c == '\'') break;
            ++j;
        }
        return s_.substr(start, j - start);
    }

    Kind Err(const std::string& what) {
        error = what;
        i_ = s_.size();
        return Kind::Error;
    }

    std::string_view s_;
    size_t i_ = 0;
};

// Appends `raw` with the five XML entities and numeric character references decoded. An unknown,
// invalid or unterminated reference stays as literal text. In attribute values a literal tab, CR
// or LF becomes a space (XML attribute-value normalisation; CR LF counts once). `limit` caps `out`.
void AppendDecoded(std::string_view raw, std::string& out, bool attribute, size_t limit = SIZE_MAX) {
    size_t i = 0;
    while (i < raw.size() && out.size() < limit) {
        const char c = raw[i];
        if (c == '&') {
            const std::string_view window = raw.substr(i + 1, 12);
            const size_t semi = window.find(';');
            if (semi != std::string_view::npos && semi > 0) {
                const std::string_view ent = window.substr(0, semi);
                bool done = true;
                if (ent == "amp") out.push_back('&');
                else if (ent == "lt") out.push_back('<');
                else if (ent == "gt") out.push_back('>');
                else if (ent == "quot") out.push_back('"');
                else if (ent == "apos") out.push_back('\'');
                else if (ent[0] == '#' && ent.size() >= 2) {
                    const bool hex = ent[1] == 'x' || ent[1] == 'X';
                    const std::string_view digits = ent.substr(hex ? 2 : 1);
                    uint32_t cp = 0;
                    bool okNum = !digits.empty() && digits.size() <= 8;
                    for (char d : digits) {
                        if (!okNum) break;
                        uint32_t v;
                        if (d >= '0' && d <= '9') v = static_cast<uint32_t>(d - '0');
                        else if (hex && d >= 'a' && d <= 'f') v = static_cast<uint32_t>(d - 'a' + 10);
                        else if (hex && d >= 'A' && d <= 'F') v = static_cast<uint32_t>(d - 'A' + 10);
                        else {
                            okNum = false;
                            break;
                        }
                        cp = cp * (hex ? 16u : 10u) + v;
                        if (cp > 0x10FFFF) okNum = false;
                    }
                    if (okNum && cp != 0 && !(cp >= 0xD800 && cp <= 0xDFFF)) AppendUtf8(out, cp);
                    else done = false;
                } else {
                    done = false;
                }
                if (done) {
                    i += semi + 2;
                    continue;
                }
            }
            out.push_back('&');
            ++i;
            continue;
        }
        if (attribute && (c == '\t' || c == '\n' || c == '\r')) {
            out.push_back(' ');
            i += (c == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') ? 2 : 1;
            continue;
        }
        out.push_back(c);
        ++i;
    }
}

std::string DecodeAttr(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    AppendDecoded(raw, out, true);
    return out;
}

// An attribute's decoded value; nullopt when it is absent or the literal "nil" third-party writers
// emit for an absent value.
std::optional<std::string> AttrOf(const XmlScanner& sc, std::string_view name) {
    const XmlAttr* a = sc.Find(name);
    if (!a) return std::nullopt;
    std::string v = DecodeAttr(a->value);
    if (v == "nil") return std::nullopt;
    return v;
}

std::string AttrText(const XmlScanner& sc, std::string_view name) {
    std::optional<std::string> v = AttrOf(sc, name);
    return v ? std::move(*v) : std::string();
}

int ParseIntIn(std::string_view s, int lo, int hi, int fallback) {
    uint64_t v = 0;
    if (!ParseUInt(s, v) || v < static_cast<uint64_t>(lo) || v > static_cast<uint64_t>(hi)) return fallback;
    return static_cast<int>(v);
}

int AttrInt(const XmlScanner& sc, std::string_view name, int lo, int hi, int fallback) {
    const std::optional<std::string> v = AttrOf(sc, name);
    return v ? ParseIntIn(*v, lo, hi, fallback) : fallback;
}

// "true" -> true, "false" -> false, anything else (absent, "nil") -> `fallback`.
bool AttrBool(const XmlScanner& sc, std::string_view name, bool fallback) {
    const std::optional<std::string> v = AttrOf(sc, name);
    if (!v) return fallback;
    if (*v == "true") return true;
    if (*v == "false") return false;
    return fallback;
}

// "id,id,..." -> sorted unique ids < 65536 (larger or malformed ids are skipped and counted in
// `skipped`). A 65536-bit set (8 KiB) dedupes, so the output never exceeds 65536 ids whatever the input.
std::vector<uint16_t> ParseNodeList(std::string_view list, size_t* skipped = nullptr) {
    std::vector<uint64_t> bits(65536 / 64, 0);
    size_t count = 0, bad = 0;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string_view::npos) comma = list.size();
        const std::string_view part = Trim(list.substr(pos, comma - pos));
        uint64_t v = 0;
        if (ParseUInt(part, v) && v <= 0xFFFF) {
            uint64_t& word = bits[static_cast<size_t>(v >> 6)];
            const uint64_t bit = 1ull << (v & 63);
            if (!(word & bit)) {
                word |= bit;
                ++count;
            }
        } else if (!part.empty()) {
            ++bad;
        }
        pos = comma + 1;
    }
    std::vector<uint16_t> nodes;
    nodes.reserve(count);
    for (size_t w = 0; w < bits.size(); ++w)
        for (uint64_t word = bits[w]; word != 0; word &= word - 1)
            nodes.push_back(static_cast<uint16_t>(w * 64 + static_cast<size_t>(std::countr_zero(word))));
    if (skipped) *skipped += bad;
    return nodes;
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

std::string CleanText(std::string_view s, size_t maxBytes) { return Utf8Clamp(Trim(StripPobColors(s)), maxBytes); }

// ---- class / ascendancy maps (Path of Building's legacyClassIdMap; tree.json "classes" per version) --

struct LegacyClasses {
    std::string_view version;
    int classByIndex[8];   // legacy classId (0-based) -> integerId, -1 past the end
};

constexpr LegacyClasses kLegacyClassMaps[] = {
    {"0_1", {2, 6, 9, 1, 7, 10, -1, -1}},
    {"0_2", {2, 8, 6, 9, 1, 7, 10, -1}},
    {"0_3", {2, 8, 6, 9, 1, 7, 10, -1}},
};

struct AscendancyList {
    int classId;
    std::string_view ids[4];   // legacy ascendClassId 1..4 -> internalId ("" past the end)
};

struct VersionAscendancies {
    std::string_view version;
    AscendancyList classes[8];
};

constexpr VersionAscendancies kAscendancies[] = {
    {"0_1",
     {{2, {"Ranger1", "Ranger3"}},
      {6, {"Warrior1", "Warrior2"}},
      {9, {"Mercenary2", "Mercenary3"}},
      {1, {"Witch1", "Witch2"}},
      {7, {"Sorceress1", "Sorceress2"}},
      {10, {"Monk2", "Monk3"}},
      {-1, {}},
      {-1, {}}}},
    {"0_2",
     {{2, {"Ranger1", "Ranger3"}},
      {8, {"Huntress1", "Huntress3"}},
      {6, {"Warrior1", "Warrior2", "Warrior3"}},
      {9, {"Mercenary1", "Mercenary2", "Mercenary3"}},
      {1, {"Witch1", "Witch2", "Witch3"}},
      {7, {"Sorceress1", "Sorceress2"}},
      {10, {"Monk2", "Monk3"}},
      {-1, {}}}},
    {"0_3",
     {{2, {"Ranger1", "Ranger3"}},
      {8, {"Huntress1", "Huntress3"}},
      {6, {"Warrior1", "Warrior2", "Warrior3"}},
      {9, {"Mercenary1", "Mercenary2", "Mercenary3"}},
      {1, {"Witch1", "Witch2", "Witch3", "Witch3b"}},
      {7, {"Sorceress1", "Sorceress2"}},
      {10, {"Monk2", "Monk3"}},
      {-1, {}}}},
    {"0_4",
     {{2, {"Ranger1", "Ranger3"}},
      {8, {"Huntress1", "Huntress3"}},
      {6, {"Warrior1", "Warrior2", "Warrior3"}},
      {9, {"Mercenary1", "Mercenary2", "Mercenary3"}},
      {11, {"Druid1", "Druid2"}},
      {1, {"Witch1", "Witch2", "Witch3", "Witch3b"}},
      {7, {"Sorceress1", "Sorceress2", "Sorceress3"}},
      {10, {"Monk2", "Monk3"}}}},
    {"0_5",
     {{2, {"Ranger1", "Ranger3"}},
      {8, {"Huntress1", "Huntress2", "Huntress3"}},
      {6, {"Warrior1", "Warrior2", "Warrior3"}},
      {9, {"Mercenary1", "Mercenary2", "Mercenary3"}},
      {11, {"Druid1", "Druid2"}},
      {1, {"Witch1", "Witch2", "Witch3", "Witch3b"}},
      {7, {"Sorceress1", "Sorceress2", "Sorceress3"}},
      {10, {"Monk1", "Monk2", "Monk3"}}}},
};

constexpr int kKnownClassIds[] = {1, 2, 6, 7, 8, 9, 10, 11};   // the integerIds of 0_4 and 0_5

bool KnownVersion(std::string_view v) {
    for (const VersionAscendancies& va : kAscendancies)
        if (va.version == v) return true;
    return false;
}

// "0_5" style: digits '_' digits, at most 16 characters.
bool IsVersionText(std::string_view v) {
    const size_t us = v.find('_');
    if (v.empty() || v.size() > 16 || us == std::string_view::npos || us == 0 || us + 1 >= v.size()) return false;
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

// ---- the official tree link inside <URL> (GGG's format, PassiveSpec:DecodeURL) -----------------

struct UrlTree {
    int classByte = -1, ascendByte = 0;
    std::vector<uint16_t> nodes;
};

bool DecodeTreeUrl(std::string_view url, UrlTree& out) {
    out = UrlTree{};
    std::string_view s = Trim(url);
    if (s.empty() || s.size() > kMaxUrlTextBytes) return false;
    if (const size_t cut = s.find_first_of("?#"); cut != std::string_view::npos) s = s.substr(0, cut);
    while (!s.empty() && s.back() == '/') s.remove_suffix(1);
    if (const size_t slash = s.rfind('/'); slash != std::string_view::npos) s = s.substr(slash + 1);
    for (int k = 0; k < 2 && s.size() >= 3 && EqualsI(s.substr(s.size() - 3), "%3D"); ++k) s.remove_suffix(3);
    std::vector<uint8_t> b;
    if (DecodeB64(s, b) != B64Status::Ok || b.size() < 7) return false;
    const uint32_t ver = (static_cast<uint32_t>(b[0]) << 24) | (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) | b[3];
    if (ver < 4 || ver > 6) return false;
    out.classByte = b[4];
    out.ascendByte = b[5] & 3;
    size_t p = 6;
    auto u16 = [&b](size_t at) { return static_cast<uint16_t>((static_cast<unsigned>(b[at]) << 8) | b[at + 1]); };
    if (ver == 4) {
        p = 7;   // the "fullscreen" byte
        if ((b.size() - p) % 2 != 0) return false;
        for (; p + 2 <= b.size(); p += 2) out.nodes.push_back(u16(p));
    } else {
        const size_t n = b[p++];
        if (b.size() - p < n * 2) return false;
        for (size_t k = 0; k < n; ++k, p += 2) out.nodes.push_back(u16(p));
    }
    std::sort(out.nodes.begin(), out.nodes.end());
    out.nodes.erase(std::unique(out.nodes.begin(), out.nodes.end()), out.nodes.end());
    return true;
}

// ---- the parse state of one document ------------------------------------------------------------

struct Warnings {
    std::vector<std::string>* out = nullptr;
    size_t count = 0;
    void Add(std::string text) {
        if (!out) return;
        if (count++ < kMaxWarnings) out->push_back(std::move(text));
    }
};

struct SpecDraft {
    std::string title, treeVersion, classInternal, classLegacy, ascInternal, ascLegacy, nodes, url;
    bool hasClassInternal = false, hasClassLegacy = false, hasAscInternal = false, hasAscLegacy = false, hasNodes = false,
         hasUrl = false;
    std::vector<std::pair<int, std::string>> weaponSets;   // (1 / 2, node list) in document order
    std::vector<std::string> strNodes, dexNodes, intNodes;
    std::vector<std::pair<uint16_t, int>> sockets;          // (node, <Item id>) in document order
    std::vector<std::pair<uint16_t, std::string>> notes;    // in document order
};

struct GroupDraft {
    BuildSkillGroup group;
    int mainActive1 = 1;            // the 1-based mainActiveSkill attribute
    std::vector<int> pobActives;    // parallel to group.gems: entries each adds to PoB's active skill list
    bool removed = false;
};

struct SkillSetDraft {
    int id = 0;
    BuildSkillSet set;
};

struct ItemSetDraft {
    int id = 0;
    BuildItemSet set;
    std::vector<std::pair<std::string, int>> slots;   // (slot name, <Item id>, 0 = empty) in document order
};

void ReadSpecAttrs(const XmlScanner& sc, SpecDraft& d) {
    d.title = AttrText(sc, "title");
    d.treeVersion = std::string(Trim(AttrText(sc, "treeVersion")));
    if (auto v = AttrOf(sc, "classInternalId")) {
        d.classInternal = *v;
        d.hasClassInternal = true;
    }
    if (auto v = AttrOf(sc, "classId")) {
        d.classLegacy = *v;
        d.hasClassLegacy = true;
    }
    if (auto v = AttrOf(sc, "ascendancyInternalId")) {
        d.ascInternal = std::string(Trim(*v));
        d.hasAscInternal = true;
    }
    if (auto v = AttrOf(sc, "ascendClassId")) {
        d.ascLegacy = *v;
        d.hasAscLegacy = true;
    }
    if (auto v = AttrOf(sc, "nodes")) {
        d.nodes = std::move(*v);
        d.hasNodes = true;
    }
}

// Turns a finished <Spec> into a BuildSpec. Item references stay <Item id>s here (resolved at the end).
BuildSpec FinishSpec(const SpecDraft& d, size_t index, Warnings& warn, std::vector<std::pair<uint16_t, int>>& socketIds) {
    BuildSpec sp;
    sp.title = CleanText(d.title, kMaxTitleBytes);
    const std::string where = "tree " + std::to_string(index + 1);
    sp.treeVersion = d.treeVersion.empty() ? std::string("0_1") : (IsVersionText(d.treeVersion) ? d.treeVersion : std::string());
    if (!d.treeVersion.empty() && sp.treeVersion.empty()) warn.Add(where + ": the tree version \"" + Clip(d.treeVersion) + "\" is not valid");
    if (d.hasClassInternal) sp.classId = ParseIntIn(d.classInternal, 0, 255, -1);
    if (sp.classId < 0 && d.hasClassLegacy) {
        const int legacy = ParseIntIn(d.classLegacy, 0, 255, -1);
        if (legacy >= 0) sp.classId = ClassIdFromLegacy(sp.treeVersion, legacy);
    }
    bool ascKnown = false;
    if (d.hasAscInternal) {
        if (d.ascInternal.empty()) {
            ascKnown = true;   // "" = no ascendancy
        } else if (IsAscendancyIdText(d.ascInternal)) {
            sp.ascendancyId = d.ascInternal;
            ascKnown = true;
        }
    }
    if (!ascKnown && d.hasAscLegacy) {
        const int legacy = ParseIntIn(d.ascLegacy, 0, 255, -1);
        if (legacy >= 0) sp.ascendancyId = AscendancyIdFromLegacy(sp.treeVersion, sp.classId, legacy);
    }

    size_t badIds = 0;
    if (d.hasNodes) {
        sp.nodes = ParseNodeList(d.nodes, &badIds);
    } else if (d.hasUrl) {
        UrlTree link;
        if (DecodeTreeUrl(d.url, link)) {
            sp.nodes = std::move(link.nodes);
            if (sp.classId < 0) sp.classId = ClassIdFromLegacy(sp.treeVersion, link.classByte);
            if (!ascKnown && !d.hasAscLegacy) sp.ascendancyId = AscendancyIdFromLegacy(sp.treeVersion, sp.classId, link.ascendByte);
        } else {
            warn.Add(where + ": its tree link could not be read");
        }
    }
    if (badIds > 0) warn.Add(where + ": " + std::to_string(badIds) + " passive ids that are not numbers below 65536 dropped");
    if (sp.nodes.size() > kMaxNodes) {
        warn.Add(where + ": " + std::to_string(sp.nodes.size() - kMaxNodes) + " passives over the limit of " + std::to_string(kMaxNodes) +
                 " dropped");
        sp.nodes.resize(kMaxNodes);
    }

    // Weapon sets: subsets of the nodes, disjoint; a later <WeaponSetN> wins a node listed in both.
    std::vector<uint16_t> ws[3];
    for (const auto& [set, list] : d.weaponSets) {
        std::vector<uint16_t> ids = Intersect(ParseNodeList(list), sp.nodes);
        const int other = set == 1 ? 2 : 1;
        ws[other] = Minus(ws[other], ids);
        std::vector<uint16_t> merged;
        std::set_union(ws[set].begin(), ws[set].end(), ids.begin(), ids.end(), std::back_inserter(merged));
        ws[set] = std::move(merged);
    }
    sp.weaponSet1 = std::move(ws[1]);
    sp.weaponSet2 = std::move(ws[2]);

    // Attribute choices: subsets of the nodes, disjoint. Path of Building applies Str, then Dex, then
    // Int, so a node listed twice keeps the later attribute.
    auto collect = [&](const std::vector<std::string>& lists) {
        std::vector<uint16_t> all;
        for (const std::string& l : lists) {
            std::vector<uint16_t> ids = ParseNodeList(l), merged;
            std::set_union(all.begin(), all.end(), ids.begin(), ids.end(), std::back_inserter(merged));
            all = std::move(merged);
        }
        return Intersect(all, sp.nodes);
    };
    sp.attrInt = collect(d.intNodes);
    sp.attrDex = Minus(collect(d.dexNodes), sp.attrInt);
    sp.attrStr = Minus(Minus(collect(d.strNodes), sp.attrInt), sp.attrDex);

    // Sockets: one jewel per node, the last <Socket> of a node wins (as Path of Building's table does).
    std::map<uint16_t, int> sockets;
    for (const auto& [node, itemId] : d.sockets) sockets[node] = itemId;
    for (const auto& [node, itemId] : sockets) {
        if (socketIds.size() >= kMaxSpecSockets) {
            warn.Add(where + ": jewel sockets over the limit of " + std::to_string(kMaxSpecSockets) + " dropped");
            break;
        }
        socketIds.emplace_back(node, itemId);
    }

    std::map<uint16_t, std::string> notes;
    for (const auto& [node, text] : d.notes) {
        std::string t = CleanText(text, kMaxNoteBytes);
        if (t.empty()) notes.erase(node);
        else notes[node] = std::move(t);
    }
    for (auto& [node, text] : notes) {
        if (sp.notes.size() >= kMaxSpecNotes) {
            warn.Add(where + ": node notes over the limit of " + std::to_string(kMaxSpecNotes) + " dropped");
            break;
        }
        sp.notes.push_back(SpecNote{node, std::move(text)});
    }
    return sp;
}

// "SpearThrowPlayer" -> "Spear Throw": a name for a gem that has neither a name nor a known id.
std::string NameFromSkillId(std::string_view id) {
    if (id.size() > 6 && id.substr(id.size() - 6) == "Player") id.remove_suffix(6);
    std::string out;
    for (size_t i = 0; i < id.size() && out.size() < kMaxNameBytes; ++i) {
        const char c = id[i];
        if (i > 0 && c >= 'A' && c <= 'Z' && !(id[i - 1] >= 'A' && id[i - 1] <= 'Z')) out.push_back(' ');
        out.push_back(c == '_' ? ' ' : c);
    }
    return std::string(Trim(out));
}

bool IsIdText(std::string_view s, size_t maxBytes) {
    if (s.empty() || s.size() > maxBytes) return false;
    for (char c : s)
        if (static_cast<unsigned char>(c) < 0x21 || c == 0x7F) return false;
    return true;
}

// One <Gem>. false when it names nothing at all. `pobActives`: the entries the gem adds to Path of
// Building's list of the group's active skills (0 when disabled; GemInfo::activeSkills when the table
// knows it; else 1 for a non-support gem).
bool ReadGem(const XmlScanner& sc, const GemData* gems, BuildGem& g, int& pobActives) {
    g = BuildGem{};
    pobActives = 0;
    const std::string nameSpec = CleanText(AttrText(sc, "nameSpec"), kMaxNameBytes);
    const std::string gemId(Trim(AttrText(sc, "gemId")));
    const std::string skillId(Trim(AttrText(sc, "skillId")));
    const std::string variantId(Trim(AttrText(sc, "variantId")));
    if (IsIdText(gemId, kMaxNameBytes)) g.gemId = gemId;
    if (IsIdText(skillId, kMaxNameBytes)) g.skillId = skillId;
    if (IsIdText(variantId, kMaxNameBytes)) g.variantId = variantId;
    const GemInfo* info = nullptr;
    if (gems && !gems->Empty()) {
        if (!g.gemId.empty()) info = gems->Find(g.gemId, g.variantId);
        if (!info && !g.skillId.empty()) info = gems->FindBySkillId(g.skillId);
        if (!info && !nameSpec.empty()) info = gems->FindByName(nameSpec);
    }
    if (info) {
        if (g.gemId.empty()) g.gemId = info->gameId.empty() ? info->id : info->gameId;
        if (g.skillId.empty()) g.skillId = info->skillId;
    }
    g.name = nameSpec;
    if (g.name.empty() && info) g.name = info->name;
    if (g.name.empty() && !g.skillId.empty()) g.name = NameFromSkillId(g.skillId);
    if (g.name.empty()) return false;
    g.level = AttrInt(sc, "level", 0, 100, 0);
    g.quality = AttrInt(sc, "quality", 0, 100, 0);
    g.enabled = AttrBool(sc, "enabled", true);
    g.support = IsSupportGem(gems, g.gemId, g.variantId, g.skillId);
    if (g.enabled) pobActives = info ? info->activeSkills : (g.support ? 0 : 1);
    return true;
}

GroupSource SourceOf(std::string_view source) {
    source = Trim(source);
    if (source.empty()) return GroupSource::Gems;
    if (StartsWith(source, "Item")) return GroupSource::Item;
    if (StartsWith(source, "Tree")) return GroupSource::Tree;
    // "Default Attack", and the groups Path of Building generates itself ("Explode", "Thorns").
    return GroupSource::DefaultAttack;
}

// mainActiveSkill (1-based, into Path of Building's list of the group's active skills: every enabled gem
// adds its GroupDraft::pobActives entries in gem order) -> BuildSkillGroup::mainActive (0-based among
// ALL the group's non-support gems, Types.h). See PobImport.h "Skill groups".
void FinishGroup(GroupDraft& d) {
    const std::vector<BuildGem>& gems = d.group.gems;
    int total = 0;
    for (size_t k = 0; k < gems.size() && k < d.pobActives.size(); ++k) total += d.pobActives[k];
    int owner = -1;   // the gem whose entry mainActiveSkill names
    if (total > 0) {
        const int target = std::clamp(d.mainActive1, 1, total);   // past the list: its last entry (PoB clamps)
        int seen = 0;
        for (size_t k = 0; k < gems.size() && k < d.pobActives.size(); ++k) {
            seen += d.pobActives[k];
            if (seen >= target) {
                owner = static_cast<int>(k);
                break;
            }
        }
    }
    // The owner's place among the non-support gems; a support's triggered skill, or no entry at all:
    // the first enabled non-support gem; none: 0.
    int ordinal = 0, mainActive = -1, firstEnabled = -1;
    for (size_t k = 0; k < gems.size(); ++k) {
        if (gems[k].support) continue;
        if (static_cast<int>(k) == owner) mainActive = ordinal;
        if (gems[k].enabled && firstEnabled < 0) firstEnabled = ordinal;
        ++ordinal;
    }
    d.group.mainActive = mainActive >= 0 ? mainActive : (firstEnabled >= 0 ? firstEnabled : 0);
}

bool LooksLikeHtml(std::string_view body) {
    const std::string_view head = body.substr(0, 512);
    if (StartsWithI(head, "<!doctype html") || StartsWithI(head, "<html")) return true;
    for (size_t i = 0; i + 5 <= head.size(); ++i)
        if (EqualsI(head.substr(i, 5), "<html")) return true;
    return false;
}

std::string PercentEncode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// %XX sequences decoded ('+' stays). false on a malformed escape.
bool PercentDecode(std::string_view s, std::string& out) {
    out.clear();
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out.push_back(s[i]);
            continue;
        }
        if (i + 2 >= s.size()) return false;
        const int hi = HexValue(s[i + 1]), lo = HexValue(s[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<char>(hi * 16 + lo));
        i += 2;
    }
    return true;
}

// A poe.ninja character page path part: percent-decoded, 1..64 bytes of UTF-8 without control
// characters or URL delimiters.
bool NinjaPart(std::string_view raw, std::string& out) {
    if (!PercentDecode(raw, out) || out.empty() || out.size() > 64 || !IsUtf8Text(out)) return false;
    for (const char c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F || c == '/' || c == '?' || c == '#') return false;
    return true;
}

// ---- input classification ----------------------------------------------------------------------

bool IsIdChar(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

bool IsId(std::string_view s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!IsIdChar(c)) return false;
    return true;
}

// Splits a path on '/', dropping empty segments (a leading or trailing slash).
std::vector<std::string_view> Segments(std::string_view path) {
    std::vector<std::string_view> out;
    size_t pos = 0;
    while (pos <= path.size() && out.size() < 64) {
        size_t slash = path.find('/', pos);
        if (slash == std::string_view::npos) slash = path.size();
        if (slash > pos) out.push_back(path.substr(pos, slash - pos));
        pos = slash + 1;
    }
    return out;
}

// A link split into a lower-case host ("www." removed) and its path segments (query and fragment
// dropped). False when the text cannot be a link (whitespace inside, no host, an empty segment).
struct LinkParts {
    std::string host;
    std::vector<std::string_view> segs;
};

bool SplitLink(std::string_view t, LinkParts& out) {
    for (char c : t)
        if (IsSpace(c) || static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return false;
    if (!IsUtf8Text(t)) return false;
    if (StartsWithI(t, "https://")) t.remove_prefix(8);
    else if (StartsWithI(t, "http://")) t.remove_prefix(7);
    if (const size_t cut = t.find_first_of("?#"); cut != std::string_view::npos) t = t.substr(0, cut);
    const size_t slash = t.find('/');
    const std::string_view host = t.substr(0, slash);
    if (host.empty() || host.size() > 253) return false;
    out.host.clear();
    for (char c : host) out.host.push_back(LowerAscii(c));
    if (out.host.rfind("www.", 0) == 0) out.host.erase(0, 4);
    const std::string_view path = slash == std::string_view::npos ? std::string_view() : t.substr(slash + 1);
    if ((!path.empty() && path[0] == '/') || path.find("//") != std::string_view::npos) return false;
    out.segs = Segments(path);
    return true;
}

bool LooksLikePobCode(std::string_view t) {
    size_t chars = 0;
    for (char c : t) {
        if (IsSpace(c)) continue;
        if (B64Value(static_cast<unsigned char>(c)) < 0 && c != '=') return false;
        ++chars;
    }
    return chars >= 64;
}

// "<id>" or "raw/<id>" as the rest of a path from `k` on; "" when it is neither.
std::string_view IdAfter(const std::vector<std::string_view>& sg, size_t k, bool rawPrefix) {
    if (rawPrefix && sg.size() == k + 2 && sg[k] == "raw") ++k;
    if (sg.size() == k + 1 && IsId(sg[k]) && sg[k] != "raw") return sg[k];
    return {};
}

Classified ClassifyLink(std::string_view t);

// "<https://pobb.in/x>" (a link in angle brackets, as Discord writes one to suppress its preview) ->
// the link inside; any other text unchanged. Only a known link is unwrapped, so "<PathOfBuilding2/>"
// stays XML.
std::string_view UnwrapAngleLink(std::string_view t) {
    if (t.size() < 3 || t.front() != '<' || t.back() != '>') return t;
    const std::string_view inner = Trim(t.substr(1, t.size() - 2));
    if (inner.empty() || inner.find_first_of("<>") != std::string_view::npos) return t;
    const InputKind k = ClassifyLink(inner).kind;
    return (k == InputKind::Unknown || k == InputKind::PobCode) ? t : inner;
}

Classified ClassifyImpl(std::string_view text) {
    Classified c;
    const std::string_view t = UnwrapAngleLink(Trim(StripBom(Trim(text))));
    if (t.empty()) {
        c.kind = InputKind::Empty;
        return c;
    }
    if (t[0] == '<') {
        c.kind = InputKind::PobXml;
        return c;
    }
    return ClassifyLink(t);
}

// A trimmed text that does not start with '<': a known link, a code, or Unknown.
Classified ClassifyLink(std::string_view t) {
    Classified c;
    LinkParts lp;
    if (t.size() <= kMaxLinkChars && SplitLink(t, lp)) {
        const auto& sg = lp.segs;
        auto link = [&c](InputKind kind, std::string url) {
            c.kind = kind;
            c.fetchUrl = std::move(url);
            return c;
        };
        if (lp.host == "pobb.in") {
            // <id>[/raw], and a user's paste u/<user>/<id>[/raw]; "u" and "raw" are never an id
            const size_t n = sg.size() - ((!sg.empty() && sg.back() == "raw") ? 1 : 0);
            if (n == 1 && IsId(sg[0]) && sg[0] != "u" && sg[0] != "raw")
                return link(InputKind::PobbinLink, "https://pobb.in/" + std::string(sg[0]) + "/raw");
            if (n == 3 && sg[0] == "u" && IsId(sg[1]) && IsId(sg[2]) && sg[2] != "raw")
                return link(InputKind::PobbinLink, "https://pobb.in/u/" + std::string(sg[1]) + "/" + std::string(sg[2]) + "/raw");
        } else if (lp.host == "pastebin.com") {
            if (sg.size() == 1 && IsId(sg[0]) && sg[0] != "raw")
                return link(InputKind::PastebinLink, "https://pastebin.com/raw/" + std::string(sg[0]));
            if (sg.size() == 2 && sg[0] == "raw" && IsId(sg[1]))
                return link(InputKind::PastebinLink, "https://pastebin.com/raw/" + std::string(sg[1]));
        } else if (lp.host == "poe.ninja" || lp.host == "poe2.ninja") {
            // poe.ninja/poe2/pob/[raw/]<id>; poe2.ninja/[poe2/]pob/[raw/]<id>. A poe.ninja link without
            // "poe2/" is a Path of Exile 1 build.
            size_t k = (!sg.empty() && sg[0] == "poe2") ? 1 : 0;
            if ((k == 1 || lp.host == "poe2.ninja") && sg.size() > k && sg[k] == "pob") {
                const std::string_view id = IdAfter(sg, k + 1, true);
                if (!id.empty()) return link(InputKind::PoeNinjaLink, "https://poe.ninja/poe2/pob/raw/" + std::string(id));
            }
            if (lp.host == "poe.ninja" && sg.size() == 6 && sg[0] == "poe2" && sg[1] == "builds" && sg[3] == "character") {
                Classified ch;
                if (NinjaPart(sg[2], ch.league) && NinjaPart(sg[4], ch.account) && NinjaPart(sg[5], ch.character)) {
                    ch.kind = InputKind::PoeNinjaCharacter;
                    return ch;
                }
            }
        } else if (lp.host == "maxroll.gg") {
            if (sg.size() >= 2 && sg[0] == "poe2") {
                const size_t k = (sg.size() >= 2 && sg[1] == "api") ? 2 : 1;
                if (sg.size() > k && sg[k] == "pob") {
                    const std::string_view id = IdAfter(sg, k + 1, false);
                    if (!id.empty()) return link(InputKind::MaxrollPobLink, "https://maxroll.gg/poe2/api/pob/" + std::string(id));
                }
                if (sg[1] == "build-guides" || sg[1] == "planner") {
                    c.kind = InputKind::MaxrollGuide;
                    return c;
                }
            }
        } else if (lp.host == "poe2db.tw") {
            if (sg.size() >= 2 && sg[0] == "pob") {
                const size_t n = sg.size() - (sg.back() == "raw" ? 1 : 0);
                if (n == 2 && IsId(sg[1]) && sg[1] != "raw")
                    return link(InputKind::Poe2dbLink, "https://poe2db.tw/pob/" + std::string(sg[1]) + "/raw");
            }
        }
    }
    if (LooksLikePobCode(t)) {
        c.kind = InputKind::PobCode;
        return c;
    }
    c.kind = InputKind::Unknown;
    return c;
}

std::string DecodeFailureText(const std::string& e) { return e.empty() ? "the build could not be decoded" : e; }

// DecodePobCode, plus `cutOff`: true when the decode failed only because the data ends early.
bool DecodePobCodeImpl(std::string_view code, std::string& xml, std::string* error, bool* cutOff) {
    xml.clear();
    if (cutOff) *cutOff = false;
    try {
        const std::string_view t = Trim(code);
        if (t.empty()) return Fail(error, "the code is empty");
        if (t.size() > kMaxCodeChars) return Fail(error, "the code is too long (over 16 MiB)");
        std::vector<uint8_t> bytes;
        const B64Status b64 = DecodeB64(t, bytes);
        if (b64 == B64Status::Cut) {
            if (cutOff) *cutOff = true;
            return Fail(error, "the code is incomplete (it looks cut off)");
        }
        if (b64 != B64Status::Ok || bytes.empty())
            return Fail(error, "the code is not a Path of Building code (not valid base64; it may be cut off or mixed with other text)");
        std::string text;
        InflateStatus st;
        if (LooksLikeZlib(bytes)) {
            st = Inflate(bytes, MZ_DEFAULT_WINDOW_BITS, text);
            if (st == InflateStatus::Corrupt) {
                std::string raw;
                if (Inflate(bytes, -MZ_DEFAULT_WINDOW_BITS, raw) == InflateStatus::Ok) {
                    text = std::move(raw);
                    st = InflateStatus::Ok;
                }
            }
        } else {
            st = Inflate(bytes, -MZ_DEFAULT_WINDOW_BITS, text);
        }
        if (st == InflateStatus::Truncated && cutOff) *cutOff = true;
        switch (st) {
        case InflateStatus::Ok: break;
        case InflateStatus::TooLarge: return Fail(error, "the code expands to more than 16 MiB (refused)");
        case InflateStatus::Truncated: return Fail(error, "the code is incomplete (it looks cut off)");
        case InflateStatus::NoMemory: return Fail(error, "out of memory while decoding the code");
        case InflateStatus::Corrupt:
        default: return Fail(error, "the code is not a Path of Building code (its compressed data is damaged)");
        }
        if (!IsUtf8Text(text)) return Fail(error, "the code does not hold text (not UTF-8)");
        xml = std::move(text);
        return true;
    } catch (const std::bad_alloc&) {
        xml.clear();
        if (cutOff) *cutOff = false;
        return Fail(error, "out of memory while decoding the code");
    } catch (...) {
        xml.clear();
        if (cutOff) *cutOff = false;
        return Fail(error, "the code could not be decoded");
    }
}

// ---- the XML document ----------------------------------------------------------------------------

bool ParseXmlImpl(std::string_view xml, BuildData& out, std::string* error, const GemData* gems, std::vector<std::string>* warnings) {
    if (xml.size() > kMaxXmlBytes) return Fail(error, "the build XML is larger than 16 MiB");
    xml = StripBom(xml);
    if (!IsUtf8Text(xml)) return Fail(error, "the build XML is not UTF-8 text");
    Warnings warn;
    warn.out = warnings;
    XmlScanner sc(xml);
    std::vector<std::string_view> open;
    BuildData b;

    bool sawRoot = false, rootDone = false, haveBuild = false, haveTree = false, haveSkills = false, haveItems = false;
    bool poe1Root = false;   // <PathOfBuilding>: Path of Building for Path of Exile 1 writes it
    // Which section the cursor is in: each flag is set by its element's start tag and cleared by its end.
    bool inTree = false, inSkills = false, inItems = false;
    std::string activeSpecText, activeSkillSetText, activeItemSetText;
    bool itemsUseSecond = false;

    // At most kMaxSpecs trees are kept (the rest only counted); of the legacy root-level <Spec>s only
    // the last one counts (each replaces the previous in Path of Building).
    std::vector<SpecDraft> specDrafts, legacySpecDrafts;
    size_t droppedSpecs = 0, droppedItemDrafts = 0;
    const auto keepSpec = [&](SpecDraft&& d, bool fromRoot) {
        if (fromRoot) {
            legacySpecDrafts.clear();
            legacySpecDrafts.push_back(std::move(d));
        } else if (specDrafts.size() < kMaxSpecs) {
            specDrafts.push_back(std::move(d));
        } else {
            ++droppedSpecs;
        }
    };
    SpecDraft spec;
    bool inSpec = false, specFromRoot = false;
    size_t specDepth = 0;
    enum class TextSink { None, Url, Note, Item } sink = TextSink::None;
    size_t sinkDepth = 0;   // open.size() while the sink's element is the innermost one
    std::string noteText, noteAttr;   // a <Note>'s body wins over its `text` attribute (PoB)
    uint16_t noteNode = 0;
    bool noteValid = false;
    bool inSockets = false, inOverrides = false, inNotes = false;

    std::vector<SkillSetDraft> skillSets;
    SkillSetDraft legacySkills;
    bool haveLegacySkills = false;
    SkillSetDraft* curSet = nullptr;
    GroupDraft group;
    bool inGroup = false;
    size_t droppedGroups = 0, droppedGems = 0, droppedSkillSets = 0;

    struct ItemDraft {
        int id = 0;
        PobItemVariants variants;
        std::string text;
    };
    std::vector<ItemDraft> items;
    const auto keepItem = [&](ItemDraft&& d) {
        if (items.size() < kMaxItemDrafts) items.push_back(std::move(d));
        else ++droppedItemDrafts;
    };
    ItemDraft item;
    std::vector<ItemSetDraft> itemSets;
    ItemSetDraft legacySlots;
    bool haveLegacySlots = false;
    bool inItemSet = false;
    size_t droppedItemSets = 0;

    for (;;) {
        const XmlScanner::Kind k = sc.Next();
        if (k == XmlScanner::Kind::Eof) break;
        if (k == XmlScanner::Kind::Error) return Fail(error, "the build XML is malformed: " + sc.error);
        if (k == XmlScanner::Kind::Text) {
            if (sink != TextSink::None && open.size() == sinkDepth) {
                if (sink == TextSink::Url) AppendDecoded(sc.text, spec.url, false, kMaxUrlTextBytes);
                else if (sink == TextSink::Note) AppendDecoded(sc.text, noteText, false, kMaxNoteBytes * 2);
                else AppendDecoded(sc.text, item.text, false, kMaxItemTextBytes);
            }
            continue;
        }
        if (k == XmlScanner::Kind::Start) {
            const size_t depth = open.size();
            const std::string_view name = sc.name;
            if (depth == 0) {
                if (sawRoot || rootDone) return Fail(error, "the build XML has more than one root element");
                if (name != "PathOfBuilding2" && name != "PathOfBuilding")
                    return Fail(error, "not a Path of Building build (the XML root is <" + Clip(name) + ">)");
                sawRoot = true;
                poe1Root = name == "PathOfBuilding";
                if (sc.selfClosing) rootDone = true;
            } else if (depth == 1) {
                if (name == "Build" && !haveBuild) {
                    haveBuild = true;
                    b.className = CleanText(AttrText(sc, "className"), kMaxNameBytes);
                    b.ascendName = CleanText(AttrText(sc, "ascendClassName"), kMaxNameBytes);
                    if (b.ascendName == "None") b.ascendName.clear();
                    b.level = AttrInt(sc, "level", 1, 100, 0);
                } else if (name == "Tree" && !haveTree) {
                    haveTree = true;
                    inTree = !sc.selfClosing;
                    activeSpecText = AttrText(sc, "activeSpec");
                } else if (name == "Spec") {   // the legacy layout: a <Spec> directly under the root
                    spec = SpecDraft{};
                    ReadSpecAttrs(sc, spec);
                    inSpec = true;
                    specFromRoot = true;
                    specDepth = depth;
                    if (sc.selfClosing) {
                        keepSpec(std::move(spec), true);
                        spec = SpecDraft{};
                        inSpec = false;
                    }
                } else if (name == "Skills" && !haveSkills) {
                    haveSkills = true;
                    inSkills = !sc.selfClosing;
                    activeSkillSetText = AttrText(sc, "activeSkillSet");
                } else if (name == "Items" && !haveItems) {
                    haveItems = true;
                    inItems = !sc.selfClosing;
                    activeItemSetText = AttrText(sc, "activeItemSet");
                    itemsUseSecond = AttrBool(sc, "useSecondWeaponSet", false);
                }
            } else if (inTree && depth == 2 && name == "Spec") {
                spec = SpecDraft{};
                ReadSpecAttrs(sc, spec);
                inSpec = true;
                specFromRoot = false;
                specDepth = depth;
                if (sc.selfClosing) {
                    keepSpec(std::move(spec), false);
                    spec = SpecDraft{};
                    inSpec = false;
                }
            } else if (inSpec && depth == specDepth + 1) {
                if (name == "URL" && !spec.hasUrl) {
                    spec.hasUrl = true;
                    if (!sc.selfClosing) {
                        sink = TextSink::Url;
                        sinkDepth = depth + 1;
                    }
                } else if (name == "WeaponSet1" || name == "WeaponSet2") {
                    if (auto v = AttrOf(sc, "nodes"); v && spec.weaponSets.size() < kMaxListsPerSpec)
                        spec.weaponSets.emplace_back(name == "WeaponSet1" ? 1 : 2, std::move(*v));
                } else if (name == "Sockets") {
                    inSockets = !sc.selfClosing;
                } else if (name == "Overrides") {
                    inOverrides = !sc.selfClosing;
                } else if (name == "Notes") {
                    inNotes = !sc.selfClosing;
                }
            } else if (inSpec && depth == specDepth + 2) {
                if (inSockets && name == "Socket") {
                    const int node = AttrInt(sc, "nodeId", 0, 0xFFFF, -1);
                    const int itemId = AttrInt(sc, "itemId", 0, kMaxItemId, 0);
                    if (node >= 0 && itemId > 0 && spec.sockets.size() < 4096) spec.sockets.emplace_back(static_cast<uint16_t>(node), itemId);
                } else if (inOverrides && name == "AttributeOverride") {
                    if (spec.strNodes.size() < kMaxListsPerSpec) {
                        if (auto v = AttrOf(sc, "strNodes")) spec.strNodes.push_back(std::move(*v));
                        if (auto v = AttrOf(sc, "dexNodes")) spec.dexNodes.push_back(std::move(*v));
                        if (auto v = AttrOf(sc, "intNodes")) spec.intNodes.push_back(std::move(*v));
                    }
                } else if (inNotes && name == "Note") {
                    const int node = AttrInt(sc, "nodeId", 0, 0xFFFF, -1);
                    noteValid = node >= 0;
                    noteNode = static_cast<uint16_t>(noteValid ? node : 0);
                    noteText.clear();
                    noteAttr = AttrText(sc, "text");
                    if (sc.selfClosing) {
                        if (noteValid && spec.notes.size() < 4096) spec.notes.emplace_back(noteNode, std::move(noteAttr));
                        noteValid = false;
                    } else {
                        sink = TextSink::Note;
                        sinkDepth = depth + 1;
                    }
                }
            } else if (inSkills && depth == 2 && name == "SkillSet") {
                curSet = nullptr;
                if (skillSets.size() < kMaxSkillSets) {
                    SkillSetDraft d;
                    d.id = AttrInt(sc, "id", 0, INT_MAX, 0);
                    d.set.title = CleanText(AttrText(sc, "title"), kMaxTitleBytes);
                    skillSets.push_back(std::move(d));
                    curSet = &skillSets.back();
                } else {
                    ++droppedSkillSets;
                }
            } else if (inSkills && ((depth == 2 && name == "Skill") || (depth == 3 && name == "Skill" && open.back() == "SkillSet"))) {
                group = GroupDraft{};
                if (depth == 2) {   // the legacy layout: <Skill> directly under <Skills> = skill set 1
                    haveLegacySkills = true;
                    legacySkills.id = 1;
                }
                group.group.label = CleanText(AttrText(sc, "label"), kMaxNameBytes);
                // Path of Building: `active == "true" or enabled == "true"` (neither = disabled).
                group.group.enabled = AttrText(sc, "enabled") == "true" || AttrText(sc, "active") == "true";
                group.group.source = SourceOf(AttrText(sc, "source"));
                group.mainActive1 = AttrInt(sc, "mainActiveSkill", 1, 1000, 1);
                group.removed = AttrBool(sc, "removed", false);
                inGroup = true;   // a self-closing <Skill/> is finished right below
            } else if (inSkills && inGroup && name == "Gem" && (open.back() == "Skill")) {
                BuildGem g;
                int pobActives = 0;
                if (ReadGem(sc, gems, g, pobActives)) {
                    if (group.group.gems.size() < kMaxGems) {
                        group.group.gems.push_back(std::move(g));
                        group.pobActives.push_back(pobActives);
                    } else {
                        ++droppedGems;
                    }
                }
            } else if (inItems && depth == 2 && name == "Item") {
                item = ItemDraft{};
                item.id = AttrInt(sc, "id", 1, kMaxItemId, 0);
                item.variants.variant = AttrInt(sc, "variant", 1, 999, 0);
                static constexpr std::string_view kAlt[5] = {"variantAlt", "variantAlt2", "variantAlt3", "variantAlt4", "variantAlt5"};
                for (size_t a = 0; a < 5; ++a) item.variants.alt[a] = AttrInt(sc, kAlt[a], 1, 999, 0);
                if (sc.selfClosing) {
                    keepItem(std::move(item));
                } else {
                    sink = TextSink::Item;
                    sinkDepth = depth + 1;
                }
            } else if (inItems && depth == 2 && name == "ItemSet") {
                inItemSet = false;
                if (itemSets.size() < kMaxItemSets) {
                    ItemSetDraft d;
                    d.id = AttrInt(sc, "id", 0, INT_MAX, 0);
                    d.set.title = CleanText(AttrText(sc, "title"), kMaxTitleBytes);
                    d.set.useSecondWeaponSet = AttrBool(sc, "useSecondWeaponSet", false);
                    itemSets.push_back(std::move(d));
                    inItemSet = !sc.selfClosing;
                } else {
                    ++droppedItemSets;
                }
            } else if (inItems && name == "Slot" && (depth == 2 || (depth == 3 && inItemSet && open.back() == "ItemSet"))) {
                ItemSetDraft& target = depth == 2 ? legacySlots : itemSets.back();
                if (depth == 2) haveLegacySlots = true;
                const std::string slotName = CleanText(AttrText(sc, "name"), kMaxNameBytes);
                const int itemId = AttrInt(sc, "itemId", 0, kMaxItemId, 0);
                if (!slotName.empty() && target.slots.size() < 4096) target.slots.emplace_back(slotName, itemId);
            }

            if (!sc.selfClosing) {
                if (open.size() >= kMaxDepth) return Fail(error, "the build XML is nested too deeply");
                open.push_back(name);
            } else if (inGroup && name == "Skill") {
                FinishGroup(group);
                SkillSetDraft* target = depth == 2 ? &legacySkills : curSet;
                if (!group.removed && target) {
                    if (target->set.groups.size() < kMaxGroups) target->set.groups.push_back(std::move(group.group));
                    else ++droppedGroups;
                }
                inGroup = false;
            }
            continue;
        }
        // an end tag
        if (open.empty()) return Fail(error, "the build XML is malformed: </" + Clip(sc.name) + "> closes nothing");
        if (open.back() != sc.name)
            return Fail(error, "the build XML is malformed: </" + Clip(sc.name) + "> does not close <" + Clip(open.back()) + ">");
        const std::string_view name = open.back();
        open.pop_back();
        const size_t depth = open.size();   // the closed element's depth
        if (sink != TextSink::None && depth + 1 == sinkDepth) {
            if (sink == TextSink::Note) {
                if (noteValid && spec.notes.size() < 4096)
                    spec.notes.emplace_back(noteNode, Trim(noteText).empty() ? std::move(noteAttr) : std::move(noteText));
                noteText.clear();
                noteAttr.clear();
                noteValid = false;
            } else if (sink == TextSink::Item) {
                keepItem(std::move(item));
                item = ItemDraft{};
            }
            sink = TextSink::None;
            continue;
        }
        if (inSpec && depth == specDepth) {
            keepSpec(std::move(spec), specFromRoot);
            spec = SpecDraft{};
            inSpec = false;
            inSockets = inOverrides = inNotes = false;
        } else if (inSpec && depth == specDepth + 1) {
            if (name == "Sockets") inSockets = false;
            else if (name == "Overrides") inOverrides = false;
            else if (name == "Notes") inNotes = false;
        } else if (inGroup && name == "Skill" && (depth == 2 || depth == 3)) {
            FinishGroup(group);
            SkillSetDraft* target = depth == 2 ? &legacySkills : curSet;
            if (!group.removed && target) {
                if (target->set.groups.size() < kMaxGroups) target->set.groups.push_back(std::move(group.group));
                else ++droppedGroups;
            }
            inGroup = false;
        } else if (inSkills && depth == 2 && name == "SkillSet") {
            curSet = nullptr;
        } else if (inItems && depth == 2 && name == "ItemSet") {
            inItemSet = false;
        } else if (depth == 1) {
            if (name == "Tree") inTree = false;
            else if (name == "Skills") inSkills = false;
            else if (name == "Items") inItems = false;
        } else if (depth == 0) {
            rootDone = true;
        }
    }
    if (!sawRoot) return Fail(error, "not a Path of Building build (no <PathOfBuilding2> element)");
    if (!open.empty()) return Fail(error, "the build XML ends before </" + Clip(open.back()) + ">");
    // A Path of Exile 1 build: the <PathOfBuilding> root and every tree on a Path of Exile 1 tree version
    // ("3_25"; Path of Exile 2's are "0_1" ... and its root is <PathOfBuilding2>). Its passive ids are
    // not this game's: refused with the cause instead of a plan of wrong passives.
    if (poe1Root) {
        const std::vector<SpecDraft>& all = specDrafts.empty() ? legacySpecDrafts : specDrafts;
        bool poe1 = !all.empty();
        std::string version;
        for (const SpecDraft& d : all) {
            const size_t us = d.treeVersion.find('_');
            uint64_t major = 0;
            poe1 = poe1 && IsVersionText(d.treeVersion) && ParseUInt(std::string_view(d.treeVersion).substr(0, us), major) && major >= 2;
            if (version.empty()) version = d.treeVersion;
        }
        if (poe1)
            return Fail(error, "it is a Path of Exile 1 build, tree version " + Clip(version) +
                                   ": paste a build made in Path of Building for Path of Exile 2");
    }

    // Items first: the trees and the item sets refer to them by <Item id>.
    std::map<int, int> itemIndex;   // <Item id> -> index into b.items, -1 = dropped by the cap
    size_t droppedItems = 0;
    for (const ItemDraft& d : items) {
        if (d.id <= 0) continue;
        BuildItem it;
        if (!ParsePobItem(d.text, it, d.variants)) continue;
        if (b.items.size() >= kMaxItems) {
            itemIndex[d.id] = -1;
            ++droppedItems;
            continue;
        }
        itemIndex[d.id] = static_cast<int>(b.items.size());
        b.items.push_back(std::move(it));
    }
    droppedItems += droppedItemDrafts;
    if (droppedItems) warn.Add(std::to_string(droppedItems) + " items over the limit of " + std::to_string(kMaxItems) + " dropped");
    const auto resolveItem = [&itemIndex](int id, bool& known) {
        const auto it = itemIndex.find(id);
        known = it != itemIndex.end();
        return known ? it->second : -1;
    };

    // Trees: the <Tree>'s, else the last legacy root-level <Spec> (each one replaces the previous in
    // Path of Building).
    const bool fromRoot = specDrafts.empty() && !legacySpecDrafts.empty();
    const std::vector<SpecDraft>& drafts = fromRoot ? legacySpecDrafts : specDrafts;
    if (droppedSpecs) warn.Add(std::to_string(droppedSpecs) + " trees over the limit of " + std::to_string(kMaxSpecs) + " dropped");
    for (size_t i = 0; i < drafts.size() && b.specs.size() < kMaxSpecs; ++i) {
        std::vector<std::pair<uint16_t, int>> socketIds;
        BuildSpec sp = FinishSpec(drafts[i], i, warn, socketIds);
        for (const auto& [node, id] : socketIds) {
            bool known = false;
            const int idx = resolveItem(id, known);
            if (known) sp.sockets.push_back(SpecSocket{node, idx});   // an id no <Item> has is dropped, as PoB does
        }
        b.specs.push_back(std::move(sp));
    }
    if (fromRoot) {
        b.activeSpec = b.specs.empty() ? -1 : 0;
    } else {
        const int active = ParseIntIn(activeSpecText, 1, static_cast<int>(std::min<size_t>(b.specs.size(), INT_MAX)), 0);
        b.activeSpec = active >= 1 ? active - 1 : -1;
    }

    // Skill sets: the <SkillSet>s in document order; legacy <Skill>s under <Skills> are set 1.
    if (haveLegacySkills) {
        SkillSetDraft* one = nullptr;
        for (SkillSetDraft& s : skillSets)
            if (s.id == 1) one = &s;
        if (one) {
            for (BuildSkillGroup& g : legacySkills.set.groups)
                if (one->set.groups.size() < kMaxGroups) one->set.groups.push_back(std::move(g));
        } else if (skillSets.size() < kMaxSkillSets) {
            skillSets.insert(skillSets.begin(), std::move(legacySkills));
        }
    }
    if (droppedSkillSets) warn.Add(std::to_string(droppedSkillSets) + " skill sets over the limit of " + std::to_string(kMaxSkillSets) + " dropped");
    if (droppedGroups) warn.Add(std::to_string(droppedGroups) + " skill groups over the limit of " + std::to_string(kMaxGroups) + " per set dropped");
    if (droppedGems) warn.Add(std::to_string(droppedGems) + " gems over the limit of " + std::to_string(kMaxGems) + " per group dropped");
    // The active sets are ids; an absent attribute is set 1 (Path of Building's default).
    const int activeSkillId = activeSkillSetText.empty() ? 1 : ParseIntIn(activeSkillSetText, 0, INT_MAX, -1);
    for (size_t i = 0; i < skillSets.size(); ++i) {
        if (activeSkillId >= 0 && skillSets[i].id == activeSkillId && b.activeSkillSet < 0) b.activeSkillSet = static_cast<int>(i);
        b.skillSets.push_back(std::move(skillSets[i].set));
    }

    // Item sets: the <ItemSet>s in document order; a legacy <Slot> list under <Items> is the only set
    // when there is no <ItemSet>.
    if (itemSets.empty() && haveLegacySlots) {
        legacySlots.id = 1;
        legacySlots.set.useSecondWeaponSet = itemsUseSecond;
        itemSets.push_back(std::move(legacySlots));
    }
    if (droppedItemSets) warn.Add(std::to_string(droppedItemSets) + " item sets over the limit of " + std::to_string(kMaxItemSets) + " dropped");
    const int activeItemId = activeItemSetText.empty() ? 1 : ParseIntIn(activeItemSetText, 0, INT_MAX, -1);
    for (size_t i = 0; i < itemSets.size(); ++i) {
        ItemSetDraft& d = itemSets[i];
        // A slot named twice keeps its first place and its last item (Path of Building's table).
        for (const auto& [slotName, itemId] : d.slots) {
            bool known = false;
            const int idx = itemId > 0 ? resolveItem(itemId, known) : -1;
            if (idx < 0 && GearSlotFromPobName(slotName) == kNoGearSlot) continue;
            auto same = std::find_if(d.set.slots.begin(), d.set.slots.end(), [&](const BuildItemSlot& s) { return s.name == slotName; });
            if (same != d.set.slots.end()) {
                same->itemIndex = idx;
                continue;
            }
            if (d.set.slots.size() >= kMaxSetSlots) continue;
            d.set.slots.push_back(BuildItemSlot{slotName, idx});
        }
        if (activeItemId >= 0 && d.id == activeItemId && b.activeItemSet < 0) b.activeItemSet = static_cast<int>(i);
        b.itemSets.push_back(std::move(d.set));
    }
    if (warn.count > kMaxWarnings && warnings) warnings->push_back(std::to_string(warn.count - kMaxWarnings) + " more warnings not shown");

    out = std::move(b);
    return true;
}

// The body of a download: a code, or raw XML.
bool DecodeBody(std::string_view raw, const std::string& url, const GemData* gems, ImportResult& r) {
    const std::string_view body = Trim(StripBom(Trim(raw)));
    std::string err;
    if (body.empty()) {
        r.problem = ImportProblem::Download;
        r.detail = "the download from " + url + " was empty";
        return false;
    }
    if (body[0] == '<') {
        if (LooksLikeHtml(body)) {
            r.problem = ImportProblem::Download;
            r.detail = "the site sent a web page instead of a build (" + url + "): the paste may be gone or private";
            return false;
        }
        if (!ParsePobXml(body, r.build, &err, gems, &r.warnings)) {
            r.problem = ImportProblem::Parse;
            r.detail = err;
            return false;
        }
        return true;
    }
    std::string xml;
    if (!DecodePobCode(body, xml, &err)) {
        r.problem = ImportProblem::Decode;
        r.detail = "the download is not a Path of Building code: " + DecodeFailureText(err);
        return false;
    }
    if (!ParsePobXml(xml, r.build, &err, gems, &r.warnings)) {
        r.problem = ImportProblem::Parse;
        r.detail = err;
        return false;
    }
    return true;
}

bool IsCancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }

// One download through the seam. false with r.problem / r.detail set.
bool Download(const HttpGetFn& http, const std::string& url, const std::atomic<bool>* cancel, ImportResult& r, HttpResult& h) {
    if (IsCancelled(cancel)) {
        r.problem = ImportProblem::Cancelled;
        r.detail = "cancelled";
        return false;
    }
    if (!http) {
        r.problem = ImportProblem::Download;
        r.detail = "downloads are not available";
        return false;
    }
    h = http(url, kImportMaxBytes, kImportTimeoutMs, cancel);
    if (h.status > 0) r.httpStatus = h.status;
    if (h.error == "cancelled" || IsCancelled(cancel)) {
        r.problem = ImportProblem::Cancelled;
        r.detail = "cancelled";
        return false;
    }
    if (h.status == 404) {
        r.problem = ImportProblem::NotFound;
        r.detail = "HTTP status 404 from " + url;
        return false;
    }
    if (!h.Ok()) {
        const std::string why = !h.error.empty() ? h.error : "HTTP status " + std::to_string(h.status);
        r.problem = ImportProblem::Download;
        r.detail = "the build could not be downloaded from " + url + ": " + why;
        return false;
    }
    return true;
}

// The poe.ninja character page: the snapshot of the league, then the character with its PoB export.
bool ImportNinjaCharacter(const Classified& c, const std::atomic<bool>* cancel, const HttpGetFn& http, const GemData* gems, ImportResult& r) {
    using json = nlohmann::json;
    HttpResult h;
    if (!Download(http, kNinjaIndexUrl, cancel, r, h)) return false;
    const json index = json::parse(h.body.begin(), h.body.end(), nullptr, false);
    std::string snapshotName, version;
    const auto str = [](const json& o, const char* key) -> std::string {
        const auto it = o.find(key);
        return (it != o.end() && it->is_string() && it->get_ref<const std::string&>().size() <= 256) ? it->get<std::string>() : std::string();
    };
    if (!index.is_discarded() && index.is_object()) {
        const auto list = index.find("snapshotVersions");
        if (list != index.end() && list->is_array()) {
            for (int pass = 0; pass < 2 && snapshotName.empty(); ++pass) {
                for (const json& s : *list) {
                    if (!s.is_object()) continue;
                    const std::string key = str(s, pass == 0 ? "url" : "snapshotName");
                    if (key.empty() || !EqualsI(key, c.league)) continue;
                    snapshotName = str(s, "snapshotName");
                    version = str(s, "version");
                    if (!snapshotName.empty()) break;
                }
            }
        }
    } else {
        r.problem = ImportProblem::Download;
        r.detail = "poe.ninja's league list could not be read";
        return false;
    }
    if (snapshotName.empty()) {
        r.problem = ImportProblem::NotFound;
        r.detail = "poe.ninja lists no build snapshot for the league \"" + Clip(c.league) + "\"";
        return false;
    }
    bool versionOk = !version.empty() && version.size() <= 64;
    for (const char ch : version) versionOk = versionOk && (IsIdChar(ch) || ch == '.');
    if (!versionOk) version = "current";
    const std::string url = "https://poe.ninja/poe2/api/builds/" + version + "/character?account=" + PercentEncode(c.account) +
                            "&name=" + PercentEncode(c.character) + "&overview=" + PercentEncode(snapshotName);
    if (!Download(http, url, cancel, r, h)) {
        if (r.problem == ImportProblem::NotFound) r.detail = "poe.ninja has no such character in that league (HTTP status 404)";
        return false;
    }
    const json ch = json::parse(h.body.begin(), h.body.end(), nullptr, false);
    std::string code;
    if (!ch.is_discarded() && ch.is_object()) {
        const auto it = ch.find("pathOfBuildingExport");
        if (it != ch.end() && it->is_string()) code = it->get<std::string>();
    }
    if (Trim(code).empty()) {
        r.problem = ImportProblem::Parse;
        r.detail = "poe.ninja's answer holds no Path of Building export for this character";
        return false;
    }
    std::string xml, err;
    if (!DecodePobCode(code, xml, &err)) {
        r.problem = ImportProblem::Decode;
        r.detail = "poe.ninja's Path of Building export: " + DecodeFailureText(err);
        return false;
    }
    if (!ParsePobXml(xml, r.build, &err, gems, &r.warnings)) {
        r.problem = ImportProblem::Parse;
        r.detail = err;
        return false;
    }
    return true;
}

} // namespace

// ---- public ------------------------------------------------------------------------------------

Classified ClassifyInput(std::string_view text) {
    try {
        return ClassifyImpl(text);
    } catch (...) {
        return Classified{};
    }
}

bool StartsImport(InputKind kind) {
    switch (kind) {
    case InputKind::PobCode:
    case InputKind::PobXml:
    case InputKind::PobbinLink:
    case InputKind::PoeNinjaLink:
    case InputKind::MaxrollPobLink:
    case InputKind::Poe2dbLink:
    case InputKind::PastebinLink:
    case InputKind::PoeNinjaCharacter: return true;
    default: return false;
    }
}

bool Base64UrlDecode(std::string_view text, std::vector<uint8_t>& out) {
    try {
        return DecodeB64(text, out) == B64Status::Ok;
    } catch (...) {
        out.clear();
        return false;
    }
}

std::string Base64UrlEncode(const uint8_t* data, size_t size) {
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    try {
        if (!data || size == 0 || size > (SIZE_MAX / 4 - 4)) return {};
        std::string out;
        out.reserve((size + 2) / 3 * 4);
        size_t i = 0;
        for (; i + 3 <= size; i += 3) {
            const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
            out.push_back(kAlphabet[(v >> 18) & 63]);
            out.push_back(kAlphabet[(v >> 12) & 63]);
            out.push_back(kAlphabet[(v >> 6) & 63]);
            out.push_back(kAlphabet[v & 63]);
        }
        if (size - i == 1) {
            const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
            out.push_back(kAlphabet[(v >> 18) & 63]);
            out.push_back(kAlphabet[(v >> 12) & 63]);
        } else if (size - i == 2) {
            const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
            out.push_back(kAlphabet[(v >> 18) & 63]);
            out.push_back(kAlphabet[(v >> 12) & 63]);
            out.push_back(kAlphabet[(v >> 6) & 63]);
        }
        return out;
    } catch (...) {
        return {};
    }
}

bool DecodePobCode(std::string_view code, std::string& xml, std::string* error) { return DecodePobCodeImpl(code, xml, error, nullptr); }

bool ParsePobXml(std::string_view xml, BuildData& out, std::string* error, const GemData* gems, std::vector<std::string>* warnings) {
    out = BuildData{};
    try {
        BuildData b;
        if (!ParseXmlImpl(xml, b, error, gems, warnings)) return false;
        out = std::move(b);
        return true;
    } catch (const std::bad_alloc&) {
        out = BuildData{};
        return Fail(error, "out of memory while reading the build XML");
    } catch (...) {
        out = BuildData{};
        return Fail(error, "the build XML could not be read");
    }
}

std::string StripPobColors(std::string_view text) {
    try {
        std::string out;
        out.reserve(text.size());
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '^' && i + 1 < text.size()) {
                const char n = text[i + 1];
                if (n >= '0' && n <= '9') {
                    ++i;
                    continue;
                }
                if (n == 'x' && i + 7 < text.size()) {
                    bool hex = true;
                    for (size_t k = i + 2; k <= i + 7; ++k) {
                        const char h = text[k];
                        if (!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F'))) {
                            hex = false;
                            break;
                        }
                    }
                    if (hex) {
                        i += 7;
                        continue;
                    }
                }
            }
            out.push_back(text[i]);
        }
        return out;
    } catch (...) {
        return {};
    }
}

int ClassIdFromLegacy(std::string_view treeVersion, int legacyClassId) {
    if (legacyClassId < 0) return -1;
    const std::string_view v = treeVersion.empty() ? std::string_view("0_1") : treeVersion;
    for (const LegacyClasses& m : kLegacyClassMaps)
        if (m.version == v) return legacyClassId < 8 ? m.classByIndex[legacyClassId] : -1;
    if (v == "0_4" || v == "0_5") {
        for (const int id : kKnownClassIds)
            if (id == legacyClassId) return id;
        return -1;
    }
    // A newer tree than this table knows: the attribute is the integerId (as from 0_4 on).
    return (legacyClassId >= 1 && legacyClassId <= 255) ? legacyClassId : -1;
}

std::string AscendancyIdFromLegacy(std::string_view treeVersion, int classIntegerId, int ascendClassId) {
    if (ascendClassId <= 0 || ascendClassId > 4 || classIntegerId < 0) return {};
    const std::string_view v = treeVersion.empty() ? std::string_view("0_1") : treeVersion;
    const VersionAscendancies* table = &kAscendancies[std::size(kAscendancies) - 1];   // unknown: the newest
    if (KnownVersion(v))
        for (const VersionAscendancies& va : kAscendancies)
            if (va.version == v) table = &va;
    for (const AscendancyList& l : table->classes)
        if (l.classId == classIntegerId) return std::string(l.ids[ascendClassId - 1]);
    return {};
}

ImportResult ImportFromText(std::string_view text, const std::atomic<bool>* cancel, const HttpGetFn& http, const GemData* gems) {
    ImportResult r;
    auto fail = [&r](ImportProblem p, std::string detail) {
        r.ok = false;
        r.problem = p;
        r.detail = std::move(detail);
        r.build = BuildData{};
        return r;
    };
    try {
        const std::string_view t = UnwrapAngleLink(Trim(StripBom(Trim(text))));   // the link is the source, not "<link>"
        const Classified c = ClassifyImpl(t);
        r.kind = c.kind;
        if (IsCancelled(cancel)) return fail(ImportProblem::Cancelled, "cancelled");
        std::string err;
        switch (c.kind) {
        case InputKind::Empty: return fail(ImportProblem::Empty, "nothing to import");
        case InputKind::Unknown:
            return fail(ImportProblem::Unrecognized,
                        "not a Path of Building code, PoB XML or a pobb.in / poe.ninja / maxroll / poe2db / pastebin link");
        case InputKind::MaxrollGuide:
            return fail(ImportProblem::MaxrollGuide,
                        "a maxroll build guide or planner link holds no Path of Building code: open the guide's Path of Building "
                        "link (maxroll.gg/poe2/pob/...) and paste that");
        case InputKind::PobCode: {
            r.source = "PoB code";
            std::string xml;
            if (!DecodePobCodeImpl(t, xml, &err, nullptr)) return fail(ImportProblem::Decode, DecodeFailureText(err));
            if (!ParsePobXml(xml, r.build, &err, gems, &r.warnings)) return fail(ImportProblem::Parse, err);
            break;
        }
        case InputKind::PobXml:
            r.source = "PoB XML";
            if (!ParsePobXml(t, r.build, &err, gems, &r.warnings)) return fail(ImportProblem::Parse, err);
            break;
        case InputKind::PobbinLink:
        case InputKind::PoeNinjaLink:
        case InputKind::MaxrollPobLink:
        case InputKind::Poe2dbLink:
        case InputKind::PastebinLink: {
            r.source = Utf8Clamp(t, kMaxSourceBytes);
            HttpResult h;
            if (!Download(http, c.fetchUrl, cancel, r, h)) return fail(r.problem, r.detail);
            if (!DecodeBody(h.body, c.fetchUrl, gems, r)) return fail(r.problem, r.detail);
            break;
        }
        case InputKind::PoeNinjaCharacter:
            r.source = Utf8Clamp(t, kMaxSourceBytes);
            if (!ImportNinjaCharacter(c, cancel, http, gems, r)) return fail(r.problem, r.detail);
            break;
        }
        bool anyNodes = false;
        for (const BuildSpec& s : r.build.specs)
            anyNodes = anyNodes || !s.nodes.empty();
        if (!anyNodes) return fail(ImportProblem::NoSpecs, "the build has no passive tree with allocated points");
        r.ok = true;
        r.problem = ImportProblem::None;
        r.detail.clear();
        return r;
    } catch (const std::bad_alloc&) {
        return fail(ImportProblem::Decode, "out of memory while importing");
    } catch (...) {
        return fail(ImportProblem::Decode, "the import failed");
    }
}

} // namespace bp
