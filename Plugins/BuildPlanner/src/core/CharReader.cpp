// BuildPlanner - the character's passives, points and level (reader)
// CharReader.cpp - see CharReader.h. Reads the game READ-ONLY through IGame; every pointer, count and
// value read is validated before it is used.
//
// Rules this file adds to the header's contract (tools/make_fixture.py mirrors the reads):
//   - The configured object slot needs its alias too ([PSD + passiveObjAlias] == object + objSelfBias);
//     a slot found by the scan needs only the self-pointer pair and the vtable inside the module.
//   - A slot found by a scan must hold a character with at least one allocated passive (a zeroed
//     structure cannot pass); the configured / cached slots also accept an empty list (a new
//     character, a full respec).
//   - Attribute choices of nodes that are not allocated are ignored; any other entry that is not an
//     attribute node with a known option refuses the whole read (BadList): a torn vector reads as such.
//   - The vectors and scalars move together: when the allocated vector is found elsewhere in the
//     object (vector scan), every object field is read at the same shift.
//   - The open-tree cross-check (node state bytes) runs when the tree is open and the list or the
//     table changed, at most every 2 s otherwise; it refuses (BadList) only a gross disagreement
//     (more than max(4, a tenth of the list) nodes), since the UI may lag an allocation by a frame.
//   - Class without ascendancy nodes: the player path's class unless the class start with an
//     allocated neighbour belongs to another class (then unknown: the planner takes the build's).
#include "CharReader.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <string_view>

namespace bp {
namespace {

constexpr size_t kMaxListEntries = 400;
constexpr size_t kMaxJewelSockets = 64;
constexpr size_t kMaxPsdVector = 64;
constexpr uint32_t kMaxObjectBlock = 0x2000;   // one read of the object (default 0x2F0 bytes)
constexpr uint32_t kMaxScanBytes = 0x4000;     // a scan range is cut to this (a bad offsets.json cannot make a huge read)
constexpr int kMaxLevel = 100, kMaxQuestPoints = 60, kMaxAscPoints = 16, kMaxWeaponSetPoints = 100;
constexpr int64_t kUiCheckEveryMs = 2000;

// Inventories of the player entity's class (Characters.dat Id -> the tree data's class name).
struct PathClass {
    std::string_view path;
    std::string_view className;
};
constexpr PathClass kPathClasses[] = {
    {"Metadata/Characters/Str/StrFour", "Marauder"},       {"Metadata/Characters/Int/IntFour", "Witch"},
    {"Metadata/Characters/Dex/DexFour", "Ranger"},         {"Metadata/Characters/StrDex/StrDexFour", "Duelist"},
    {"Metadata/Characters/DexInt/DexIntFour", "Shadow"},   {"Metadata/Characters/StrInt/StrIntFour", "Templar"},
    {"Metadata/Characters/Str/StrFourb", "Warrior"},       {"Metadata/Characters/Int/IntFourb", "Sorceress"},
    {"Metadata/Characters/Dex/DexFourb", "Huntress"},      {"Metadata/Characters/StrDex/StrDexFourb", "Mercenary"},
    {"Metadata/Characters/DexInt/DexIntFourb", "Monk"},    {"Metadata/Characters/StrInt/StrIntFourb", "Druid"},
};

bool IsAlnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// The class name of a player entity path; "" when the path is not a known character. A path may
// carry a suffix ("...IntFourb@2"), never another character's name ("IntFour" is not "IntFourb").
std::string_view ClassOfPath(std::string_view path) {
    for (const PathClass& p : kPathClasses)
        if (path.size() >= p.path.size() && path.compare(0, p.path.size(), p.path) == 0 &&
            (path.size() == p.path.size() || !IsAlnum(path[p.path.size()])))
            return p.className;
    return {};
}

template <class T>
T At(const std::vector<uint8_t>& block, uint32_t off) {
    T v{};
    if (static_cast<size_t>(off) + sizeof(T) <= block.size()) std::memcpy(&v, block.data() + off, sizeof(T));
    return v;
}

// A std::vector {begin, end, capacity} of `elem`-byte entries: the count, or -1 when it cannot be one.
int64_t VectorCount(uint64_t begin, uint64_t end, uint64_t cap, uint32_t elem, size_t maxCount) {
    if (begin == 0 && end == 0 && cap == 0) return 0;
    if (elem == 0 || !PlausiblePtr(begin) || end < begin || cap < end || !PlausiblePtr(cap)) return -1;
    if ((end - begin) % elem != 0) return -1;
    const uint64_t n = (end - begin) / elem;
    return n <= maxCount ? static_cast<int64_t>(n) : -1;
}

struct Vec {
    int64_t count = -1;   // -1 = malformed
    uint64_t begin = 0;
};

Vec VecAt(const std::vector<uint8_t>& block, uint32_t off, uint32_t elem, size_t maxCount) {
    Vec v;
    if (static_cast<size_t>(off) + 24 > block.size()) return v;
    v.begin = At<uint64_t>(block, off);
    v.count = VectorCount(v.begin, At<uint64_t>(block, off + 8), At<uint64_t>(block, off + 16), elem, maxCount);
    return v;
}

// One attempt at reading the character at given slots. state.problem says why it failed.
struct Attempt {
    CharProblem problem = CharProblem::NoPlayerData;
    CharState state;
    bool objectOk = false;   // the object validated (self pointers, vtable): a vector scan may help
};

} // namespace

struct CharReaderImpl {
    static uint32_t ObjectBlockEnd(const Offsets& o, int32_t shift) {
        const int64_t fields[][2] = {{o.objSelfA, 8},
                                     {o.objSelfB, 8},
                                     {static_cast<int64_t>(o.objAllocated) + shift, 24},
                                     {static_cast<int64_t>(o.objAttributes) + shift, 24},
                                     {static_cast<int64_t>(o.objGranted) + shift, 24},
                                     {static_cast<int64_t>(o.objJewelSockets) + shift, 24},
                                     {static_cast<int64_t>(o.objLevel) + shift, 4},
                                     {static_cast<int64_t>(o.objQuestPoints) + shift, 4},
                                     {static_cast<int64_t>(o.objWeaponSetPoints) + shift, 4},
                                     {static_cast<int64_t>(o.objAscPointsA) + shift, 4},
                                     {static_cast<int64_t>(o.objAscPointsB) + shift, 4}};
        int64_t end = 8;   // the vtable
        for (const auto& f : fields) {
            if (f[0] < 0) return 0;
            end = std::max(end, f[0] + f[1]);
        }
        return end <= kMaxObjectBlock ? static_cast<uint32_t>(end) : 0;
    }

