// BuildPlanner - the character's gems and the Skills panel (reader)
// SkillsReader.cpp - see SkillsReader.h. Reads the game READ-ONLY through IGame; every count, pointer
// and string that comes from the game is checked before it is used.
//
// tools/make_fixture.py (record_skills) mirrors these reads. The order of one tick: the panel (its
// discovery, the open bit, the cheap block identity, the full block read when due), then the
// character's skills when due (a row a block named this tick is walked at once), then the state
// is composed (blocks matched to skills) and published when it differs from the last one.
// The tooltip source (the game's UI manager, GearReader.h) is looked up when the area changes
// and checked again every 2 s.
#include "SkillsReader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <unordered_map>

#include "GearReader.h"   // ReadInventoryItems

namespace bp {
namespace {

constexpr size_t kMaxInventoryRecords = 512;
constexpr size_t kMaxLookupBytes = 0x1000;
constexpr uint64_t kMaxComponents = 256;
constexpr uint64_t kMaxGrantedSkills = 8;
constexpr uint64_t kMaxSockets = 6;
constexpr size_t kMinPanelChildren = 3, kMaxPanelChildren = 16;
constexpr size_t kMaxListChildren = 512;
constexpr size_t kMaxRootChildren = 512;
constexpr size_t kMaxBigSocketChildren = 16;
constexpr int kBlockChildren = 7;
constexpr int64_t kBlocksMs = 250, kSkillsOpenMs = 500, kSkillsClosedMs = 2000;
// A failed character read is retried after 250 ms (a torn read while a gem moves), after 2 s once ten
// reads in a row failed (an offset that moved: no constant load).
constexpr int64_t kSkillsRetryMs = 250, kSkillsSlowRetryMs = 2000;
constexpr int kSkillsFastRetries = 10;
constexpr size_t kMaxCachedItems = 4096, kMaxCachedTypes = 1024;
constexpr size_t kMaxExtraRows = 8;
constexpr int kGrantedStrikes = 3;   // consecutive failed reads of one item's granted skills before they are dropped
constexpr uint32_t kMaxGemLevel = 40;
constexpr uint32_t kMaxDatRow = 4096;
constexpr int64_t kTooltipSourceMs = 2000;

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// At most `max` bytes of a UTF-8 text, never ending inside a character.
std::string CapUtf8(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0u) == 0x80u) --n;
    return s.substr(0, n);
}

// u32 fields at base + each offset: one read over their span when it is small (the reference
// layout), else one read per field. The span is computed in 64 bits: an offset near 0xFFFFFFFF
// (config/offsets.json) would wrap `o + 4` in 32 bits, pass the span test and copy from ~4 GB past
// `buf` (an access violation on the watcher thread, which no catch(...) stops).
bool ReadU32s(IGame& g, uint64_t base, std::initializer_list<uint32_t> offs, uint32_t* out) {
    uint64_t lo = UINT64_MAX, hi = 0;
    for (const uint32_t o : offs) {
        lo = std::min<uint64_t>(lo, o);
        hi = std::max<uint64_t>(hi, uint64_t{o} + 4);
    }
    size_t i = 0;
    if (hi > lo && hi - lo <= 64) {
        uint8_t buf[64] = {};
        if (!g.Read(base + lo, buf, static_cast<size_t>(hi - lo))) return false;
        for (const uint32_t o : offs) std::memcpy(&out[i++], buf + (o - lo), 4);
        return true;
    }
    for (const uint32_t o : offs)
        if (!g.Get(base + o, out[i++])) return false;
    return true;
}

