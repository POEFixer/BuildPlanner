// BuildPlanner - the passive tree data
// TreeData.cpp - see TreeData.h. Reads Path of Building (PoE2)'s tree.json, stripped (tools/strip_tree.py)
// or not, with every count and string from the file capped: a downloaded file is untrusted input.
//
// Decisions the header leaves open:
//  - Node indices follow the passive ids (the nodes are sorted by id), so "ties by node id" and index
//    order agree. A duplicate id keeps the first entry in the file.
//  - PoB's group list has null holes (index = the node's group - 1): Node::group is that index and the
//    holes stay as empty groups. An object of groups (keyed by number) is read too.
//  - Connection ids the file does not define (class starts list a few) are ignored, as Path of
//    Building does. Links to or from isOnlyImage nodes are dropped (art, never on a path).
//  - Node::drawnOrbit: the first entry of a pair wins when both ends list each other (one pair in 0_5).
//    An orbit outside orbitRadii, or one whose radius is 0, is kLinkStraight (PoB's BuildConnector finds
//    no radius and draws a straight line).
//  - An attribute option's attribute comes from its name ("Strength" / "Dexterity" / "Intelligence"),
//    else from its position (PoB's override order).
//  - An AscendancyInfo::nodesName is the ascendancy's own name when nodes carry it, else the
//    Node::ascendancy of the first node holding a variant keyed by that name ("Abyssal Lich" -> "Lich").
#include "TreeData.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <utility>

#include <nlohmann/json.hpp>

#include "JsonFile.h"

namespace bp {
namespace {

using json = nlohmann::json;

constexpr double kPi = 3.14159265358979323846;

// Caps on what a file may hold (the 0_5 data: 4914 nodes, 1623 groups, at most 13 links per node,
// 10 orbits, 8 classes of at most 4 ascendancies).
constexpr size_t kMaxTreeNodes = 65536;
constexpr size_t kMaxTreeGroups = 65536;
constexpr size_t kMaxNodeLinks = 128;
constexpr size_t kMaxNodeStats = 32;
constexpr size_t kMaxNodeVariants = 16;
constexpr size_t kMaxUnlockNodes = 32;
constexpr size_t kMaxClassStarts = 8;
constexpr size_t kMaxTreeClasses = 64;
constexpr size_t kMaxClassAscendancies = 16;
constexpr size_t kMaxOrbits = 64;
constexpr size_t kMaxOrbitSlots = 4096;
constexpr size_t kMaxJewelSlots = 1024;
constexpr size_t kMaxIdBytes = 128;      // stringId, internalId, version strings
constexpr size_t kMaxTextBytes = 256;    // names, class / ascendancy names
constexpr size_t kMaxStatBytes = 1024;   // one stat line
constexpr size_t kMaxMetaBytes = 1024;   // bp_meta texts
constexpr size_t kMaxTreeFileBytes = 64u << 20;
constexpr size_t kMinTreeNodes = 1000;
constexpr size_t kMinTreeClasses = 6;

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}

bool IsDigits(std::string_view s, size_t maxLen) {
    if (s.empty() || s.size() > maxLen) return false;
    for (const char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// Cuts a UTF-8 string to at most `cap` bytes without splitting a code point.
std::string Capped(const std::string& s, size_t cap) {
    if (s.size() <= cap) return s;
    size_t cut = cap;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0u) == 0x80u) --cut;
    return s.substr(0, cut);
}

bool Flag(const json& o, const char* key) {
    const auto it = o.find(key);
    if (it == o.end()) return false;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number_integer()) return it->get<int64_t>() != 0;
    return false;
}

std::string Str(const json& o, const char* key, size_t cap) {
    const auto it = o.find(key);
    return (it != o.end() && it->is_string()) ? Capped(it->get_ref<const std::string&>(), cap) : std::string();
}

