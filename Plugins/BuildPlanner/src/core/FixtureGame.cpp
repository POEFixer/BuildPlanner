// BuildPlanner - an IGame replaying a capture file (.mem)
// FixtureGame.cpp - an IGame replaying a capture file (see FixtureGame.h).
//
// ---- The capture file (.mem), format version 1 (little endian) ----------------------------------
//   offset   size  field
//   0        8     magic "BPFIXMEM"
//   8        4     u32 format version (1)
//   12       4     u32 H: header bytes (1 .. 1 MiB)
//   16       H     the header: one UTF-8 JSON object (below)
//   16+H     4     u32 N: region count (<= 2,000,000)
//   20+H     8     u64 T: the regions' bytes together (<= 256 MiB)
//   28+H     4     u32 C: compressed bytes; the file ends right after them
//   32+H     C     a zlib stream that inflates to exactly 12 * N + T bytes: N region headers
//                  {u64 address, u32 size}, then the N regions' bytes in the same order. Regions are
//                  sorted by address, size >= 1, they neither overlap nor wrap.
// A read succeeds only when all its bytes lie inside ONE region (tools/make_fixture.py merges what
// touches), so a byte the capture does not hold is an unreadable byte, as in a game process.
//
// The header (unknown keys are ignored; addresses are "0x..." strings or JSON integers):
//   "format"      "BuildPlanner capture"
//   "source"      {snapshot, tool, note}: diagnostics only
//   "module"      {base, size}                       ModuleBase / ModuleSize
//   "client"      {w, h, cull, baseW, baseH}         the client size and the host's GameCullValue /
//                                                    base resolution in UiRect's formula
//   "status"      {attached, inGame, treeVisible, areaSerial}   Status() (client size from "client")
//   "player"      {valid, level, name, path}         Player()
//   "ui"          {gameUiRoot, inventoryPanelRoot, hostTreePanelSlot}
//                 hostTreePanelSlot (optional): the host's own UiRootStruct slot of the tree panel;
//                 when given, Status().treeVisible is that panel's visible bit read from the capture
//                 (so an edit that hides or moves the panel shows), else status.treeVisible
//   "uiOffsets"   {self, children, controlName, parent, relPos, posMod, scaleMul, flags, type,
//                  scaleIndex, size, text, visibleBit, modifyBit}: the captured build's UiElementBase
//   "inventories" [{hostId, address, boxesX, boxesY, grid: [x, y, cell] (optional),
//                   scan: {supported, ready, stamp, areaSerial,
//                          items: [{entity, x, y, w, h, path, baseName, uniqueName}]} (optional)}]
//                 InvGet / InvRead; an id not listed reads as not published (InvGet valid == false,
//                 InvRead supported but not ready). InvScan is a no-op: a capture is one moment.
//   "items"       [{entity, path, baseName, uniqueName, rarity (0..3), englishName}]
//                 ItemRead / ItemEnglishName by entity address
//   "expect"      what tests check (tools/make_fixture.py writes it; the game never reads it)
//
// Everything a reader decodes from memory is decoded here as the host decodes it
// (plugin_manager/bridge/Bridge_Ui.cpp): UiRead's child count (<= 4096, valid first pointer), the
// parent check, the visible / modify bits, ComputeScreenRect(s)' recursive formula with the base
// resolution and the cull, ReadStdWString's bounds, FindElements' breadth-first search.
#include "FixtureGame.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include <miniz/miniz.h>
#include <nlohmann/json.hpp>

#include "JsonFile.h"

namespace bp {
namespace {

using json = nlohmann::json;

constexpr char kMagic[8] = {'B', 'P', 'F', 'I', 'X', 'M', 'E', 'M'};
constexpr uint32_t kFormatVersion = 1;
constexpr size_t kMaxFileBytes = 64u << 20;
constexpr uint32_t kMaxHeaderBytes = 1u << 20;
constexpr uint32_t kMaxRegions = 2000000;
constexpr uint64_t kMaxRegionBytes = 256ull << 20;
constexpr int kMaxRectDepth = 32;          // the host's GetUnScaledPosition depth limit
constexpr int kHostMaxChildren = 4096;     // ui_read / ui_enumerate_children
constexpr int kFindMaxDepth = 32;          // UiSearch::kMaxDepth
constexpr int kFindBudget = 4000;          // UiSearch::kNodeBudget
constexpr size_t kFindChildLimit = 2048;   // UiSearch::kChildScanLimit
constexpr int64_t kMaxStringUnits = 1000;  // ReadStdWString

struct UiLayout {
    uint32_t children = 0x10, controlName = 0x98, parent = 0xB8, relPos = 0x100, posMod = 0x108, scaleMul = 0x118,
             flags = 0x168, type = 0x170, scaleIndex = 0x172, size = 0x270, text = 0x360;
    uint32_t visibleBit = 11, modifyBit = 10;
};

struct ItemEntry {
    ItemInfo info;
    std::string englishName;
};

// The capture as loaded; shared (immutable) by every game opened or derived from it.
struct Data {
    std::vector<uint64_t> starts, ends;    // region i covers [starts[i], ends[i])
    std::vector<size_t> offsets;           // region i's bytes start at bytes[offsets[i]]
    std::string bytes;