// A Skills panel header text against a gem's localized name; "Spectre: {0}" matches by the text
// around the placeholder.
bool NameMatches(const std::string& header, const std::string& gemName) {
    if (header.empty() || gemName.empty()) return false;
    if (header == gemName) return true;
    const size_t at = gemName.find("{0}");
    if (at == std::string::npos || at == 0) return false;
    const std::string prefix = gemName.substr(0, at), suffix = gemName.substr(at + 3);
    return header.size() > prefix.size() + suffix.size() && header.compare(0, prefix.size(), prefix) == 0 &&
           header.compare(header.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool SameGem(const CharGem& a, const CharGem& b) {
    return a.present == b.present && a.entity == b.entity && a.gemId == b.gemId && a.name == b.name && a.level == b.level;
}

bool SameSkill(const CharSkill& a, const CharSkill& b) {
    if (!SameGem(a.gem, b.gem) || a.datRow != b.datRow || a.slotX != b.slotX || a.itemGranted != b.itemGranted ||
        a.socketCount != b.socketCount || a.supports.size() != b.supports.size())
        return false;
    for (size_t i = 0; i < a.supports.size(); ++i)
        if (!SameGem(a.supports[i], b.supports[i])) return false;
    return true;
}

bool SameSkills(const std::vector<CharSkill>& a, const std::vector<CharSkill>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!SameSkill(a[i], b[i])) return false;
    return true;
}

bool SameBlock(const SkillBlockUi& a, const SkillBlockUi& b) {
    return a.element == b.element && a.header == b.header && a.headerRow == b.headerRow && a.bigSocket == b.bigSocket &&
           a.sockets == b.sockets &&
           a.socketCount == b.socketCount && a.filled == b.filled && a.skill == b.skill && a.socketsAgree == b.socketsAgree &&
           a.name == b.name;
}

bool SameState(const SkillsState& a, const SkillsState& b) {
    if (a.problem != b.problem || a.skillsKnown != b.skillsKnown || a.panelFound != b.panelFound ||
        a.panelBuilt != b.panelBuilt || a.panelOpen != b.panelOpen || a.panel != b.panel || a.scrollView != b.scrollView ||
        a.blocks.size() != b.blocks.size() || !SameSkills(a.skills, b.skills) || !(a.tooltipSource == b.tooltipSource))
        return false;
    for (size_t i = 0; i < a.blocks.size(); ++i)
        if (!SameBlock(a.blocks[i], b.blocks[i])) return false;
    return true;
}

struct InvRef {
    uint32_t row = 0;
    int hostId = 0;
    uint64_t inv = 0;
    bool operator==(const InvRef&) const = default;
};

// Component indexes of one entity type (the lookup object is shared by the type).
struct TypeIdx {
    int skillGem = -1, sockets = -1, mods = -1;
    bool operator==(const TypeIdx&) const = default;
};

struct Comps {
    uint64_t details = 0, skillGem = 0, sockets = 0, mods = 0;
};

struct CachedItem {
    uint64_t details = 0;
    ItemInfo info;
};

// What the full block read saw of one live block (matched to the skills when the state is composed).
struct RawBlock {
    uint64_t element = 0, header = 0, headerRow = 0, big = 0;
    std::array<uint64_t, kMaxSupportSockets> sockets{};
    std::array<bool, kMaxSupportSockets> filled{};
    int shown = 0;
    bool idRead = false;          // the big socket's row and slot were read
    bool idsOk = false;           // ... and every shown socket names this block, in order, without gaps
    uint32_t row = 0, slot = 0;
    std::string name;
};

enum class Fail : uint8_t { None, NoPlayerData, NoGems, ReadFailed };

SkillsProblem ProblemOf(Fail f) {
    switch (f) {
    case Fail::NoPlayerData: return SkillsProblem::NoPlayerData;
    case Fail::NoGems: return SkillsProblem::NoGems;
    case Fail::ReadFailed: return SkillsProblem::ReadFailed;
    default: return SkillsProblem::None;
    }
}

} // namespace

struct SkillsWork {
    bool haveArea = false;
    bool rootOk = false;
    // ---- character data ----
    std::vector<InvRef> rows;                               // every player inventory by its row (per area)
    std::vector<uint32_t> extraRows;                        // rows named by blocks (not gem / equipment rows)
    std::unordered_map<uint64_t, TypeIdx> types;            // component lookup -> indexes
    std::unordered_map<uint64_t, CachedItem> items;         // entity -> ItemRead answer
    std::unordered_map<uint64_t, int> strikes;              // granted path: consecutive failures per item / inventory
    std::vector<std::pair<uint64_t, int32_t>> counters;     // walked gem inventories: request counters
    bool skillsTried = false, skillsDue = true;
    int skillsFailures = 0;                                 // consecutive failed character reads
    bool skillsKnown = false;
    SkillsProblem skillsProblem = SkillsProblem::NotInGame;
    std::vector<CharSkill> skills;
    // ---- the panel ----
    uint64_t panel = 0, view = 0, list = 0;
    bool panelBuilt = false, panelOpen = false;
    std::vector<uint64_t> listKids;
    std::vector<RawBlock> blocks;
    // ---- the game's tooltip ----
    TooltipSource tooltipSource;
    bool sourceDue = true;
    int64_t lastSourceMs = 0;
};

struct SkillsReaderImpl {
    static SkillsWork& Work(SkillsReader& r) {
        if (!r.m_work) r.m_work = std::make_shared<SkillsWork>();
        return *r.m_work;
    }

    // ================================ character data ================================

    static bool BuildRowsOnce(SkillsReader& r, uint64_t root, std::vector<InvRef>& rows) {
        IGame& g = r.m_game;
        const Offsets& o = r.m_off;
        rows.clear();
        const uint64_t psd = g.Ptr(root + o.psdSlot);
        if (!PlausibleObject(psd)) return false;
        const uint32_t stride = o.psdInventoryStride;
        if (stride < 16 || stride > 256 || o.psdInventoryPtr > stride - 8) return false;
        uint64_t v[3] = {};
        if (!g.Read(psd + o.psdInventories, v, sizeof(v))) return false;
        if (!PlausibleObject(v[0]) || v[1] <= v[0] || v[2] < v[1] || (v[1] - v[0]) % stride != 0) return false;
        const uint64_t n = (v[1] - v[0]) / stride;
        if (n > kMaxInventoryRecords) return false;
        std::vector<uint8_t> recs(static_cast<size_t>(n * stride));
        if (!g.Read(v[0], recs.data(), recs.size())) return false;
        for (uint64_t i = 0; i < n; ++i) {
            const uint8_t* p = recs.data() + i * stride;
            int32_t id = 0;
            uint64_t inv = 0;
            std::memcpy(&id, p, 4);
            std::memcpy(&inv, p + o.psdInventoryPtr, 8);
            if (!PlausibleObject(inv)) continue;
            uint32_t row = 0;
            if (!g.Get(inv + o.invDatRow, row)) return false;   // a torn read: not a map to keep for the area
            if (row >= kMaxDatRow) continue;
            if (std::any_of(rows.begin(), rows.end(), [&](const InvRef& x) { return x.row == row; })) continue;
            rows.push_back({row, id, inv});
        }
        return !rows.empty();
    }