// An integer from a number (an integral float too) or a string of digits.
bool AsInt(const json& v, int64_t& out) {
    if (v.is_number_integer()) {
        out = v.is_number_unsigned() ? static_cast<int64_t>(std::min<uint64_t>(v.get<uint64_t>(), INT64_MAX))
                                     : v.get<int64_t>();
        return true;
    }
    if (v.is_number_float()) {
        const double d = v.get<double>();
        if (!std::isfinite(d) || d != std::floor(d) || std::fabs(d) > 9.0e15) return false;
        out = static_cast<int64_t>(d);
        return true;
    }
    if (v.is_string()) {
        const std::string& s = v.get_ref<const std::string&>();
        if (!IsDigits(s, 18)) return false;
        out = std::strtoll(s.c_str(), nullptr, 10);
        return true;
    }
    return false;
}

bool Int(const json& o, const char* key, int64_t& out) {
    const auto it = o.find(key);
    return it != o.end() && AsInt(*it, out);
}

bool Num(const json& v, double& out) {
    if (!v.is_number()) return false;
    out = v.get<double>();
    return std::isfinite(out);
}

void Strings(const json& o, const char* key, std::vector<std::string>& out, size_t maxCount, size_t cap) {
    const auto it = o.find(key);
    if (it == o.end() || !it->is_array()) return;
    for (const json& s : *it) {
        if (out.size() >= maxCount) break;
        if (s.is_string()) out.push_back(Capped(s.get_ref<const std::string&>(), cap));
    }
}

bool Fail(std::string* error, std::string what) {
    if (error) *error = std::move(what);
    return false;
}

Attr AttrFromName(std::string_view name, size_t position) {
    if (name.size() >= 3) {
        const std::string_view head = name.substr(0, 3);
        if (EqualsNoCase(head, "str")) return Attr::Str;
        if (EqualsNoCase(head, "dex")) return Attr::Dex;
        if (EqualsNoCase(head, "int")) return Attr::Int;
    }
    return position < 3 ? static_cast<Attr>(position + 1) : Attr::None;
}

// The drawn-line code of a connection's "orbit" value (TreeData.h, Node::drawnOrbit).
int8_t OrbitCode(int64_t v, const std::vector<int>& radii) {
    if (v == 0) return 0;
    const int64_t a = v < 0 ? (v == INT64_MIN ? INT64_MAX : -v) : v;
    if (a > 100 || static_cast<size_t>(a) >= radii.size() || radii[static_cast<size_t>(a)] <= 0) return kLinkStraight;
    return static_cast<int8_t>(v < 0 ? -a : a);
}

struct RawNode {
    Node node;
    int64_t groupKey = -1;
    std::vector<std::pair<int64_t, int64_t>> connections;   // (id, orbit)
    std::vector<int64_t> unlockIds;
};