    static bool InModule(IGame& g, uint64_t p) {
        const uint64_t base = g.ModuleBase(), size = g.ModuleSize();
        return base != 0 && size != 0 && p >= base && p - base < size;
    }

    // The passive object at `obj`: self pointers and vtable (no vectors).
    static bool ObjectSignature(IGame& g, const Offsets& o, uint64_t obj, const std::vector<uint8_t>& block) {
        return At<uint64_t>(block, o.objSelfA) == obj + o.objSelfBias && At<uint64_t>(block, o.objSelfB) == obj &&
               InModule(g, At<uint64_t>(block, 0));
    }

    // Validates and decodes the allocated list at `off` of the object block. `strict`: at least one entry.
    static CharProblem ReadAllocated(CharReader& r, const std::vector<uint8_t>& block, uint32_t off, bool strict,
                                     std::vector<AllocatedNode>& out, int& unknown) {
        out.clear();
        unknown = 0;
        const Vec v = VecAt(block, off, 4, kMaxListEntries);
        if (v.count < 0 || (strict && v.count == 0)) return CharProblem::BadList;
        if (v.count == 0) return CharProblem::None;
        std::vector<uint32_t> raw(static_cast<size_t>(v.count));
        if (!r.m_game.Read(v.begin, raw.data(), raw.size() * 4)) return CharProblem::ReadFailed;
        int known = 0;
        for (size_t i = 0; i < raw.size(); ++i) {
            const uint16_t id = static_cast<uint16_t>(raw[i] & 0xFFFFu);
            const uint32_t set = (raw[i] >> 16) & 0xFFu;
            if (i > 0 && id <= static_cast<uint16_t>(raw[i - 1] & 0xFFFFu)) return CharProblem::BadList;
            if (set > 2) return CharProblem::BadList;
            if (r.m_tree->IndexOf(id) != kNoIndex) ++known;
            else ++unknown;
            out.push_back({r.m_tree->BaseId(id), static_cast<WeaponSet>(set)});
        }
        if (static_cast<size_t>(known) * 10 < raw.size() * 9) return CharProblem::UnknownIds;
        std::stable_sort(out.begin(), out.end(), [](const AllocatedNode& a, const AllocatedNode& b) { return a.id < b.id; });
        out.erase(std::unique(out.begin(), out.end(), [](const AllocatedNode& a, const AllocatedNode& b) { return a.id == b.id; }),
                  out.end());
        return CharProblem::None;
    }