    // The row map is kept for the whole area: it is accepted only when two reads agree, so one torn
    // read cannot hide an inventory until the next area.
    static bool BuildRows(SkillsReader& r, uint64_t root) {
        SkillsWork& w = Work(r);
        std::vector<InvRef> again;
        if (!BuildRowsOnce(r, root, w.rows) || !BuildRowsOnce(r, root, again) || again != w.rows) {
            w.rows.clear();
            return false;
        }
        return true;
    }

    static const InvRef* RowRef(SkillsWork& w, uint32_t row) {
        for (const InvRef& x : w.rows)
            if (x.row == row) return &x;
        return nullptr;
    }

    static bool TypesOnce(SkillsReader& r, uint64_t lookup, TypeIdx& t) {
        IGame& g = r.m_game;
        const Offsets& o = r.m_off;
        const uint32_t stride = o.lookupBucketStride;
        if (stride < 12 || stride > 64) return false;
        uint64_t v[2] = {};
        if (!g.Read(lookup + o.lookupBuckets, v, sizeof(v))) return false;
        if (!PlausiblePtr(v[0]) || v[1] <= v[0] || v[1] - v[0] > kMaxLookupBytes || (v[1] - v[0]) % stride != 0) return false;
        std::vector<uint8_t> raw(static_cast<size_t>(v[1] - v[0]));
        if (!g.Read(v[0], raw.data(), raw.size())) return false;
        t = TypeIdx{};
        for (size_t at = 0; at + stride <= raw.size(); at += stride) {
            uint64_t name = 0;
            int32_t index = -1;
            std::memcpy(&name, raw.data() + at, 8);
            std::memcpy(&index, raw.data() + at + 8, 4);
            if (!PlausiblePtr(name) || index < 0 || static_cast<uint64_t>(index) >= kMaxComponents) continue;
            char text[16] = {};
            if (!g.Read(name, text, sizeof(text))) continue;
            const auto is = [&](const char* want) { return std::strncmp(text, want, sizeof(text)) == 0; };
            if (t.skillGem < 0 && is("SkillGem")) t.skillGem = index;
            else if (t.sockets < 0 && is("Sockets")) t.sockets = index;
            else if (t.mods < 0 && is("Mods")) t.mods = index;
        }
        return true;
    }

    // Component indexes per lookup object, cached for the area once two reads agree.
    static bool TypesOf(SkillsReader& r, uint64_t lookup, TypeIdx& out) {
        SkillsWork& w = Work(r);
        const auto hit = w.types.find(lookup);
        if (hit != w.types.end()) {
            out = hit->second;
            return true;
        }
        TypeIdx again;
        if (!TypesOnce(r, lookup, out) || !TypesOnce(r, lookup, again) || !(again == out)) return false;
        if (w.types.size() >= kMaxCachedTypes) w.types.clear();
        w.types[lookup] = out;
        return true;
    }

    static bool ComponentsOf(SkillsReader& r, uint64_t entity, Comps& c) {
        IGame& g = r.m_game;
        const Offsets& o = r.m_off;
        c = Comps{};
        if (!PlausibleObject(entity)) return false;
        c.details = g.Ptr(entity + o.entityDetails);
        if (!PlausibleObject(c.details)) return false;
        const uint64_t lookup = g.Ptr(c.details + o.detailsLookup);
        TypeIdx t;
        if (!PlausibleObject(lookup) || !TypesOf(r, lookup, t)) return false;
        uint64_t v[2] = {};
        if (!g.Read(entity + o.entityComponents, v, sizeof(v))) return false;
        if (!PlausibleObject(v[0]) || v[1] < v[0] || (v[1] - v[0]) % 8 != 0 || (v[1] - v[0]) / 8 > kMaxComponents) return false;
        const uint64_t count = (v[1] - v[0]) / 8;
        const auto at = [&](int index, uint64_t& out) {
            if (index < 0) return true;
            if (static_cast<uint64_t>(index) >= count) return false;
            out = g.Ptr(v[0] + static_cast<uint64_t>(index) * 8);
            return PlausibleObject(out);
        };
        return at(t.skillGem, c.skillGem) && at(t.sockets, c.sockets) && at(t.mods, c.mods);
    }

    // A gem item: ItemRead (cached per entity while its EntityDetails stay, once two answers agree), a
    // "Metadata/Items/Gem" path (else NoGems).
    static Fail GemOf(SkillsReader& r, uint64_t entity, uint64_t details, CharGem& out) {
        SkillsWork& w = Work(r);
        auto it = w.items.find(entity);
        if (it == w.items.end() || it->second.details != details) {
            const ItemInfo a = r.m_game.ItemRead(entity), b = r.m_game.ItemRead(entity);
            if (a.valid != b.valid || a.path != b.path || a.baseName != b.baseName) return Fail::ReadFailed;
            if (w.items.size() >= kMaxCachedItems) w.items.clear();
            CachedItem ci;
            ci.details = details;
            ci.info = a;
            it = w.items.insert_or_assign(entity, std::move(ci)).first;
        }
        const ItemInfo& info = it->second.info;
        if (!info.valid || !StartsWith(info.path, "Metadata/Items/Gem")) return Fail::NoGems;
        out = CharGem{};
        out.present = true;
        out.entity = entity;
        out.gemId = info.path;
        out.name = CapUtf8(info.baseName, kMaxNameBytes);
        return Fail::None;
    }