bool ReadNode(const json& o, int64_t keyId, RawNode& r) {
    int64_t skill = -1;
    if (!Int(o, "skill", skill)) skill = keyId;
    if (skill < 0 || skill > 0xFFFF) return false;
    Node& n = r.node;
    n.id = static_cast<uint16_t>(skill);
    n.stringId = Str(o, "stringId", kMaxIdBytes);
    n.name = Str(o, "name", kMaxTextBytes);
    n.ascendancy = Str(o, "ascendancyName", kMaxTextBytes);
    if (Flag(o, "isNotable")) n.flags |= kNodeNotable;
    if (Flag(o, "isKeystone")) n.flags |= kNodeKeystone;
    if (Flag(o, "isJewelSocket")) n.flags |= kNodeJewel;
    if (Flag(o, "isAscendancyStart")) n.flags |= kNodeAscStart;
    if (Flag(o, "isOnlyImage")) n.flags |= kNodeOnlyImage;
    if (Flag(o, "isAttribute")) n.flags |= kNodeAttribute;
    if (Flag(o, "isMultipleChoice")) n.flags |= kNodeMultipleChoice;
    if (Flag(o, "isMultipleChoiceOption")) n.flags |= kNodeMultipleChoiceOption;
    if (Flag(o, "isFreeAllocate")) n.flags |= kNodeFreeAllocate;
    Strings(o, "classesStart", n.classesStart, kMaxClassStarts, kMaxTextBytes);
    if (!n.classesStart.empty()) n.flags |= kNodeClassStart;
    Strings(o, "stats", n.stats, kMaxNodeStats, kMaxStatBytes);

    if (!Int(o, "group", r.groupKey)) r.groupKey = -1;
    int64_t orbit = 0, orbitIndex = 0;
    Int(o, "orbit", orbit);
    Int(o, "orbitIndex", orbitIndex);
    n.orbit = static_cast<int>(std::clamp<int64_t>(orbit, -1, 1000));
    n.orbitIndex = static_cast<int>(std::clamp<int64_t>(orbitIndex, -1, 100000));

    if (const auto c = o.find("connections"); c != o.end() && c->is_array()) {
        for (const json& e : *c) {
            if (r.connections.size() >= kMaxNodeLinks) break;
            int64_t id = -1, orb = 0;
            if (e.is_object()) {
                if (!Int(e, "id", id)) continue;
                Int(e, "orbit", orb);
            } else if (!AsInt(e, id)) {
                continue;
            }
            if (id >= 0 && id <= 0xFFFF) r.connections.emplace_back(id, orb);
        }
    }

    const auto opts = o.find("options");
    if (n.Is(kNodeAttribute) && opts != o.end() && opts->is_array()) {
        size_t k = 0;
        for (const json& op : *opts) {
            if (k >= 3) break;
            int64_t id = -1;
            if (!op.is_object() || !Int(op, "id", id) || id < 0 || id > 0xFFFF) {
                ++k;
                continue;
            }
            n.options[k].id = static_cast<uint16_t>(id);
            n.options[k].attr = AttrFromName(Str(op, "name", kMaxTextBytes), k);
            ++k;
        }
    }
    if (Flag(o, "isSwitchable") && opts != o.end() && opts->is_object()) {
        n.flags |= kNodeSwitchable;
        for (auto it = opts->begin(); it != opts->end() && n.variants.size() < kMaxNodeVariants; ++it) {
            if (!it.value().is_object() || it.key().empty()) continue;
            NodeVariant v;
            v.key = Capped(it.key(), kMaxTextBytes);
            int64_t id = 0;
            if (Int(it.value(), "id", id) && id > 0 && id <= 0xFFFF) v.id = static_cast<uint16_t>(id);
            v.name = Str(it.value(), "name", kMaxTextBytes);
            if (const auto s = it.value().find("stats"); s != it.value().end() && s->is_array()) {
                v.hasStats = true;
                Strings(it.value(), "stats", v.stats, kMaxNodeStats, kMaxStatBytes);
            }
            n.variants.push_back(std::move(v));
        }
        std::sort(n.variants.begin(), n.variants.end(),
                  [](const NodeVariant& a, const NodeVariant& b) { return a.key < b.key; });
    }
    if (const auto u = o.find("unlockConstraint"); u != o.end() && u->is_object()) {
        n.flags |= kNodeConstrained;
        if (const auto un = u->find("nodes"); un != u->end() && un->is_array())
            for (const json& e : *un) {
                if (r.unlockIds.size() >= kMaxUnlockNodes) break;
                int64_t id = -1;
                if (AsInt(e, id) && id >= 0 && id <= 0xFFFF) r.unlockIds.push_back(id);
            }
    }
    return true;
}