    UiLayout ui;
    int clientW = 0, clientH = 0, cull = 0;
    float baseW = 2560.0f, baseH = 1600.0f;
    GameStatus status;
    PlayerInfo player;
    uint64_t moduleBase = 0, moduleSize = 0, gameUiRoot = 0, inventoryPanelRoot = 0;
    uint32_t hostTreePanelSlot = 0;        // 0 = use status.treeVisible
    std::unordered_map<int, InvSnapshot> inventories;   // by host id; snapshot.info = InvGet's answer
    std::unordered_map<uint64_t, ItemEntry> items;
};

bool Fail(std::string* error, const std::string& what) {
    if (error) *error = what;
    return false;
}

template <class T>
T Load(const char* p) {
    T v{};
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// "0x..." strings, decimal strings and JSON integers.
bool AsU64(const json& v, uint64_t& out) {
    if (v.is_number_unsigned()) {
        out = v.get<uint64_t>();
        return true;
    }
    if (v.is_number_integer()) {
        const int64_t n = v.get<int64_t>();
        if (n < 0) return false;
        out = static_cast<uint64_t>(n);
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
    if (i >= s.size() || s.size() - i > 16) return false;
    uint64_t n = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        uint64_t d;
        if (c >= '0' && c <= '9') d = static_cast<uint64_t>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = static_cast<uint64_t>(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = static_cast<uint64_t>(c - 'A' + 10);
        else return false;
        if (base == 10 && n > (UINT64_MAX - d) / 10) return false;
        n = n * static_cast<uint64_t>(base) + d;
    }
    out = n;
    return true;
}

uint64_t U64(const json& o, const char* key, uint64_t def = 0) {
    if (!o.is_object()) return def;
    const auto it = o.find(key);
    uint64_t v = 0;
    return (it != o.end() && AsU64(*it, v)) ? v : def;
}

int Int(const json& o, const char* key, int def = 0) {
    const uint64_t v = U64(o, key, static_cast<uint64_t>(def < 0 ? 0 : def));
    return v > 0x7FFFFFFFull ? def : static_cast<int>(v);
}

float Float(const json& o, const char* key, float def) {
    if (!o.is_object()) return def;
    const auto it = o.find(key);
    if (it == o.end() || !it->is_number()) return def;
    const double d = it->get<double>();
    return std::isfinite(d) ? static_cast<float>(d) : def;
}

bool Bool(const json& o, const char* key, bool def) {
    if (!o.is_object()) return def;
    const auto it = o.find(key);
    return (it != o.end() && it->is_boolean()) ? it->get<bool>() : def;
}

std::string Str(const json& o, const char* key) {
    if (!o.is_object()) return {};
    const auto it = o.find(key);
    return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

void ParseHeader(const json& h, Data& d) {
    const json& ui = h.value("uiOffsets", json::object());
    UiLayout& l = d.ui;
    const auto off = [&](const char* key, uint32_t& field) {
        const uint64_t v = U64(ui, key, field);
        if (v < 0x10000) field = static_cast<uint32_t>(v);   // an element is far smaller; anything else is a typo
    };
    off("children", l.children);
    off("controlName", l.controlName);
    off("parent", l.parent);
    off("relPos", l.relPos);
    off("posMod", l.posMod);
    off("scaleMul", l.scaleMul);
    off("flags", l.flags);
    off("type", l.type);
    off("scaleIndex", l.scaleIndex);
    off("size", l.size);
    off("text", l.text);
    l.visibleBit = static_cast<uint32_t>(std::min(31, Int(ui, "visibleBit", 11)));
    l.modifyBit = static_cast<uint32_t>(std::min(31, Int(ui, "modifyBit", 10)));

    const json& c = h.value("client", json::object());
    d.clientW = Int(c, "w");
    d.clientH = Int(c, "h");
    d.cull = Int(c, "cull");
    d.baseW = Float(c, "baseW", 2560.0f);
    d.baseH = Float(c, "baseH", 1600.0f);
    if (!(d.baseW > 0.0f) || !(d.baseH > 0.0f)) {
        d.baseW = 2560.0f;
        d.baseH = 1600.0f;
    }

    const json& s = h.value("status", json::object());
    d.status.attached = Bool(s, "attached", true);
    d.status.inGame = Bool(s, "inGame", true);
    d.status.treeVisible = Bool(s, "treeVisible", false);
    d.status.areaSerial = U64(s, "areaSerial", 1);

    const json& p = h.value("player", json::object());
    d.player.valid = Bool(p, "valid", false);
    d.player.level = Int(p, "level");
    d.player.name = Str(p, "name");
    d.player.path = Str(p, "path");

    const json& m = h.value("module", json::object());
    d.moduleBase = U64(m, "base");
    d.moduleSize = U64(m, "size");

    const json& u = h.value("ui", json::object());
    d.gameUiRoot = U64(u, "gameUiRoot");
    d.inventoryPanelRoot = U64(u, "inventoryPanelRoot");
    const uint64_t slot = U64(u, "hostTreePanelSlot");
    d.hostTreePanelSlot = slot < 0x100000 ? static_cast<uint32_t>(slot) : 0;

    const json& invs = h.value("inventories", json::array());
    if (invs.is_array()) {
        for (const json& e : invs) {
            if (!e.is_object()) continue;
            InvSnapshot snap;
            InvInfo& info = snap.info;
            info.valid = true;
            info.hostId = Int(e, "hostId");
            info.address = U64(e, "address");
            info.boxesX = Int(e, "boxesX");
            info.boxesY = Int(e, "boxesY");
            const json grid = e.value("grid", json());
            if (grid.is_array() && grid.size() == 3 && grid[0].is_number() && grid[1].is_number() && grid[2].is_number()) {
                info.gridValid = true;
                info.gridX = grid[0].get<float>();
                info.gridY = grid[1].get<float>();
                info.cell = grid[2].get<float>();
            }
            snap.supported = true;
            const json& scan = e.value("scan", json());
            if (scan.is_object()) {
                snap.supported = Bool(scan, "supported", true);
                snap.ready = Bool(scan, "ready", true);
                snap.scanStamp = U64(scan, "stamp", 1);
                snap.areaSerial = U64(scan, "areaSerial", d.status.areaSerial);
                const json& items = scan.value("items", json::array());
                if (items.is_array()) {
                    for (const json& it : items) {
                        if (!it.is_object()) continue;
                        InvItem item;
                        item.entity = U64(it, "entity");
                        item.slotX = Int(it, "x");
                        item.slotY = Int(it, "y");
                        item.w = Int(it, "w");
                        item.h = Int(it, "h");
                        item.path = Str(it, "path");
                        item.baseName = Str(it, "baseName");
                        item.uniqueName = Str(it, "uniqueName");
                        snap.items.push_back(std::move(item));
                    }
                }
            }
            d.inventories[info.hostId] = std::move(snap);
        }
    }
    const json& items = h.value("items", json::array());
    if (items.is_array()) {
        for (const json& e : items) {
            if (!e.is_object()) continue;
            ItemEntry entry;
            entry.info.valid = true;
            entry.info.path = Str(e, "path");
            entry.info.baseName = Str(e, "baseName");
            entry.info.uniqueName = Str(e, "uniqueName");
            const int rarity = Int(e, "rarity");
            entry.info.rarity = static_cast<Rarity>(rarity >= 0 && rarity <= 3 ? rarity : 0);
            entry.englishName = Str(e, "englishName");
            d.items[U64(e, "entity")] = std::move(entry);
        }
    }
}

// Splits a capture file into its header text and the inflated region table.
bool ParseFile(const std::string& file, std::string& headerText, std::string* inflated, uint32_t* regionCount,
               uint64_t* regionBytes, std::string* error) {
    if (file.size() < 16) return Fail(error, "the capture file is too short");
    if (std::memcmp(file.data(), kMagic, sizeof(kMagic)) != 0) return Fail(error, "not a BuildPlanner capture (bad magic)");
    const uint32_t version = Load<uint32_t>(file.data() + 8);
    if (version != kFormatVersion) return Fail(error, "capture format version " + std::to_string(version) + " is not supported");
    const uint32_t h = Load<uint32_t>(file.data() + 12);
    if (h == 0 || h > kMaxHeaderBytes || file.size() - 16 < h) return Fail(error, "the capture header is truncated or too large");
    headerText.assign(file.data() + 16, h);
    if (!inflated) return true;
    const size_t at = 16 + static_cast<size_t>(h);
    if (file.size() - at < 16) return Fail(error, "the capture file is truncated");
    const uint32_t n = Load<uint32_t>(file.data() + at);
    const uint64_t t = Load<uint64_t>(file.data() + at + 4);
    const uint32_t c = Load<uint32_t>(file.data() + at + 12);
    if (n > kMaxRegions) return Fail(error, "the capture holds too many regions");
    if (t > kMaxRegionBytes) return Fail(error, "the capture holds over 256 MiB");
    if (static_cast<uint64_t>(file.size() - at - 16) != c) return Fail(error, "the capture's compressed size does not match the file");
    const uint64_t want = static_cast<uint64_t>(n) * 12u + t;
    if (want == 0) {
        if (c != 0) {
            // An empty table still compresses to a few bytes; accept only an inflate of nothing.
            std::string none(1, '\0');
            mz_ulong got = 1;
            if (mz_uncompress(reinterpret_cast<unsigned char*>(none.data()), &got,
                              reinterpret_cast<const unsigned char*>(file.data() + at + 16), c) != MZ_OK || got != 0)
                return Fail(error, "the capture's region table does not inflate");
        }
        inflated->clear();
        *regionCount = 0;
        *regionBytes = 0;
        return true;
    }
    inflated->assign(static_cast<size_t>(want), '\0');
    mz_ulong got = static_cast<mz_ulong>(want);
    if (mz_uncompress(reinterpret_cast<unsigned char*>(inflated->data()), &got,
                      reinterpret_cast<const unsigned char*>(file.data() + at + 16), c) != MZ_OK ||
        got != static_cast<mz_ulong>(want))
        return Fail(error, "the capture's region table does not inflate to its stated size");
    *regionCount = n;
    *regionBytes = t;
    return true;
}

bool LoadData(const std::filesystem::path& path, Data& d, std::string* error) {
    const FileRead r = ReadFileCapped(path, kMaxFileBytes);
    if (r.status == FileStatus::Missing) return Fail(error, "the capture file does not exist: " + PathUtf8(path));
    if (r.status == FileStatus::TooLarge) return Fail(error, "the capture file is over 64 MiB");
    if (!r.Ok()) return Fail(error, "the capture file could not be read: " + r.error);
    std::string headerText, table;
    uint32_t n = 0;
    uint64_t t = 0;
    if (!ParseFile(r.bytes, headerText, &table, &n, &t, error)) return false;
    const json h = json::parse(headerText, nullptr, false);
    if (h.is_discarded() || !h.is_object()) return Fail(error, "the capture header is not a JSON object");
    ParseHeader(h, d);

    d.starts.resize(n);
    d.ends.resize(n);
    d.offsets.resize(n);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const uint64_t a = Load<uint64_t>(table.data() + static_cast<size_t>(i) * 12);
        const uint32_t size = Load<uint32_t>(table.data() + static_cast<size_t>(i) * 12 + 8);
        if (a == 0 || size == 0) return Fail(error, "a capture region is empty or at address 0");
        if (a > UINT64_MAX - size) return Fail(error, "a capture region wraps the address space");
        if (i > 0 && a < d.ends[i - 1]) return Fail(error, "the capture's regions overlap or are not sorted");
        if (size > t - sum) return Fail(error, "the capture's regions hold more than their stated total");
        d.starts[i] = a;
        d.ends[i] = a + size;
        d.offsets[i] = static_cast<size_t>(sum);
        sum += size;
    }
    if (sum != t) return Fail(error, "the capture's regions hold less than their stated total");
    d.bytes = table.substr(static_cast<size_t>(n) * 12);
    return true;
}

// Lays patches over a copy of `base`: the union of the regions and the patches, adjacent pieces merged.
std::shared_ptr<const Data> Patched(const Data& base, const std::vector<FixturePatch>& patches) {
    struct Span {
        uint64_t a, b;
    };
    std::vector<Span> spans;
    spans.reserve(base.starts.size() + patches.size());
    for (size_t i = 0; i < base.starts.size(); ++i) spans.push_back({base.starts[i], base.ends[i]});
    for (const FixturePatch& p : patches)
        if (p.address != 0 && !p.bytes.empty() && p.address <= UINT64_MAX - p.bytes.size())
            spans.push_back({p.address, p.address + p.bytes.size()});
    std::sort(spans.begin(), spans.end(), [](const Span& x, const Span& y) { return x.a < y.a; });
    std::vector<Span> merged;
    for (const Span& s : spans) {
        if (!merged.empty() && s.a <= merged.back().b) merged.back().b = std::max(merged.back().b, s.b);
        else merged.push_back(s);
    }
    auto out = std::make_shared<Data>();
    Data& d = *out;
    d.ui = base.ui;
    d.clientW = base.clientW;
    d.clientH = base.clientH;
    d.cull = base.cull;
    d.baseW = base.baseW;
    d.baseH = base.baseH;
    d.status = base.status;
    d.player = base.player;
    d.moduleBase = base.moduleBase;
    d.moduleSize = base.moduleSize;
    d.gameUiRoot = base.gameUiRoot;
    d.inventoryPanelRoot = base.inventoryPanelRoot;
    d.hostTreePanelSlot = base.hostTreePanelSlot;
    d.inventories = base.inventories;
    d.items = base.items;
    size_t total = 0;
    for (const Span& s : merged) {
        d.starts.push_back(s.a);
        d.ends.push_back(s.b);
        d.offsets.push_back(total);
        total += static_cast<size_t>(s.b - s.a);
    }
    d.bytes.assign(total, '\0');
    const auto place = [&](uint64_t a, const char* src, size_t n) {
        const size_t i = static_cast<size_t>(std::upper_bound(d.starts.begin(), d.starts.end(), a) - d.starts.begin()) - 1;
        std::memcpy(d.bytes.data() + d.offsets[i] + (a - d.starts[i]), src, n);
    };
    for (size_t i = 0; i < base.starts.size(); ++i)
        place(base.starts[i], base.bytes.data() + base.offsets[i], static_cast<size_t>(base.ends[i] - base.starts[i]));
    for (const FixturePatch& p : patches)
        if (p.address != 0 && !p.bytes.empty() && p.address <= UINT64_MAX - p.bytes.size())
            place(p.address, p.bytes.data(), p.bytes.size());
    return out;
}

uint64_t SplitMix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
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

std::string Utf16ToUtf8(const uint16_t* s, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00u);
            ++i;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            c = 0xFFFD;
        }
        AppendUtf8(out, c);
    }
    return out;
}

bool ValidUserPointer(uint64_t p) { return p >= 0x10000ull && p <= 0x00007FFFFFFFFFFFull; }

// The UiElementBase fields the host's element read takes, raw.
struct RawEl {
    uint64_t first = 0, last = 0, parent = 0;
    float relX = 0, relY = 0, modX = 0, modY = 0, mult = 0, w = 0, h = 0;
    uint32_t flags = 0;
    uint16_t type = 0;
    uint8_t scaleIndex = 0;
};

struct Pos {
    float x = 0, y = 0;
};

class FixtureGameImpl final : public IGame {
public:
    FixtureGameImpl(std::shared_ptr<const Data> data, std::vector<std::pair<uint64_t, uint64_t>> hidden, int clientW,
                    int clientH, uint32_t failEvery, uint32_t garbageOneIn, uint64_t seed)
        : m_d(std::move(data)), m_hidden(std::move(hidden)), m_clientW(clientW), m_clientH(clientH), m_failEvery(failEvery),
          m_garbageOneIn(garbageOneIn), m_seed(seed) {}