    static Fail SkillOf(SkillsReader& r, uint64_t entity, const Comps& c, uint32_t row, int x, bool granted, CharSkill& s) {
        IGame& g = r.m_game;
        const Offsets& o = r.m_off;
        s = CharSkill{};
        s.datRow = static_cast<int>(row);
        s.slotX = x;
        s.itemGranted = granted;
        if (const Fail f = GemOf(r, entity, c.details, s.gem); f != Fail::None) return f;
        uint32_t level = 0;
        if (c.skillGem && !g.Get(c.skillGem + o.skillGemLevel, level)) return Fail::ReadFailed;
        if (level >= 1 && level <= kMaxGemLevel) s.gem.level = static_cast<int>(level);   // else 0: unknown (cosmetic)
        if (c.sockets == 0) return Fail::None;
        if (g.Ptr(c.sockets + o.socketsOwner) != entity) return Fail::ReadFailed;
        uint64_t ents[kMaxSockets] = {};
        uint64_t cv[2] = {};
        if (!g.Read(c.sockets + o.socketsEntities, ents, sizeof(ents)) || !g.Read(c.sockets + o.socketsCounts, cv, sizeof(cv)))
            return Fail::ReadFailed;
        uint64_t count = 0;
        if (cv[0] != 0 || cv[1] != 0) {
            if (!PlausiblePtr(cv[0]) || cv[1] < cv[0] || cv[1] - cv[0] > kMaxSockets) return Fail::ReadFailed;
            count = cv[1] - cv[0];
        }
        s.socketCount = static_cast<int>(count);
        s.supports.resize(static_cast<size_t>(count));
        for (uint64_t k = 0; k < count; ++k) {
            const uint64_t e = ents[k];
            if (e == 0) continue;
            if (!PlausibleObject(e)) return Fail::ReadFailed;
            const uint64_t details = g.Ptr(e + o.entityDetails);
            if (!PlausibleObject(details)) return Fail::ReadFailed;
            if (const Fail f = GemOf(r, e, details, s.supports[static_cast<size_t>(k)]); f != Fail::None) return f;
        }
        return Fail::None;
    }

    // The skill gems an item grants ([Mods + modsGrantedSkills]); false when the vector or an entry
    // does not read as gems (the caller's strike rule decides what that costs).
    static bool GrantedOf(SkillsReader& r, uint64_t mods, uint32_t row, int x, std::vector<CharSkill>& out) {
        IGame& g = r.m_game;
        uint64_t v[2] = {};
        if (!g.Read(mods + r.m_off.modsGrantedSkills, v, sizeof(v))) return false;
        if (v[0] == 0 && v[1] == 0) return true;
        if (!PlausibleObject(v[0]) || v[1] <= v[0] || (v[1] - v[0]) % 8 != 0 || (v[1] - v[0]) / 8 > kMaxGrantedSkills) return false;
        uint64_t ents[kMaxGrantedSkills] = {};
        if (!g.Read(v[0], ents, static_cast<size_t>(v[1] - v[0]))) return false;
        std::vector<CharSkill> found;
        for (uint64_t i = 0; i < (v[1] - v[0]) / 8; ++i) {
            Comps c;
            CharSkill s;
            if (!ComponentsOf(r, ents[i], c) || c.skillGem == 0 || SkillOf(r, ents[i], c, row, x, true, s) != Fail::None) return false;
            found.push_back(std::move(s));
        }
        for (CharSkill& s : found) out.push_back(std::move(s));
        return true;
    }

    static bool InventoryOf(SkillsReader& r, const InvRef& ref, std::vector<InvCellItem>& items, bool* stale) {
        IGame& g = r.m_game;
        const Offsets& o = r.m_off;
        uint32_t row = 0;
        if (!g.Get(ref.inv + o.invDatRow, row)) return false;
        if (row != ref.row) {
            *stale = true;
            return false;
        }
        int32_t boxes[2] = {};
        if (!g.Read(ref.inv + o.invBoxes, boxes, sizeof(boxes))) return false;
        return ReadInventoryItems(g, o, ref.inv, boxes[0], boxes[1], items);
    }

    static std::vector<uint32_t> EquipmentRows(const Offsets& o) {
        std::vector<uint32_t> rows;
        for (const GearSlotInfo& s : kGearSlots)
            rows.push_back(s.slot == GearSlot::Ring3 ? o.invRowRing3 : static_cast<uint32_t>(s.datRow));
        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        return rows;
    }

    static std::vector<uint32_t> GemRows(SkillsReader& r) {
        std::vector<uint32_t> rows = {r.m_off.invRowSkillSlots, r.m_off.invRowAscendancySkills};
        rows.insert(rows.end(), Work(r).extraRows.begin(), Work(r).extraRows.end());
        std::sort(rows.begin(), rows.end());
        rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
        return rows;
    }