bool ParseTreeImpl(std::string_view text, PassiveTree& out, std::string* error, std::string_view versionHint) {
    const json d = json::parse(text.begin(), text.end(), nullptr, false);
    if (d.is_discarded() || !d.is_object()) return Fail(error, "the tree data is not valid JSON");
    const auto nodesIt = d.find("nodes");
    const auto groupsIt = d.find("groups");
    const auto constIt = d.find("constants");
    if (nodesIt == d.end() || !nodesIt->is_object() || groupsIt == d.end() ||
        !(groupsIt->is_array() || groupsIt->is_object()) || constIt == d.end() || !constIt->is_object())
        return Fail(error, "the tree data has no nodes / groups / constants (not a Path of Building tree.json)");

    PassiveTree t;
    if (const auto m = d.find("bp_meta"); m != d.end() && m->is_object()) {
        t.source = Str(*m, "source", kMaxMetaBytes);
        t.treeVersion = Str(*m, "treeVersion", kMaxIdBytes);
        t.commit = Str(*m, "commit", kMaxIdBytes);
    }
    if (t.treeVersion.empty()) t.treeVersion = Capped(std::string(versionHint), kMaxIdBytes);

    // constants
    for (const char* key : {"orbitRadii", "skillsPerOrbit"}) {
        const auto it = constIt->find(key);
        if (it == constIt->end() || !it->is_array() || it->empty() || it->size() > kMaxOrbits)
            return Fail(error, std::string("constants.") + key + " is missing or malformed");
        std::vector<int>& dst = (key[0] == 'o') ? t.orbitRadii : t.skillsPerOrbit;
        for (const json& v : *it) {
            double x = 0;
            if (!Num(v, x) || x < 0 || x > 1.0e6) return Fail(error, std::string("constants.") + key + " holds a bad value");
            dst.push_back(static_cast<int>(x));
        }
    }
    if (t.orbitRadii.size() != t.skillsPerOrbit.size())
        return Fail(error, "constants.orbitRadii and constants.skillsPerOrbit differ in length");
    std::vector<std::vector<double>> anglesByOrbit;
    if (const auto a = constIt->find("orbitAnglesByOrbit"); a != constIt->end() && a->is_array() && a->size() <= kMaxOrbits) {
        for (const json& row : *a) {
            std::vector<double> angles;
            if (row.is_array() && row.size() <= kMaxOrbitSlots)
                for (const json& v : row) {
                    double x = 0;
                    angles.push_back(Num(v, x) ? x : 0.0);
                }
            anglesByOrbit.push_back(std::move(angles));
        }
    }

    // classes
    if (const auto c = d.find("classes"); c != d.end() && c->is_array()) {
        for (const json& cl : *c) {
            if (t.classes.size() >= kMaxTreeClasses) break;
            if (!cl.is_object()) continue;
            ClassInfo ci;
            ci.name = Str(cl, "name", kMaxTextBytes);
            int64_t id = -1;
            if (Int(cl, "integerId", id) && id >= 0 && id < 1000) ci.integerId = static_cast<int>(id);
            if (const auto a = cl.find("ascendancies"); a != cl.end() && a->is_array())
                for (const json& asc : *a) {
                    if (ci.ascendancies.size() >= kMaxClassAscendancies) break;
                    if (!asc.is_object()) continue;
                    AscendancyInfo ai;
                    ai.name = Str(asc, "name", kMaxTextBytes);
                    if (ai.name.empty()) ai.name = Str(asc, "id", kMaxTextBytes);
                    ai.internalId = Str(asc, "internalId", kMaxIdBytes);
                    ci.ascendancies.push_back(std::move(ai));
                }
            if (!ci.name.empty()) t.classes.push_back(std::move(ci));
        }
    }

    // groups: key -> index into t.groups
    std::unordered_map<int64_t, uint32_t> groupIndex;
    if (groupsIt->size() > kMaxTreeGroups) return Fail(error, "the tree data holds too many groups");
    auto readGroup = [&](const json& g, int64_t key) {
        Group grp;
        double x = 0, y = 0;
        const bool ok = g.is_object() && g.contains("x") && g.contains("y") && Num(g["x"], x) && Num(g["y"], y) &&
                        std::fabs(x) < 1.0e7 && std::fabs(y) < 1.0e7;
        if (ok) {
            grp.x = static_cast<float>(x);
            grp.y = static_cast<float>(y);
            groupIndex.emplace(key, static_cast<uint32_t>(t.groups.size()));
        }
        t.groups.push_back(std::move(grp));
    };
    if (groupsIt->is_array()) {
        int64_t key = 1;   // PoB: node.group is 1-based into this list
        for (const json& g : *groupsIt) readGroup(g, key++);
    } else {
        for (auto it = groupsIt->begin(); it != groupsIt->end(); ++it) {
            if (!IsDigits(it.key(), 9)) continue;
            readGroup(it.value(), std::strtoll(it.key().c_str(), nullptr, 10));
        }
    }

    // nodes
    if (nodesIt->size() > kMaxTreeNodes) return Fail(error, "the tree data holds too many nodes");
    std::vector<RawNode> raw;
    raw.reserve(nodesIt->size());
    for (auto it = nodesIt->begin(); it != nodesIt->end(); ++it) {
        if (!it.value().is_object()) continue;
        const int64_t keyId = IsDigits(it.key(), 5) ? std::strtoll(it.key().c_str(), nullptr, 10) : -1;
        RawNode r;
        if (ReadNode(it.value(), keyId, r)) raw.push_back(std::move(r));
    }
    std::stable_sort(raw.begin(), raw.end(), [](const RawNode& a, const RawNode& b) { return a.node.id < b.node.id; });
    raw.erase(std::unique(raw.begin(), raw.end(), [](const RawNode& a, const RawNode& b) { return a.node.id == b.node.id; }),
              raw.end());
    if (raw.size() < kMinTreeNodes)
        return Fail(error, "the tree data holds " + std::to_string(raw.size()) + " nodes (at least " +
                               std::to_string(kMinTreeNodes) + " expected)");
    if (t.classes.size() < kMinTreeClasses)
        return Fail(error, "the tree data holds " + std::to_string(t.classes.size()) + " classes (at least " +
                               std::to_string(kMinTreeClasses) + " expected)");

    t.nodes.reserve(raw.size());
    for (RawNode& r : raw) {
        const auto g = groupIndex.find(r.groupKey);
        if (g == groupIndex.end())
            return Fail(error, "passive " + std::to_string(r.node.id) + " names an unknown group " + std::to_string(r.groupKey));
        Node& n = r.node;
        n.group = g->second;
        if (n.orbit >= 0 && static_cast<size_t>(n.orbit) < t.orbitRadii.size() && n.orbitIndex >= 0) {
            const size_t o = static_cast<size_t>(n.orbit);
            const size_t k = static_cast<size_t>(n.orbitIndex);
            double angle = 0.0;
            if (o < anglesByOrbit.size() && k < anglesByOrbit[o].size()) angle = anglesByOrbit[o][k];
            else if (t.skillsPerOrbit[o] > 0) angle = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(t.skillsPerOrbit[o]);
            const double radius = static_cast<double>(t.orbitRadii[o]);
            const Group& grp = t.groups[n.group];
            n.angle = static_cast<float>(angle);
            n.orbitRadius = static_cast<float>(radius);
            n.x = static_cast<float>(grp.x + std::sin(angle) * radius);
            n.y = static_cast<float>(grp.y - std::cos(angle) * radius);
        } else {
            n.x = t.groups[n.group].x;
            n.y = t.groups[n.group].y;
        }
        t.indexById.emplace(n.id, static_cast<uint32_t>(t.nodes.size()));
        t.nodes.push_back(n);   // copied: `raw` keeps the connections for the link pass
    }
    const size_t count = t.nodes.size();

    // links, both directions; drawn lines with their orbit code
    std::vector<std::vector<std::pair<uint32_t, int8_t>>> drawn(count);
    auto addDrawn = [&drawn](uint32_t from, uint32_t to, int8_t code) {
        for (const auto& e : drawn[from])
            if (e.first == to) return;   // the first entry of a pair wins
        drawn[from].emplace_back(to, code);
    };
    for (uint32_t a = 0; a < count; ++a) {
        for (const auto& [id, orbit] : raw[a].connections) {
            const auto bi = t.indexById.find(static_cast<uint16_t>(id));
            if (bi == t.indexById.end() || bi->second == a) continue;
            const uint32_t b = bi->second;
            Node& na = t.nodes[a];
            Node& nb = t.nodes[b];
            if (na.Is(kNodeOnlyImage) || nb.Is(kNodeOnlyImage)) continue;
            na.links.push_back(b);
            nb.links.push_back(a);
            if (na.ascendancy != nb.ascendancy || na.Is(kNodeClassStart) || nb.Is(kNodeClassStart)) continue;
            addDrawn(a, b, OrbitCode(orbit, t.orbitRadii));
            addDrawn(b, a, OrbitCode(orbit == INT64_MIN ? INT64_MAX : -orbit, t.orbitRadii));
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        Node& n = t.nodes[i];
        std::sort(n.links.begin(), n.links.end());
        n.links.erase(std::unique(n.links.begin(), n.links.end()), n.links.end());
        std::sort(drawn[i].begin(), drawn[i].end(),
                  [](const std::pair<uint32_t, int8_t>& x, const std::pair<uint32_t, int8_t>& y) { return x.first < y.first; });
        n.drawnLinks.reserve(drawn[i].size());
        n.drawnOrbit.reserve(drawn[i].size());
        for (const auto& [to, code] : drawn[i]) {
            n.drawnLinks.push_back(to);
            n.drawnOrbit.push_back(code);
        }
        for (const int64_t id : raw[i].unlockIds) {
            const auto u = t.indexById.find(static_cast<uint16_t>(id));
            if (u != t.indexById.end() && u->second != i) n.unlockNodes.push_back(u->second);
        }
        std::sort(n.unlockNodes.begin(), n.unlockNodes.end());
        n.unlockNodes.erase(std::unique(n.unlockNodes.begin(), n.unlockNodes.end()), n.unlockNodes.end());
        t.groups[n.group].nodes.push_back(i);
    }
    for (uint32_t i = 0; i < count; ++i) {
        Node& n = t.nodes[i];
        if (n.Is(kNodeMultipleChoiceOption))
            for (const uint32_t l : n.links)
                if (t.nodes[l].Is(kNodeMultipleChoice)) {
                    n.choiceParent = l;
                    break;
                }
        for (const NodeVariant& v : n.variants)
            if (v.id != 0 && !t.indexById.count(v.id)) t.aliasOf.emplace(v.id, i);
        if (n.Is(kNodeAttribute))
            for (const AttrOption& op : n.options)
                if (op.id != 0 && op.attr != Attr::None) t.attrOfOption.emplace(op.id, op.attr);
    }

    // which Node::ascendancy value an ascendancy's nodes carry
    for (ClassInfo& c : t.classes)
        for (AscendancyInfo& a : c.ascendancies) {
            a.nodesName = a.name;
            bool own = false;
            for (const Node& n : t.nodes)
                if (n.ascendancy == a.name) {
                    own = true;
                    break;
                }
            if (own || a.name.empty()) continue;
            for (const Node& n : t.nodes) {
                if (n.ascendancy.empty()) continue;
                const bool keyed = std::any_of(n.variants.begin(), n.variants.end(),
                                               [&](const NodeVariant& v) { return v.key == a.name; });
                if (keyed) {
                    a.nodesName = n.ascendancy;
                    break;
                }
            }
        }

    if (const auto j = d.find("jewelSlots"); j != d.end() && j->is_array())
        for (const json& v : *j) {
            if (t.jewelSlots.size() >= kMaxJewelSlots) break;
            int64_t id = -1;
            if (AsInt(v, id) && id >= 0 && id <= 0xFFFF) t.jewelSlots.push_back(static_cast<uint16_t>(id));
        }

    out = std::move(t);
    return true;
}

// "<digits>_<digits>[_<digits>...]" -> its parts; false when it is not a version.
bool VersionParts(std::string_view v, std::vector<long>& parts) {
    parts.clear();
    if (v.empty() || v.size() > 64) return false;
    size_t from = 0;
    while (true) {
        const size_t at = v.find('_', from);
        const std::string_view part = v.substr(from, at == std::string_view::npos ? std::string_view::npos : at - from);
        if (!IsDigits(part, 9)) return false;
        parts.push_back(std::strtol(std::string(part).c_str(), nullptr, 10));
        if (at == std::string_view::npos) break;
        from = at + 1;
    }
    return true;
}

bool IsVersionFolder(std::string_view v) {
    std::vector<long> parts;
    return VersionParts(v, parts) && parts.size() == 2;
}

} // namespace