    // Everything at given PSD / object slots. `configuredObject`: the alias must agree too.
    static Attempt ReadAt(CharReader& r, uint64_t root, uint32_t psdSlot, uint32_t objSlot, int32_t shift, bool strict,
                          bool configuredObject) {
        Attempt a;
        IGame& g = r.m_game;
        const Offsets& o = r.m_off;
        const PassiveTree& tree = *r.m_tree;
        CharState& s = a.state;

        const uint64_t psd = g.Ptr(root + psdSlot);
        if (!PlausibleObject(psd)) {
            a.problem = CharProblem::NoPlayerData;
            return a;
        }
        s.psd = psd;
        const uint64_t obj = g.Ptr(psd + objSlot);
        if (!PlausibleObject(obj)) {
            a.problem = CharProblem::NoPassiveObject;
            return a;
        }
        s.object = obj;
        const uint32_t blockEnd = ObjectBlockEnd(o, shift);
        if (blockEnd == 0) {
            a.problem = CharProblem::BadList;
            return a;
        }
        std::vector<uint8_t> block(blockEnd);
        if (!g.Read(obj, block.data(), block.size())) {
            a.problem = CharProblem::ReadFailed;
            return a;
        }
        if (!ObjectSignature(g, o, obj, block) ||
            (configuredObject && o.passiveObjAlias != 0 && g.Ptr(psd + o.passiveObjAlias) != obj + o.objSelfBias)) {
            a.problem = CharProblem::NoPassiveObject;
            return a;
        }
        a.objectOk = true;
        const auto off = [shift](uint32_t field) { return static_cast<uint32_t>(static_cast<int64_t>(field) + shift); };

        int unknown = 0;
        a.problem = ReadAllocated(r, block, off(o.objAllocated), strict, s.allocated, unknown);
        s.unknownIds = unknown;
        if (a.problem != CharProblem::None) return a;

        // Attribute choices.
        const Vec av = VecAt(block, off(o.objAttributes), 4, kMaxListEntries);
        if (av.count < 0) {
            a.problem = CharProblem::BadList;
            return a;
        }
        if (av.count > 0) {
            std::vector<uint32_t> raw(static_cast<size_t>(av.count));
            if (!g.Read(av.begin, raw.data(), raw.size() * 4)) {
                a.problem = CharProblem::ReadFailed;
                return a;
            }
            for (const uint32_t e : raw) {
                const uint16_t nodeId = tree.BaseId(static_cast<uint16_t>(e & 0xFFFFu));
                const Node* node = tree.Find(nodeId);
                const Attr attr = tree.AttrOfOption(static_cast<uint16_t>(e >> 16));
                if (node == nullptr || !node->Is(kNodeAttribute) || attr == Attr::None) {
                    a.problem = CharProblem::BadList;
                    return a;
                }
                const bool allocated = std::binary_search(s.allocated.begin(), s.allocated.end(), AllocatedNode{nodeId, WeaponSet::None},
                                                          [](const AllocatedNode& x, const AllocatedNode& y) { return x.id < y.id; });
                if (allocated) s.attributes.push_back({nodeId, attr});
            }
            std::stable_sort(s.attributes.begin(), s.attributes.end(), [](const ChosenAttr& x, const ChosenAttr& y) { return x.node < y.node; });
            s.attributes.erase(std::unique(s.attributes.begin(), s.attributes.end(),
                                           [](const ChosenAttr& x, const ChosenAttr& y) { return x.node == y.node; }),
                               s.attributes.end());
        }

        // Nodes granted by items.
        const Vec gv = VecAt(block, off(o.objGranted), 2, kMaxListEntries);
        if (gv.count < 0) {
            a.problem = CharProblem::BadList;
            return a;
        }
        if (gv.count > 0) {
            std::vector<uint16_t> raw(static_cast<size_t>(gv.count));
            if (!g.Read(gv.begin, raw.data(), raw.size() * 2)) {
                a.problem = CharProblem::ReadFailed;
                return a;
            }
            for (const uint16_t id : raw) {
                if (tree.IndexOf(id) == kNoIndex) {   // item-granted passives are tree passives: a torn vector
                    a.problem = CharProblem::BadList;
                    return a;
                }
                s.granted.push_back(tree.BaseId(id));
            }
            std::sort(s.granted.begin(), s.granted.end());
            s.granted.erase(std::unique(s.granted.begin(), s.granted.end()), s.granted.end());
        }

        // Jewel sockets (records of jewelSocketStride bytes, the graph id inside). The id's offset is
        // compared without `+ 2`: jewelSocketGraphId 0xFFFFFFFE (config/offsets.json) would wrap it.
        if (o.jewelSocketStride >= 2 && o.jewelSocketStride <= 0x100 && o.jewelSocketGraphId <= o.jewelSocketStride - 2) {
            const Vec jv = VecAt(block, off(o.objJewelSockets), o.jewelSocketStride, kMaxJewelSockets);
            if (jv.count < 0) {
                a.problem = CharProblem::BadList;
                return a;
            }
            if (jv.count > 0) {
                std::vector<uint8_t> raw(static_cast<size_t>(jv.count) * o.jewelSocketStride);
                if (!g.Read(jv.begin, raw.data(), raw.size())) {
                    a.problem = CharProblem::ReadFailed;
                    return a;
                }
                for (int64_t i = 0; i < jv.count; ++i) {
                    uint16_t id = 0;
                    std::memcpy(&id, raw.data() + static_cast<size_t>(i) * o.jewelSocketStride + o.jewelSocketGraphId, 2);
                    if (tree.IndexOf(id) == kNoIndex) {
                        a.problem = CharProblem::BadList;
                        return a;
                    }
                    s.jewelSockets.push_back(tree.BaseId(id));
                }
                std::sort(s.jewelSockets.begin(), s.jewelSockets.end());
                s.jewelSockets.erase(std::unique(s.jewelSockets.begin(), s.jewelSockets.end()), s.jewelSockets.end());
            }
        }

        // Scalars.
        const int32_t level = At<int32_t>(block, off(o.objLevel));
        const int32_t quest = At<int32_t>(block, off(o.objQuestPoints));
        const int32_t wsp = At<int32_t>(block, off(o.objWeaponSetPoints));
        const int32_t ascA = At<int32_t>(block, off(o.objAscPointsA));
        const int32_t ascB = At<int32_t>(block, off(o.objAscPointsB));
        if (level < 1 || level > kMaxLevel || quest < 0 || quest > kMaxQuestPoints || wsp < 0 || wsp > kMaxWeaponSetPoints ||
            ascA < 0 || ascA > kMaxAscPoints || ascB < 0 || ascB > kMaxAscPoints) {
            a.problem = CharProblem::BadScalars;
            return a;
        }
        s.level = level;
        s.questPoints = quest;
        s.weaponSetPoints = wsp;
        s.ascPointsTotal = std::max(ascA, ascB);

        // Points: only nodes that cost one (Node::CostsPoint).
        int ordinary = 0;
        std::map<std::string, int> ascNames;
        for (const AllocatedNode& n : s.allocated) {
            const Node* node = tree.Find(n.id);
            if (node == nullptr) continue;
            if (node->IsAscendancy()) ++ascNames[node->ascendancy];
            if (!node->CostsPoint()) continue;
            if (node->IsAscendancy()) {
                ++s.ascAllocated;
                continue;
            }
            ++s.mainAllocated;
            if (n.set == WeaponSet::One) ++s.set1Count;
            else if (n.set == WeaponSet::Two) ++s.set2Count;
            else ++ordinary;
        }
        s.freePoints = (level - 1) + quest - (ordinary + std::max(s.set1Count, s.set2Count));
        s.freeSet1 = wsp - s.set1Count;
        s.freeSet2 = wsp - s.set2Count;
        s.freeAscPoints = s.ascPointsTotal - s.ascAllocated;
        if (s.freePoints < 0 || s.freeAscPoints < 0 || s.freeSet1 < 0 || s.freeSet2 < 0) {
            a.problem = CharProblem::BadScalars;
            return a;
        }

        // Who the character is.
        int bestAsc = 0;
        for (const auto& [name, count] : ascNames)
            if (count > bestAsc) {
                bestAsc = count;
                s.ascendancy = name;
            }
        const PlayerInfo player = g.Player();
        const ClassInfo* cls = nullptr;
        if (!s.ascendancy.empty()) {
            const AscendancyInfo* asc = tree.AscendancyByNodesName(s.ascendancy, &cls);
            if (asc) s.ascendancyId = asc->internalId;
            else cls = nullptr;
        } else {
            cls = ClassFromStart(tree, s.allocated, player.valid ? ClassOfPath(player.path) : std::string_view());
        }
        if (cls) {
            s.classId = cls->integerId;
            s.className = cls->name;
        }

        // The level cross-check with the host's player.
        if (!player.valid) {
            a.problem = CharProblem::ReadFailed;
            return a;
        }
        if (player.level != level) {
            a.problem = CharProblem::LevelMismatch;
            return a;
        }
        s.problem = CharProblem::None;
        s.valid = true;
        a.problem = CharProblem::None;
        return a;
    }