    static Fail ReadCharacterOnce(SkillsReader& r, uint64_t root, std::vector<CharSkill>& out, bool* stale) {
        IGame& g = r.m_game;
        const Offsets& o = r.m_off;
        SkillsWork& w = Work(r);
        out.clear();
        if (w.rows.empty() && !BuildRows(r, root)) return Fail::NoPlayerData;
        if (!RowRef(w, o.invRowSkillSlots)) return Fail::NoGems;
        std::vector<CharSkill> granted;
        std::vector<std::pair<uint64_t, int32_t>> counters;
        std::vector<InvCellItem> items;
        // The item-granted path is optional (an offset of it moving must cost only those
        // skills), but a torn read must not drop a skill either: a failure there fails the whole read
        // until the same item (or inventory) failed kGrantedStrikes reads in a row. The walk goes on
        // after such a failure so that every failing item counts in the same read; the counts of this
        // read replace the last ones however it ends.
        bool grantedTorn = false;
        std::unordered_map<uint64_t, int> strikes;
        struct Commit {
            SkillsWork& w;
            std::unordered_map<uint64_t, int>& next;
            ~Commit() { w.strikes.swap(next); }
        } commit{w, strikes};
        const auto giveUp = [&](uint64_t key) {
            const auto it = w.strikes.find(key);
            const int n = (it == w.strikes.end() ? 0 : it->second) + 1;
            strikes[key] = n;
            return n >= kGrantedStrikes;
        };
        const std::vector<uint32_t> gemRows = GemRows(r);
        for (const uint32_t row : gemRows) {
            const InvRef* ref = RowRef(w, row);
            if (!ref) continue;
            if (!InventoryOf(r, *ref, items, stale)) return Fail::ReadFailed;
            int32_t counter = 0;
            if (g.Get(ref->inv + o.invRequestCounter, counter)) counters.emplace_back(ref->inv, counter);
            for (const InvCellItem& item : items) {
                Comps c;
                if (!ComponentsOf(r, item.entity, c)) return Fail::ReadFailed;
                if (c.skillGem != 0) {
                    CharSkill s;
                    const Fail f = SkillOf(r, item.entity, c, row, item.x, false, s);
                    if (f != Fail::None) return f;
                    out.push_back(std::move(s));
                } else if (c.mods != 0 && !GrantedOf(r, c.mods, row, item.x, granted) && !giveUp(item.entity)) {
                    grantedTorn = true;
                }
            }
        }
        for (const uint32_t row : EquipmentRows(o)) {
            if (std::find(gemRows.begin(), gemRows.end(), row) != gemRows.end()) continue;
            const InvRef* ref = RowRef(w, row);
            if (!ref) continue;
            if (!InventoryOf(r, *ref, items, stale)) {
                if (*stale) return Fail::ReadFailed;
                if (!giveUp(ref->inv)) grantedTorn = true;
                continue;
            }
            for (const InvCellItem& item : items) {
                Comps c;
                const bool ok =
                    ComponentsOf(r, item.entity, c) && (c.skillGem != 0 || c.mods == 0 || GrantedOf(r, c.mods, row, item.x, granted));
                if (!ok && !giveUp(item.entity)) grantedTorn = true;
            }
        }
        if (grantedTorn) return Fail::ReadFailed;
        for (CharSkill& s : granted) out.push_back(std::move(s));
        w.counters = std::move(counters);
        return Fail::None;
    }

    static void ReadSkills(SkillsReader& r, uint64_t root, int64_t now) {
        SkillsWork& w = Work(r);
        r.m_lastSkillsMs = now;
        w.skillsTried = true;
        w.skillsDue = false;
        std::vector<CharSkill> skills;
        bool stale = false;
        Fail f = ReadCharacterOnce(r, root, skills, &stale);
        if (stale) {   // an inventory record changed meaning: map the rows again, once
            w.rows.clear();
            stale = false;
            f = ReadCharacterOnce(r, root, skills, &stale);
        }
        if (f != Fail::None) {
            ++w.skillsFailures;
            w.skillsKnown = false;
            w.skills.clear();
            w.counters.clear();
            w.skillsProblem = ProblemOf(f);
            return;
        }
        w.skillsFailures = 0;
        w.skillsKnown = true;
        w.skillsProblem = SkillsProblem::None;
        if (!SameSkills(w.skills, skills)) w.skills = std::move(skills);
    }

    static bool CountersMoved(SkillsReader& r) {
        for (const auto& c : Work(r).counters) {
            int32_t now = 0;
            if (!r.m_game.Get(c.first + r.m_off.invRequestCounter, now) || now != c.second) return true;
        }
        return false;
    }

    static bool SkillsDue(SkillsReader& r, int64_t now) {
        SkillsWork& w = Work(r);
        if (!w.skillsTried || w.skillsDue) return true;
        const int64_t since = now - r.m_lastSkillsMs;
        if (!w.skillsKnown) return since >= (w.skillsFailures >= kSkillsFastRetries ? kSkillsSlowRetryMs : kSkillsRetryMs);
        if (since >= (w.panelOpen ? kSkillsOpenMs : kSkillsClosedMs)) return true;
        return CountersMoved(r);
    }

    // ================================ the panel ================================

    struct PanelShape {
        uint64_t view = 0, list = 0;
        bool built = false;
    };