uint32_t PassiveTree::IndexOf(uint16_t id) const {
    const auto it = indexById.find(id);
    if (it != indexById.end()) return it->second;
    const auto alias = aliasOf.find(id);
    return alias == aliasOf.end() ? kNoIndex : alias->second;
}

uint16_t PassiveTree::BaseId(uint16_t id) const {
    const uint32_t i = IndexOf(id);
    return (i == kNoIndex || i >= nodes.size()) ? id : nodes[i].id;
}

const Node* PassiveTree::Find(uint16_t id) const {
    const uint32_t i = IndexOf(id);
    return (i == kNoIndex || i >= nodes.size()) ? nullptr : &nodes[i];
}

const ClassInfo* PassiveTree::ClassById(int integerId) const {
    if (integerId < 0) return nullptr;
    for (const ClassInfo& c : classes)
        if (c.integerId == integerId) return &c;
    return nullptr;
}

const ClassInfo* PassiveTree::ClassByName(std::string_view name) const {
    if (name.empty()) return nullptr;
    for (const ClassInfo& c : classes)
        if (EqualsNoCase(c.name, name)) return &c;
    return nullptr;
}

const AscendancyInfo* PassiveTree::AscendancyById(std::string_view internalId, const ClassInfo** owner) const {
    if (owner) *owner = nullptr;
    if (internalId.empty()) return nullptr;
    for (const ClassInfo& c : classes)
        for (const AscendancyInfo& a : c.ascendancies)
            if (!a.internalId.empty() && EqualsNoCase(a.internalId, internalId)) {
                if (owner) *owner = &c;
                return &a;
            }
    return nullptr;
}