    // The class without ascendancy nodes: the path's class, cross-checked against the class start that
    // has an allocated neighbour.
    static const ClassInfo* ClassFromStart(const PassiveTree& tree, const std::vector<AllocatedNode>& allocated,
                                           std::string_view pathClass) {
        const auto isAllocated = [&](uint16_t id) {
            return std::binary_search(allocated.begin(), allocated.end(), AllocatedNode{id, WeaponSet::None},
                                      [](const AllocatedNode& x, const AllocatedNode& y) { return x.id < y.id; });
        };
        uint32_t start = kNoIndex;
        int starts = 0;
        for (uint32_t i = 0; i < tree.nodes.size(); ++i) {
            const Node& n = tree.nodes[i];
            if (!n.Is(kNodeClassStart)) continue;
            for (const uint32_t l : n.links)
                if (l < tree.nodes.size() && !tree.nodes[l].IsAscendancy() && isAllocated(tree.nodes[l].id)) {
                    start = i;
                    ++starts;
                    break;
                }
        }
        if (starts > 1) start = kNoIndex;
        const ClassInfo* byPath = pathClass.empty() ? nullptr : tree.ClassByName(pathClass);
        if (byPath) {
            if (start == kNoIndex) return byPath;
            const auto& names = tree.nodes[start].classesStart;
            return std::find(names.begin(), names.end(), byPath->name) != names.end() ? byPath : nullptr;
        }
        if (start == kNoIndex) return nullptr;
        const ClassInfo* only = nullptr;
        for (const std::string& name : tree.nodes[start].classesStart) {
            const ClassInfo* c = tree.ClassByName(name);
            if (c == nullptr) continue;
            if (only != nullptr) return nullptr;
            only = c;
        }
        return only;
    }