    // The panel: children named close_button / label_title / client_area; client_area/[0]/[1] is the
    // scroll view with 3 children; its [1]/[0] the list. A built list must show a live block (7
    // children, its [1] holding a child named "collapsed"); `requireBuilt` (the structural search,
    // where 15 panels share the triple) also refuses an unbuilt one and one without a live block.
    static bool ValidatePanel(SkillsReader& r, uint64_t p, bool requireBuilt, PanelShape& out) {
        IGame& g = r.m_game;
        out = PanelShape{};
        const std::vector<uint64_t> kids = g.UiChildren(p);
        if (kids.size() < kMinPanelChildren || kids.size() > kMaxPanelChildren) return false;
        bool close = false, title = false;
        uint64_t area = 0;
        for (const uint64_t k : kids) {
            const std::string n = g.UiControlName(k);
            if (n == "close_button") close = true;
            else if (n == "label_title") title = true;
            else if (n == "client_area" && area == 0) area = k;
        }
        if (!close || !title || area == 0) return false;
        const std::vector<uint64_t> a = g.UiChildren(area);
        if (a.empty()) return false;
        const std::vector<uint64_t> a0 = g.UiChildren(a[0]);
        if (a0.size() < 2) return false;
        const std::vector<uint64_t> view = g.UiChildren(a0[1]);
        if (view.size() != 3) return false;
        const std::vector<uint64_t> content = g.UiChildren(view[1]);
        if (content.empty() || !PlausibleObject(content[0])) return false;
        const std::vector<uint64_t> list = g.UiChildren(content[0]);
        out.view = a0[1];
        out.list = content[0];
        out.built = !list.empty();
        if (!out.built) return !requireBuilt;
        bool anyBlock = false;
        for (size_t i = 0; i < list.size() && i < kMaxListChildren; ++i) {
            const UiElem be = g.UiRead(list[i]);
            if (!be.valid || !be.visible || be.childCount != kBlockChildren) continue;
            const std::vector<uint64_t> bk = g.UiChildren(list[i]);
            if (bk.size() != static_cast<size_t>(kBlockChildren)) continue;
            anyBlock = true;
            const std::vector<uint64_t> big = g.UiChildren(bk[1]);
            for (size_t j = 0; j < big.size() && j < kMaxBigSocketChildren; ++j)
                if (g.UiControlName(big[j]) == "collapsed") return true;
        }
        return !anyBlock && !requireBuilt;   // a built list without a live block: a character without skills
    }

    static void DropPanel(SkillsReader& r) {
        SkillsWork& w = Work(r);
        w.panel = w.view = w.list = 0;
        w.panelBuilt = w.panelOpen = false;
        w.listKids.clear();
        w.blocks.clear();
        r.m_wasOpen = false;
    }

    static bool DiscoverPanel(SkillsReader& r, uint64_t root) {
        IGame& g = r.m_game;
        SkillsWork& w = Work(r);
        PanelShape shape;
        uint64_t found = 0;
        const uint64_t shortcut = g.Ptr(root + r.m_off.skillsPanelSlot);
        if (PlausibleObject(shortcut) && ValidatePanel(r, shortcut, false, shape)) {
            found = shortcut;
        } else {
            const std::vector<uint64_t> kids = g.UiChildren(root);
            for (size_t i = 0; i < kids.size() && i < kMaxRootChildren && found == 0; ++i)
                if (PlausibleObject(kids[i]) && ValidatePanel(r, kids[i], true, shape)) found = kids[i];
        }
        if (found == 0) return false;
        w.panel = found;
        w.view = shape.view;
        w.list = shape.list;
        w.panelBuilt = shape.built;
        return true;
    }


    static void FullBlocks(SkillsReader& r, const std::vector<uint64_t>& kids, int64_t now) {
        IGame& g = r.m_game;
        const Offsets& o = r.m_off;
        SkillsWork& w = Work(r);
        std::vector<RawBlock> blocks;
        std::vector<uint32_t> walked = GemRows(r);
        for (const uint32_t row : EquipmentRows(o)) walked.push_back(row);
        for (size_t i = 0; i < kids.size() && i < kMaxListChildren; ++i) {
            const UiElem be = g.UiRead(kids[i]);
            if (!be.valid || !be.visible || be.childCount != kBlockChildren) continue;
            const std::vector<uint64_t> bk = g.UiChildren(kids[i]);
            if (bk.size() != static_cast<size_t>(kBlockChildren)) continue;
            RawBlock b;
            b.element = kids[i];
            b.header = bk[0];
            b.big = bk[1];
            uint32_t id[2] = {};
            b.idRead = ReadU32s(g, b.big, {o.bigSocketRow, o.bigSocketSlot}, id);
            b.row = id[0];
            b.slot = id[1];
            b.idsOk = b.idRead;
            bool gap = false;
            for (int s = 0; s < kMaxSupportSockets; ++s) {
                const uint64_t sock = bk[static_cast<size_t>(2 + s)];
                const UiElem se = g.UiRead(sock);
                if (!se.valid || !se.visible) {
                    gap = true;
                    continue;
                }
                if (gap) b.idsOk = false;   // a shown socket after a hidden one
                b.sockets[static_cast<size_t>(s)] = sock;
                ++b.shown;
                const std::vector<uint64_t> sk = g.UiChildren(sock);
                if (sk.size() > 2) {
                    const UiElem icon = g.UiRead(sk[2]);
                    b.filled[static_cast<size_t>(s)] = icon.valid && icon.visible;
                }
                uint32_t sid[3] = {};
                if (!ReadU32s(g, sock, {o.supSocketOwnerRow, o.supSocketOwnerSlot, o.supSocketIndex}, sid) || sid[0] != b.row ||
                    sid[1] != b.slot || sid[2] != static_cast<uint32_t>(s))
                    b.idsOk = false;
            }
            const std::vector<uint64_t> hk = g.UiChildren(b.header);
            if (!hk.empty()) b.headerRow = hk[0];
            if (hk.size() > 5) b.name = CapUtf8(g.UiText(hk[5]), kMaxNameBytes);
            if (b.idRead && b.row < kMaxDatRow && std::find(walked.begin(), walked.end(), b.row) == walked.end() &&
                w.extraRows.size() < kMaxExtraRows) {
                w.extraRows.push_back(b.row);   // a skill in an inventory we do not walk yet: walk it from now on
                walked.push_back(b.row);
                w.skillsDue = true;
            }
            blocks.push_back(std::move(b));
        }
        w.blocks = std::move(blocks);
        w.listKids = kids;
        r.m_lastBlocksMs = now;
    }