    const std::shared_ptr<const Data>& DataPtr() const { return m_d; }
    const std::vector<std::pair<uint64_t, uint64_t>>& Hidden() const { return m_hidden; }
    int ClientW() const { return m_clientW; }
    int ClientH() const { return m_clientH; }

    // ---- IGame ----
    GameStatus Status() override {
        GameStatus s = m_d->status;
        s.clientW = static_cast<float>(m_clientW);
        s.clientH = static_cast<float>(m_clientH);
        if (m_d->hostTreePanelSlot != 0) {
            uint64_t panel = 0;
            RawEl e;
            s.treeVisible = m_d->gameUiRoot != 0 && Mem(m_d->gameUiRoot + m_d->hostTreePanelSlot, &panel, 8) && panel != 0 &&
                            ReadEl(panel, e) && Bit(e.flags, m_d->ui.visibleBit);
        }
        return s;
    }
    PlayerInfo Player() override { return m_d->player; }
    uint64_t ModuleBase() override { return m_d->moduleBase; }
    uint64_t ModuleSize() override { return m_d->moduleSize; }

    bool Read(uint64_t addr, void* buf, size_t n) override { return buf != nullptr && Mem(addr, buf, n); }

    UiElem UiRead(uint64_t addr) override {
        UiElem out;
        RawEl e;
        if (addr == 0 || !ReadEl(addr, e)) return out;
        out.parent = (e.parent == 0 || ValidUserPointer(e.parent)) ? e.parent : 0;
        if (ValidUserPointer(e.first) && e.last > e.first) {
            const uint64_t span = e.last - e.first;
            if (span % 8 == 0 && span / 8 <= static_cast<uint64_t>(kHostMaxChildren)) out.childCount = static_cast<int>(span / 8);
        }
        out.relX = e.relX;
        out.relY = e.relY;
        out.modX = e.modX;
        out.modY = e.modY;
        out.w = e.w;
        out.h = e.h;
        out.scaleMul = e.mult;
        out.flags = e.flags;
        out.type = e.type;
        out.scaleIndex = e.scaleIndex;
        out.visible = Bit(e.flags, m_d->ui.visibleBit);
        out.modPos = Bit(e.flags, m_d->ui.modifyBit);
        out.valid = true;
        return out;
    }