    // ---- the bounded fallback scans (docs/OFFSETS.md section 4.2) ----

    // The PSD slot: a pointer in the panel root's scan range equal to one of its independent holders.
    static std::vector<uint32_t> ScanPsdSlots(CharReader& r, uint64_t root, const TreeTable* table) {
        std::vector<uint32_t> out;
        IGame& g = r.m_game;
        const Offsets& o = r.m_off;
        std::vector<uint64_t> holders;
        const uint64_t sd = g.Ptr(root + o.serverDataSlot);
        if (PlausibleObject(sd)) {
            uint64_t v[3] = {};
            if (g.Read(sd + o.serverDataPsdVector, v, sizeof(v)) && VectorCount(v[0], v[1], v[2], 8, kMaxPsdVector) > 0)
                holders.push_back(g.Ptr(v[0]));
        }
        uint64_t panel = (table && table->found) ? table->panel : g.Ptr(root + o.treePanelSlot);
        if (PlausibleObject(panel)) {
            holders.push_back(g.Ptr(panel + o.treePanelPsdA));
            holders.push_back(g.Ptr(panel + o.treePanelPsdB));
        }
        holders.erase(std::remove_if(holders.begin(), holders.end(), [](uint64_t h) { return !PlausibleObject(h); }), holders.end());
        if (holders.empty() || o.psdScanTo <= o.psdScanFrom) return out;
        const uint32_t from = o.psdScanFrom & ~7u;
        const uint32_t to = std::min(o.psdScanTo, from + kMaxScanBytes);
        if (to <= from + 8) return out;
        std::vector<uint64_t> block((to - from) / 8);
        if (!g.Read(root + from, block.data(), block.size() * 8)) return out;
        for (size_t i = 0; i < block.size(); ++i)
            if (std::find(holders.begin(), holders.end(), block[i]) != holders.end()) out.push_back(from + static_cast<uint32_t>(i * 8));
        const uint32_t cfg = o.psdSlot;
        std::stable_sort(out.begin(), out.end(), [cfg](uint32_t a, uint32_t b) {
            return (a > cfg ? a - cfg : cfg - a) < (b > cfg ? b - cfg : cfg - b);
        });
        return out;
    }