const AscendancyInfo* PassiveTree::AscendancyByNodesName(std::string_view nodesName, const ClassInfo** owner) const {
    if (owner) *owner = nullptr;
    if (nodesName.empty()) return nullptr;
    for (const ClassInfo& c : classes)
        for (const AscendancyInfo& a : c.ascendancies)
            if (a.nodesName == nodesName) {
                if (owner) *owner = &c;
                return &a;
            }
    return nullptr;
}

uint32_t PassiveTree::ClassStartNode(std::string_view className) const {
    if (className.empty()) return kNoIndex;
    for (uint32_t i = 0; i < nodes.size(); ++i) {
        if (!nodes[i].Is(kNodeClassStart)) continue;
        for (const std::string& c : nodes[i].classesStart)
            if (EqualsNoCase(c, className)) return i;
    }
    return kNoIndex;
}

uint32_t PassiveTree::AscendancyStartNode(std::string_view nodesName) const {
    if (nodesName.empty()) return kNoIndex;
    for (uint32_t i = 0; i < nodes.size(); ++i)
        if (nodes[i].Is(kNodeAscStart) && nodes[i].ascendancy == nodesName) return i;
    return kNoIndex;
}

Attr PassiveTree::AttrOfOption(uint16_t optionId) const {
    if (optionId == 0) return Attr::None;
    if (!attrOfOption.empty()) {
        const auto it = attrOfOption.find(optionId);
        return it == attrOfOption.end() ? Attr::None : it->second;
    }
    for (const Node& n : nodes)   // a tree built by hand (tests): no map
        if (n.Is(kNodeAttribute))
            for (const AttrOption& op : n.options)
                if (op.id == optionId) return op.attr;
    return Attr::None;
}