    std::vector<uint64_t> UiChildren(uint64_t addr) override {
        std::vector<uint64_t> out;
        if (addr == 0) return out;
        uint64_t hdr[2] = {};
        if (!Mem(addr + m_d->ui.children, hdr, sizeof(hdr))) return out;
        if (hdr[0] == 0 || hdr[1] <= hdr[0]) return out;
        const uint64_t count = (hdr[1] - hdr[0]) / 8;
        if (count == 0 || count > static_cast<uint64_t>(kHostMaxChildren)) return out;
        out.resize(static_cast<size_t>(count));
        if (!Mem(hdr[0], out.data(), out.size() * 8)) out.clear();
        return out;
    }

    Rect UiRect(uint64_t addr) override {
        std::unordered_map<uint64_t, Pos> memo;
        return RectOf(addr, memo);
    }

    void UiRects(const uint64_t* addrs, size_t n, Rect* out) override {
        if (addrs == nullptr || out == nullptr) return;
        std::unordered_map<uint64_t, Pos> memo;
        for (size_t i = 0; i < n; ++i) out[i] = RectOf(addrs[i], memo);
    }

    std::string UiControlName(uint64_t addr) override { return addr == 0 ? std::string() : WString(addr + m_d->ui.controlName); }
    std::string UiText(uint64_t addr) override { return addr == 0 ? std::string() : WString(addr + m_d->ui.text); }