    // The object slot: a pointer in the PSD's scan range to an object with the self-pointer pair.
    static std::vector<uint32_t> ScanObjectSlots(CharReader& r, uint64_t psd) {
        std::vector<uint32_t> out;
        IGame& g = r.m_game;
        const Offsets& o = r.m_off;
        const uint32_t from = o.objScanFrom & ~7u;
        const uint32_t to = std::min(o.objScanTo, from + kMaxScanBytes);
        if (to <= from + 8) return out;
        std::vector<uint64_t> block((to - from) / 8);
        if (!g.Read(psd + from, block.data(), block.size() * 8)) return out;
        std::vector<uint64_t> tried;
        for (size_t i = 0; i < block.size(); ++i) {
            const uint64_t p = block[i];
            if (!PlausibleObject(p) || std::find(tried.begin(), tried.end(), p) != tried.end()) continue;
            tried.push_back(p);
            uint64_t a = 0, b = 0;
            if (o.objSelfB == o.objSelfA + 8) {
                uint64_t pair[2] = {};
                if (!g.Read(p + o.objSelfA, pair, sizeof(pair))) continue;
                a = pair[0];
                b = pair[1];
            } else if (!g.Get(p + o.objSelfA, a) || !g.Get(p + o.objSelfB, b)) {
                continue;
            }
            if (a == p + o.objSelfBias && b == p) out.push_back(from + static_cast<uint32_t>(i * 8));
        }
        const uint32_t cfg = o.passiveObjSlot;
        std::stable_sort(out.begin(), out.end(), [cfg](uint32_t x, uint32_t y) {
            return (x > cfg ? x - cfg : cfg - x) < (y > cfg ? y - cfg : cfg - y);
        });
        return out;
    }

    // The allocated vector inside a validated object: a sane, ascending, known list with a sane
    // attribute vector at the same distance as configured. Returns the shift, or false.
    static bool ScanVector(CharReader& r, uint64_t obj, int32_t& shift) {
        const Offsets& o = r.m_off;
        if (o.vecScanTo <= o.vecScanFrom) return false;
        const uint32_t from = o.vecScanFrom & ~7u;
        const uint32_t to = std::min(o.vecScanTo, from + kMaxScanBytes);
        const int64_t attrDelta = static_cast<int64_t>(o.objAttributes) - o.objAllocated;
        std::vector<uint8_t> block(to + 24 + static_cast<uint32_t>(std::max<int64_t>(0, attrDelta)));
        if (!r.m_game.Read(obj, block.data(), block.size())) return false;
        const uint32_t cfg = o.objAllocated;
        std::vector<uint32_t> offs;
        for (uint32_t off = from; off + 8 <= to; off += 8) offs.push_back(off);
        std::stable_sort(offs.begin(), offs.end(), [cfg](uint32_t x, uint32_t y) {
            return (x > cfg ? x - cfg : cfg - x) < (y > cfg ? y - cfg : cfg - y);
        });
        for (const uint32_t off : offs) {
            std::vector<AllocatedNode> list;
            int unknown = 0;
            if (ReadAllocated(r, block, off, true, list, unknown) != CharProblem::None) continue;
            const int64_t attrOff = static_cast<int64_t>(off) + attrDelta;
            if (attrOff < 0 || VecAt(block, static_cast<uint32_t>(attrOff), 4, kMaxListEntries).count < 0) continue;
            shift = static_cast<int32_t>(static_cast<int64_t>(off) - o.objAllocated);
            return true;
        }
        return false;
    }

    // Everything the scans may find, best first; returns the successful attempt (or the first failure).
    static bool Probe(CharReader& r, uint64_t root, const TreeTable* table, Attempt& best) {
        const Offsets& o = r.m_off;
        std::vector<uint32_t> psdSlots{o.psdSlot};
        for (const uint32_t s : ScanPsdSlots(r, root, table))
            if (std::find(psdSlots.begin(), psdSlots.end(), s) == psdSlots.end()) psdSlots.push_back(s);
        for (const uint32_t psdSlot : psdSlots) {
            const bool psdScanned = psdSlot != o.psdSlot;
            Attempt a = ReadAt(r, root, psdSlot, o.passiveObjSlot, 0, psdScanned, true);
            if (a.problem == CharProblem::None) {
                Adopt(r, psdSlot, 0, 0, a, psdScanned, false);
                best = std::move(a);
                return true;
            }
            const uint64_t psd = r.m_game.Ptr(root + psdSlot);
            if (!PlausibleObject(psd)) continue;
            std::vector<uint32_t> objSlots;
            if (a.objectOk) objSlots.push_back(o.passiveObjSlot);
            for (const uint32_t s : ScanObjectSlots(r, psd))
                if (std::find(objSlots.begin(), objSlots.end(), s) == objSlots.end()) objSlots.push_back(s);
            for (const uint32_t objSlot : objSlots) {
                const bool objScanned = objSlot != o.passiveObjSlot;
                Attempt b = objScanned ? ReadAt(r, root, psdSlot, objSlot, 0, true, false) : a;
                if (b.problem == CharProblem::None) {
                    Adopt(r, psdSlot, objSlot, 0, b, psdScanned, objScanned);
                    best = std::move(b);
                    return true;
                }
                if (!b.objectOk) continue;
                int32_t shift = 0;
                if (!ScanVector(r, b.state.object, shift) || shift == 0) continue;
                Attempt c = ReadAt(r, root, psdSlot, objSlot, shift, true, !objScanned);
                if (c.problem == CharProblem::None) {
                    Adopt(r, psdSlot, objSlot, shift, c, psdScanned, true);
                    best = std::move(c);
                    return true;
                }
            }
            if (psdSlot == o.psdSlot) best = std::move(a);
        }
        return false;
    }