std::vector<uint16_t> PassiveTree::StartNodeIds() const {
    std::vector<uint16_t> ids;
    for (const Node& n : nodes)
        if (n.Is(kNodeClassStart | kNodeAscStart)) ids.push_back(n.id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

int PassiveTree::AllocatableCount(std::string_view nodesName) const {
    int count = 0;
    for (const Node& n : nodes) {
        if (!n.Allocatable()) continue;
        if (!n.IsAscendancy() || (!nodesName.empty() && n.ascendancy == nodesName)) ++count;
    }
    return count;
}

bool ParseTree(std::string_view json, PassiveTree& out, std::string* error, std::string_view versionHint) {
    out = PassiveTree{};
    try {
        return ParseTreeImpl(json, out, error, versionHint);
    } catch (const std::exception& e) {
        out = PassiveTree{};
        return Fail(error, std::string("the tree data could not be read: ") + e.what());
    } catch (...) {
        out = PassiveTree{};
        return Fail(error, "the tree data could not be read");
    }
}

bool LoadTreeFile(const std::filesystem::path& path, PassiveTree& out, std::string* error, std::string_view versionHint) {
    out = PassiveTree{};
    try {
        const FileRead f = ReadFileCapped(path, kMaxTreeFileBytes);
        switch (f.status) {
            case FileStatus::Ok: break;
            case FileStatus::Missing: return Fail(error, "the tree data file does not exist: " + PathUtf8(path));
            case FileStatus::TooLarge: return Fail(error, "the tree data file is larger than 64 MiB: " + PathUtf8(path));
            default: return Fail(error, "the tree data file could not be read (" + f.error + "): " + PathUtf8(path));
        }
        return ParseTree(f.bytes, out, error, versionHint);
    } catch (...) {
        out = PassiveTree{};
        return Fail(error, "the tree data file could not be read");
    }
}

std::string TreeListingUrl() {
    return "https://api.github.com/repos/PathOfBuildingCommunity/PathOfBuilding-PoE2/contents/src/TreeData";
}

std::string NewestTreeVersion(std::string_view listingJson) {
    try {
        const json d = json::parse(listingJson.begin(), listingJson.end(), nullptr, false);
        if (d.is_discarded() || !d.is_array()) return {};
        std::string best;
        for (const json& e : d) {
            if (!e.is_object()) continue;
            const std::string type = Str(e, "type", 16);
            if (!type.empty() && type != "dir") continue;
            const std::string name = Str(e, "name", kMaxIdBytes);
            if (!IsVersionFolder(name)) continue;
            if (best.empty() || CompareTreeVersions(name, best) > 0) best = name;
        }
        return best;
    } catch (...) {
        return {};
    }
}

std::string TreeFileUrl(std::string_view version) {
    if (!IsVersionFolder(version)) return {};
    return "https://raw.githubusercontent.com/PathOfBuildingCommunity/PathOfBuilding-PoE2/dev/src/TreeData/" +
           std::string(version) + "/tree.json";
}

int CompareTreeVersions(std::string_view a, std::string_view b) {
    std::vector<long> pa, pb;
    bool okA = false, okB = false;
    try {
        okA = VersionParts(a, pa);
        okB = VersionParts(b, pb);
    } catch (...) {
        return 0;
    }
    if (!okA || !okB) return okA == okB ? 0 : (okA ? 1 : -1);
    const size_t n = std::max(pa.size(), pb.size());
    for (size_t i = 0; i < n; ++i) {
        const long x = i < pa.size() ? pa[i] : 0, y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

} // namespace bp