    std::vector<uint64_t> UiFind(uint64_t root, const char* controlName, int maxDepth, bool visibleOnly, int maxResults) override {
        std::vector<uint64_t> found;
        if (root == 0) root = m_d->gameUiRoot;
        if (root == 0 || controlName == nullptr || controlName[0] == '\0' || maxDepth < 0 || maxResults <= 0) return found;
        const std::string want = controlName;
        const int depthLimit = std::min(maxDepth, kFindMaxDepth);
        std::vector<std::pair<uint64_t, int>> queue{{root, 0}};
        std::unordered_set<uint64_t> seen{root};
        for (size_t head = 0; head < queue.size(); ++head) {
            const auto [addr, depth] = queue[head];
            RawEl e;
            if (!ValidUserPointer(addr) || !ReadEl(addr, e)) continue;
            if (visibleOnly && !Bit(e.flags, m_d->ui.visibleBit)) continue;
            if (static_cast<int>(found.size()) < maxResults && WString(addr + m_d->ui.controlName) == want) found.push_back(addr);
            if (depth >= depthLimit) continue;
            if (!ValidUserPointer(e.first) || e.last <= e.first || (e.last - e.first) % 8 != 0) continue;
            const uint64_t count = (e.last - e.first) / 8;
            if (count > static_cast<uint64_t>(kHostMaxChildren)) continue;
            std::vector<uint64_t> kids(static_cast<size_t>(std::min<uint64_t>(count, kFindChildLimit)));
            if (!Mem(e.first, kids.data(), kids.size() * 8)) continue;
            for (const uint64_t k : kids) {
                if (k == 0) continue;
                if (seen.size() >= static_cast<size_t>(kFindBudget)) break;
                if (!seen.insert(k).second) continue;
                queue.emplace_back(k, depth + 1);
            }
        }
        return found;
    }