    static void Adopt(CharReader& r, uint32_t psdSlot, uint32_t objSlot, int32_t shift, Attempt& a, bool psdScanned, bool objScanned) {
        r.m_psdSlot = psdScanned ? psdSlot : 0;
        r.m_objSlot = objScanned && objSlot != 0 && objSlot != r.m_off.passiveObjSlot ? objSlot : 0;
        r.m_fieldShift = shift;
        a.state.psdViaScan = psdScanned;
        a.state.objectViaScan = objScanned;
    }

    // The node state bytes of the open tree against the list (TreeTable nodes only).
    static int UiDiffers(CharReader& r, const TreeTable& table, const std::vector<AllocatedNode>& list) {
        int differs = 0;
        for (const TreeNodeUi& n : table.nodes) {
            const Node& node = table.tree->nodes[n.node];
            if (node.Is(kNodeAscStart)) continue;   // the game shows it allocated, never lists it
            uint8_t state = 0;
            if (!r.m_game.Get(n.element + r.m_off.nodeState, state)) {
                ++differs;
                continue;
            }
            const auto it = std::lower_bound(list.begin(), list.end(), AllocatedNode{n.id, WeaponSet::None},
                                             [](const AllocatedNode& x, const AllocatedNode& y) { return x.id < y.id; });
            const bool inList = it != list.end() && it->id == n.id;
            const bool inUi = state <= 2;
            if (inList != inUi || (inList && static_cast<uint8_t>(it->set) != state)) ++differs;
        }
        return differs;
    }

    static bool SameContent(const CharState& a, const CharState& b) {
        return a.problem == b.problem && a.valid == b.valid && a.allocated == b.allocated && a.attributes == b.attributes &&
               a.granted == b.granted && a.jewelSockets == b.jewelSockets && a.level == b.level && a.questPoints == b.questPoints &&
               a.weaponSetPoints == b.weaponSetPoints && a.ascPointsTotal == b.ascPointsTotal && a.mainAllocated == b.mainAllocated &&
               a.ascAllocated == b.ascAllocated && a.set1Count == b.set1Count && a.set2Count == b.set2Count &&
               a.freePoints == b.freePoints && a.freeSet1 == b.freeSet1 && a.freeSet2 == b.freeSet2 &&
               a.freeAscPoints == b.freeAscPoints && a.classId == b.classId && a.className == b.className &&
               a.ascendancyId == b.ascendancyId && a.ascendancy == b.ascendancy;
    }

    static bool SameDiagnostics(const CharState& a, const CharState& b) {
        return a.psd == b.psd && a.object == b.object && a.psdViaScan == b.psdViaScan && a.objectViaScan == b.objectViaScan &&
               a.uiChecked == b.uiChecked && a.uiDiffers == b.uiDiffers && a.unknownIds == b.unknownIds;
    }

    static std::shared_ptr<const CharState> Publish(CharReader& r, CharState s) {
        const CharState& prev = *r.m_state;
        if (SameContent(prev, s)) {
            if (SameDiagnostics(prev, s)) return r.m_state;
            s.serial = prev.serial;
        } else {
            s.serial = r.m_nextSerial++;
        }
        r.m_state = std::make_shared<const CharState>(std::move(s));
        return r.m_state;
    }

    static std::shared_ptr<const CharState> Problem(CharReader& r, CharProblem p, const CharState* diag = nullptr) {
        CharState s;
        if (diag) {
            s.psd = diag->psd;
            s.object = diag->object;
            s.psdViaScan = diag->psdViaScan;
            s.objectViaScan = diag->objectViaScan;
            s.unknownIds = diag->unknownIds;
            s.uiChecked = diag->uiChecked;
            s.uiDiffers = diag->uiDiffers;
        }
        s.problem = p;
        s.valid = false;
        return Publish(r, std::move(s));
    }