    // The cheap identity of every tick: the list's children and each live block's (row, slot).
    static bool IdentityMoved(SkillsReader& r, const std::vector<uint64_t>& kids) {
        SkillsWork& w = Work(r);
        if (kids != w.listKids) return true;
        for (const RawBlock& b : w.blocks) {
            if (!b.idRead) continue;
            uint32_t id[2] = {};
            if (!ReadU32s(r.m_game, b.big, {r.m_off.bigSocketRow, r.m_off.bigSocketSlot}, id) || id[0] != b.row || id[1] != b.slot)
                return true;
        }
        return false;
    }

    static void UpdatePanel(SkillsReader& r, uint64_t root, int64_t now) {
        IGame& g = r.m_game;
        SkillsWork& w = Work(r);
        if (w.panel == 0) {
            if (!r.m_retry.Due(now)) return;
            if (!DiscoverPanel(r, root)) {
                r.m_retry.Failed(now);
                return;
            }
            r.m_retry.Succeeded();
        }
        const UiElem pe = g.UiRead(w.panel);
        if (!pe.valid) {
            DropPanel(r);
            r.m_retry.Kick();
            return;
        }
        const bool opened = pe.visible && !r.m_wasOpen;
        r.m_wasOpen = pe.visible;
        w.panelOpen = pe.visible;
        if (opened || (pe.visible && !w.panelBuilt)) {
            // Built on its first open (and then kept): check the shape again.
            PanelShape shape;
            if (!ValidatePanel(r, w.panel, false, shape)) {
                DropPanel(r);
                r.m_retry.Kick();
                return;
            }
            w.view = shape.view;
            w.list = shape.list;
            w.panelBuilt = shape.built;
        }
        if (!w.panelOpen) {
            w.blocks.clear();
            w.listKids.clear();
            return;
        }
        const std::vector<uint64_t> kids = g.UiChildren(w.list);
        if (opened || now - r.m_lastBlocksMs >= kBlocksMs || IdentityMoved(r, kids)) FullBlocks(r, kids, now);
    }

    // ================================ the state ================================

    static SkillBlockUi Match(const RawBlock& b, const std::vector<CharSkill>& skills) {
        SkillBlockUi u;
        u.element = b.element;
        u.header = b.header;
        u.headerRow = b.headerRow;
        u.bigSocket = b.big;
        u.sockets = b.sockets;
        u.socketCount = b.shown;
        u.filled = b.filled;
        u.name = b.name;
        int found = -1, candidates = 0;
        if (b.idRead) {
            for (size_t i = 0; i < skills.size(); ++i) {
                if (skills[i].datRow != static_cast<int>(b.row) || skills[i].slotX != static_cast<int>(b.slot)) continue;
                ++candidates;
                if (found < 0 || (NameMatches(b.name, skills[i].gem.name) && !NameMatches(b.name, skills[static_cast<size_t>(found)].gem.name)))
                    found = static_cast<int>(i);
            }
            if (candidates > 1 && !NameMatches(b.name, skills[static_cast<size_t>(found)].gem.name)) found = -1;
        }
        if (found < 0)
            for (size_t i = 0; i < skills.size() && found < 0; ++i)
                if (NameMatches(b.name, skills[i].gem.name)) found = static_cast<int>(i);
        u.skill = found;
        if (found >= 0 && b.idsOk) {
            const CharSkill& s = skills[static_cast<size_t>(found)];
            bool agree = s.socketCount == b.shown && s.supports.size() == static_cast<size_t>(s.socketCount);
            for (int k = 0; agree && k < b.shown; ++k)
                agree = b.filled[static_cast<size_t>(k)] == s.supports[static_cast<size_t>(k)].present;
            u.socketsAgree = agree;
        }
        return u;
    }

    static SkillsState Compose(SkillsReader& r) {
        SkillsWork& w = Work(r);
        SkillsState s;
        s.skillsKnown = w.skillsKnown;
        s.skills = w.skills;
        s.panelFound = w.panel != 0;
        s.panelBuilt = w.panelBuilt;
        s.panelOpen = w.panelOpen;
        s.panel = w.panel;
        s.scrollView = w.view;
        s.tooltipSource = w.tooltipSource;
        if (w.panelOpen)
            for (const RawBlock& b : w.blocks) s.blocks.push_back(Match(b, w.skills));
        if (!w.rootOk) s.problem = SkillsProblem::NoPlayerData;
        else if (!w.skillsKnown) s.problem = w.skillsProblem;
        else s.problem = w.panel != 0 ? SkillsProblem::None : SkillsProblem::NoPanel;
        return s;
    }