    uint64_t GameUiRoot() override { return m_d->gameUiRoot; }
    uint64_t InventoryPanelRoot() override { return m_d->inventoryPanelRoot; }

    InvInfo InvGet(int hostId) override {
        const auto it = m_d->inventories.find(hostId);
        return it == m_d->inventories.end() ? InvInfo{} : it->second.info;
    }
    void InvScan(int) override {}
    InvSnapshot InvRead(int hostId) override {
        const auto it = m_d->inventories.find(hostId);
        if (it != m_d->inventories.end()) return it->second;
        InvSnapshot s;
        s.supported = true;
        return s;
    }
    ItemInfo ItemRead(uint64_t entity) override {
        const auto it = m_d->items.find(entity);
        return it == m_d->items.end() ? ItemInfo{} : it->second.info;
    }
    std::string ItemEnglishName(uint64_t entity) override {
        const auto it = m_d->items.find(entity);
        return it == m_d->items.end() ? std::string() : it->second.englishName;
    }

private:
    static bool Bit(uint32_t flags, uint32_t bit) { return bit < 32 && ((flags >> bit) & 1u) != 0; }

    // Every memory access of this game: hidden ranges, injected faults, then the capture.
    bool Mem(uint64_t addr, void* buf, size_t n) {
        if (addr == 0 || n == 0 || addr > UINT64_MAX - n) return false;
        for (const auto& h : m_hidden)
            if (addr < h.second && addr + n > h.first) return false;
        if (m_failEvery != 0 || m_garbageOneIn != 0) {
            const uint64_t k = m_accesses.fetch_add(1, std::memory_order_relaxed) + 1;
            if (m_failEvery != 0 && k % m_failEvery == 0) return false;
            if (m_garbageOneIn != 0 && SplitMix(m_seed ^ (k * 0x2545F4914F6CDD1Dull)) % m_garbageOneIn == 0) {
                uint8_t* p = static_cast<uint8_t*>(buf);
                uint64_t r = SplitMix(m_seed + k);
                for (size_t i = 0; i < n; ++i) {
                    if (i % 8 == 0) r = SplitMix(r);
                    p[i] = static_cast<uint8_t>(r >> ((i % 8) * 8));
                }
                return true;
            }
        }
        const Data& d = *m_d;
        const auto it = std::upper_bound(d.starts.begin(), d.starts.end(), addr);
        if (it == d.starts.begin()) return false;
        const size_t i = static_cast<size_t>(it - d.starts.begin()) - 1;
        if (addr + n > d.ends[i]) return false;
        std::memcpy(buf, d.bytes.data() + d.offsets[i] + (addr - d.starts[i]), n);
        return true;
    }