    static std::shared_ptr<const CharState> Tick(CharReader& r, const WatchTick& t, const TreeTable* table, bool treeOpen) {
        const int64_t now = t.nowMs;
        if (r.m_haveArea && t.status.areaSerial != r.m_areaSerial) r.m_retry.Kick();
        r.m_areaSerial = t.status.areaSerial;
        r.m_haveArea = true;
        if (!t.status.attached || !t.status.inGame) {
            r.m_uiCheckValid = false;
            return Problem(r, CharProblem::NotInGame);
        }
        if (!r.m_tree || r.m_tree->Empty()) return Problem(r, CharProblem::NoTreeData);
        const uint64_t root = r.m_game.GameUiRoot();
        if (root == 0) return Problem(r, CharProblem::NoUiRoot);

        const Offsets& o = r.m_off;
        const uint32_t psdSlot = r.m_psdSlot != 0 ? r.m_psdSlot : o.psdSlot;
        const uint32_t objSlot = r.m_objSlot != 0 ? r.m_objSlot : o.passiveObjSlot;
        const bool cached = r.m_psdSlot != 0 || r.m_objSlot != 0 || r.m_fieldShift != 0;
        Attempt a = ReadAt(r, root, psdSlot, objSlot, r.m_fieldShift, false, r.m_objSlot == 0 && r.m_fieldShift == 0);
        if (a.problem == CharProblem::None) {
            a.state.psdViaScan = r.m_psdSlot != 0;
            a.state.objectViaScan = r.m_objSlot != 0 || r.m_fieldShift != 0;
        } else if (r.m_retry.Due(now)) {
            Attempt best;
            if (Probe(r, root, table, best)) {
                r.m_retry.Succeeded();
                a = std::move(best);
            } else {
                r.m_retry.Failed(now);
                if (!cached) a = std::move(best);   // the configured slots' verdict names the cause best
            }
        }
        if (a.problem != CharProblem::None) {
            r.m_uiCheckValid = false;
            return Problem(r, a.problem, &a.state);
        }

        // The open-tree cross-check.
        CharState& s = a.state;
        if (treeOpen && table && table->found && table->tree == r.m_tree) {
            const bool stale = !r.m_uiCheckValid || r.m_uiCheckedList != s.allocated || r.m_uiCheckedTable != table->serial ||
                               now - r.m_lastUiCheckMs >= kUiCheckEveryMs || now < r.m_lastUiCheckMs;
            if (stale) {
                r.m_uiDiffers = UiDiffers(r, *table, s.allocated);
                r.m_uiCheckValid = true;
                r.m_uiCheckedList = s.allocated;
                r.m_uiCheckedTable = table->serial;
                r.m_lastUiCheckMs = now;
            }
            s.uiChecked = true;
            s.uiDiffers = r.m_uiDiffers;
            if (r.m_uiDiffers > std::max<int>(4, static_cast<int>(s.allocated.size() / 10))) {
                s.valid = false;
                s.problem = CharProblem::BadList;
                return Problem(r, CharProblem::BadList, &s);
            }
        } else {
            r.m_uiCheckValid = false;
        }
        return Publish(r, std::move(s));
    }
};

CharReader::CharReader(IGame& game, const Offsets& offsets) : m_game(game), m_off(offsets) {
    m_state = std::make_shared<const CharState>();
}

void CharReader::SetTree(std::shared_ptr<const PassiveTree> tree) {
    if (tree == m_tree) return;
    m_tree = std::move(tree);
    m_uiCheckValid = false;
    m_retry.Kick();
}

void CharReader::Kick() { m_retry.Kick(); }

void CharReader::Reset() {
    m_state = std::make_shared<const CharState>();
    m_retry = RetryTimer{};
    m_areaSerial = 0;
    m_haveArea = false;
    m_psdSlot = 0;
    m_objSlot = 0;
    m_fieldShift = 0;
    m_uiCheckValid = false;
    m_uiCheckedList.clear();
}

std::shared_ptr<const CharState> CharReader::Tick(const WatchTick& t, const TreeTable* table, bool treeOpen) {
    try {
        return CharReaderImpl::Tick(*this, t, table, treeOpen);
    } catch (...) {
        try {
            m_retry.Failed(t.nowMs);
            return CharReaderImpl::Problem(*this, CharProblem::ReadFailed);
        } catch (...) {
            return m_state;
        }
    }
}

} // namespace bp