    static std::shared_ptr<const SkillsState> Publish(SkillsReader& r, SkillsState&& s) {
        const SkillsState* last = r.m_state.get();
        if (last && SameState(*last, s)) return r.m_state;
        auto n = std::make_shared<SkillsState>(std::move(s));
        n->serial = r.m_nextSerial++;
        n->skillsSerial = (last && last->skillsKnown == n->skillsKnown && SameSkills(last->skills, n->skills)) ? last->skillsSerial
                                                                                                                 : r.m_nextSerial++;
        r.m_state = std::move(n);
        return r.m_state;
    }
};

SkillsReader::SkillsReader(IGame& game, const Offsets& offsets) : m_game(game), m_off(offsets) {
    m_state = std::make_shared<const SkillsState>();
    m_work = std::make_shared<SkillsWork>();
}

void SkillsReader::Kick() {
    SkillsWork& w = SkillsReaderImpl::Work(*this);
    w.skillsDue = true;
    w.skillsFailures = 0;
    m_retry.Kick();
}

void SkillsReader::Reset() {
    m_state = std::make_shared<const SkillsState>();
    m_retry = RetryTimer{};
    m_areaSerial = 0;
    m_lastSkillsMs = 0;
    m_lastBlocksMs = 0;
    m_wasOpen = false;
    m_work = std::make_shared<SkillsWork>();
}

std::shared_ptr<const SkillsState> SkillsReader::Tick(const WatchTick& t) {
    try {
        SkillsWork& w = SkillsReaderImpl::Work(*this);
        if (!t.status.inGame) {
            if (w.haveArea || w.panel != 0) *m_work = SkillsWork{};
            m_wasOpen = false;
            return SkillsReaderImpl::Publish(*this, SkillsState{});
        }
        if (!w.haveArea || t.status.areaSerial != m_areaSerial) {
            // Everything read from the character or the panel is per area; the panel is checked again.
            *m_work = SkillsWork{};
            w.haveArea = true;
            m_areaSerial = t.status.areaSerial;
            m_wasOpen = false;
            m_retry.Kick();
        }
        const uint64_t root = m_game.GameUiRoot();
        w.rootOk = PlausibleObject(root);
        if (w.sourceDue || t.nowMs - w.lastSourceMs >= kTooltipSourceMs || t.nowMs < w.lastSourceMs) {
            w.tooltipSource = FindTooltipSource(m_game, m_off, root);
            w.lastSourceMs = t.nowMs;
            w.sourceDue = false;
        }
        if (!w.rootOk) {
            SkillsReaderImpl::DropPanel(*this);
            w.skillsKnown = false;
            w.skills.clear();
            return SkillsReaderImpl::Publish(*this, SkillsReaderImpl::Compose(*this));
        }
        SkillsReaderImpl::UpdatePanel(*this, root, t.nowMs);
        if (SkillsReaderImpl::SkillsDue(*this, t.nowMs)) SkillsReaderImpl::ReadSkills(*this, root, t.nowMs);
        return SkillsReaderImpl::Publish(*this, SkillsReaderImpl::Compose(*this));
    } catch (...) {
        return m_state;   // out of memory: keep the last state
    }
}

SkillsFrame ReadSkillsFrame(IGame& game, const SkillsState& state) {
    SkillsFrame f;
    try {
        if (!state.panelFound || !state.panelOpen || state.panel == 0 || state.scrollView == 0) return f;
        const GameTooltip tooltip = ReadGameTooltip(game, state.tooltipSource);
        f.tooltip = tooltip;
        std::vector<uint64_t> addrs;
        constexpr size_t kSockets = static_cast<size_t>(kMaxSupportSockets);
        addrs.reserve(2 + state.blocks.size() * (3 + kSockets));
        addrs.push_back(state.panel);
        addrs.push_back(state.scrollView);
        for (const SkillBlockUi& b : state.blocks) {
            addrs.push_back(b.element);
            addrs.push_back(b.headerRow);
            addrs.push_back(b.bigSocket);
            for (const uint64_t s : b.sockets)
                if (s != 0) addrs.push_back(s);
        }
        std::vector<Rect> rects(addrs.size());
        game.UiRects(addrs.data(), addrs.size(), rects.data());
        const auto usable = [](const Rect& r) {
            return r.ok && std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h) && r.w >= 0.0f &&
                   r.h >= 0.0f && std::fabs(r.x) < 1e6f && std::fabs(r.y) < 1e6f && r.w < 1e6f && r.h < 1e6f;
        };
        const auto take = [&](size_t i) { return usable(rects[i]) ? rects[i] : Rect{}; };
        f.panel = take(0);
        f.clip = take(1);
        if (!f.panel.ok || !f.clip.ok || f.clip.w <= 0.0f || f.clip.h <= 0.0f) {
            f = SkillsFrame{};
            f.tooltip = tooltip;
            return f;
        }
        size_t at = 2;
        f.blocks.resize(state.blocks.size());
        for (size_t i = 0; i < state.blocks.size(); ++i) {
            const SkillBlockUi& b = state.blocks[i];
            SkillsFrame::Block& fb = f.blocks[i];
            fb.block = take(at++);
            fb.header = take(at++);
            fb.bigSocket = take(at++);
            for (size_t k = 0; k < kSockets; ++k)
                if (b.sockets[k] != 0) fb.sockets[k] = take(at++);
            const Rect& r = fb.block;
            fb.inView = r.ok && r.x < f.clip.Right() && r.Right() > f.clip.x && r.y < f.clip.Bottom() && r.Bottom() > f.clip.y;
        }
        f.ok = true;
    } catch (...) {
        f = SkillsFrame{};
    }
    return f;
}

} // namespace bp