    template <class T>
    bool Field(uint64_t addr, uint32_t off, T& out) {
        return Mem(addr + off, &out, sizeof(T));
    }

    bool ReadEl(uint64_t addr, RawEl& e) {
        const UiLayout& l = m_d->ui;
        uint64_t ch[2] = {};
        float rel[2] = {}, mod[2] = {}, size[2] = {};
        if (!Mem(addr + l.children, ch, sizeof(ch)) || !Field(addr, l.parent, e.parent) || !Mem(addr + l.relPos, rel, sizeof(rel)) ||
            !Mem(addr + l.posMod, mod, sizeof(mod)) || !Field(addr, l.scaleMul, e.mult) || !Field(addr, l.flags, e.flags) ||
            !Field(addr, l.type, e.type) || !Field(addr, l.scaleIndex, e.scaleIndex) || !Mem(addr + l.size, size, sizeof(size)))
            return false;
        e.first = ch[0];
        e.last = ch[1];
        e.relX = rel[0];
        e.relY = rel[1];
        e.modX = mod[0];
        e.modY = mod[1];
        e.w = size[0];
        e.h = size[1];
        return true;
    }

    std::pair<float, float> Scale(int index, float multiplier) const {
        const int w = m_clientW, h = m_clientH, cull = m_d->cull;
        const float v1 = static_cast<float>((w - cull * 2) / m_d->baseW);
        const float v2 = static_cast<float>(h / m_d->baseH);
        float sx = multiplier, sy = multiplier;
        switch (index) {
        case 1: sx *= v1; sy *= v1; break;
        case 2: sx *= v2; sy *= v2; break;
        case 3: sx *= v1; sy *= v2; break;
        default: break;
        }
        return {sx, sy};
    }

    // Bridge_Ui.cpp GetUnScaledPositionMemo.
    Pos Unscaled(const RawEl& e, std::unordered_map<uint64_t, Pos>& memo, int depth) {
        Pos pos{e.relX, e.relY};
        if (e.parent == 0 || depth > kMaxRectDepth) return pos;
        RawEl p;
        if (!ReadEl(e.parent, p)) return pos;
        Pos pp;
        const auto it = memo.find(e.parent);
        if (it != memo.end()) {
            pp = it->second;
        } else {
            pp = Unscaled(p, memo, depth + 1);
            memo[e.parent] = pp;
        }
        if (Bit(e.flags, m_d->ui.modifyBit)) {
            pp.x += p.modX;
            pp.y += p.modY;
        }
        if (p.scaleIndex == e.scaleIndex && p.mult == e.mult) return {pp.x + e.relX, pp.y + e.relY};
        const auto ps = Scale(p.scaleIndex, p.mult);
        const auto ms = Scale(e.scaleIndex, e.mult);
        return {ms.first != 0 ? pp.x * ps.first / ms.first + e.relX : pp.x + e.relX,
                ms.second != 0 ? pp.y * ps.second / ms.second + e.relY : pp.y + e.relY};
    }

    Rect RectOf(uint64_t addr, std::unordered_map<uint64_t, Pos>& memo) {
        Rect r;
        RawEl e;
        if (addr == 0 || !ReadEl(addr, e)) return r;
        const auto s = Scale(e.scaleIndex, e.mult);
        const Pos p = Unscaled(e, memo, 0);
        r.x = p.x * s.first + static_cast<float>(m_d->cull);
        r.y = p.y * s.second;
        r.w = e.w * s.first;
        r.h = e.h * s.second;
        r.ok = true;
        return r;
    }

    // Process::ReadStdWString: length and capacity 1..1000, inline when capacity <= 8.
    std::string WString(uint64_t at) {
        uint8_t h[32] = {};
        if (!Mem(at, h, sizeof(h))) return {};
        const int64_t len = Load<int64_t>(reinterpret_cast<const char*>(h) + 16);
        const int64_t cap = Load<int64_t>(reinterpret_cast<const char*>(h) + 24);
        if (len <= 0 || len > kMaxStringUnits || cap <= 0 || cap > kMaxStringUnits || len > cap) return {};
        std::vector<uint16_t> units(static_cast<size_t>(len));
        if (cap <= 8) {
            std::memcpy(units.data(), h, static_cast<size_t>(len) * 2);
        } else if (!Mem(Load<uint64_t>(reinterpret_cast<const char*>(h)), units.data(), units.size() * 2)) {
            return {};
        }
        return Utf16ToUtf8(units.data(), units.size());
    }

    std::shared_ptr<const Data> m_d;
    std::vector<std::pair<uint64_t, uint64_t>> m_hidden;
    int m_clientW = 0, m_clientH = 0;
    uint32_t m_failEvery = 0, m_garbageOneIn = 0;
    uint64_t m_seed = 1;
    std::atomic<uint64_t> m_accesses{0};
};

} // namespace

std::unique_ptr<IGame> OpenFixtureGame(const std::filesystem::path& file, std::string* error) {
    try {
        auto d = std::make_shared<Data>();
        if (!LoadData(file, *d, error)) return nullptr;
        const int w = d->clientW, h = d->clientH;
        return std::make_unique<FixtureGameImpl>(std::move(d), std::vector<std::pair<uint64_t, uint64_t>>{}, w, h, 0u, 0u, 1ull);
    } catch (const std::exception& e) {
        Fail(error, std::string("the capture could not be loaded: ") + e.what());
    } catch (...) {
        Fail(error, "the capture could not be loaded");
    }
    return nullptr;
}

std::unique_ptr<IGame> EditFixtureGame(const IGame& base, const FixtureEdits& edits) {
    try {
        const auto* b = dynamic_cast<const FixtureGameImpl*>(&base);
        if (b == nullptr) return nullptr;
        std::shared_ptr<const Data> data = edits.patches.empty() ? b->DataPtr() : Patched(*b->DataPtr(), edits.patches);
        std::vector<std::pair<uint64_t, uint64_t>> hidden = b->Hidden();
        for (const auto& h : edits.hidden)
            if (h.second > h.first) hidden.push_back(h);
        const int w = edits.clientW > 0 ? edits.clientW : b->ClientW();
        const int h = edits.clientH > 0 ? edits.clientH : b->ClientH();
        return std::make_unique<FixtureGameImpl>(std::move(data), std::move(hidden), w, h, edits.failEvery, edits.garbageOneIn,
                                                 edits.seed);
    } catch (...) {
        return nullptr;
    }
}

std::string ReadFixtureHeader(const std::filesystem::path& file, std::string* error) {
    try {
        const FileRead r = ReadFileCapped(file, kMaxFileBytes);
        if (!r.Ok()) {
            Fail(error, r.status == FileStatus::Missing ? "the capture file does not exist" : "the capture file could not be read");
            return {};
        }
        std::string header;
        if (!ParseFile(r.bytes, header, nullptr, nullptr, nullptr, error)) return {};
        return header;
    } catch (...) {
        Fail(error, "the capture header could not be read");
        return {};
    }
}

} // namespace bp
