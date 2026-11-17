// BuildPlanner - equipment slots and equipped items (reader)
// GearReader.cpp - see GearReader.h. Reads the game READ-ONLY through IGame; every count, pointer and
// float that comes from the game is checked before it is used.
//
// Rules this file adds to the header's contract (tools/make_fixture.py record_gear mirrors the reads):
//   - Discovery: UiFind(panel, "build_planner_indicator", depth 3, hidden too); a slot element is an
//     indicator's parent; of the containers holding slot elements the one with the most wins (the
//     inventory panel's equipment container). A slot binds when [+uiInventoryPtr] is a plausible
//     object, [+uiSlotCell] fits [inventory + invBoxes], GearSlotFromDatRow names a GearSlot and the
//     host's InvGet(hostId).address, when published, is that inventory. The bindings live for the
//     area: any failed read fails the discovery (ReadFailed, retried), and two passes must agree.
//   - Items: per bound slot, the slot element is re-read (still readable, still bound to the same
//     inventory: else the slots are rediscovered at once), then the inventory's item covering the
//     slot's cell, ItemRead (a "Metadata/Items/" path), ItemEnglishName only when the item changed,
//     the item element (the child pointing at that entity) and its tooltip ([+itemElTooltip] whose
//     parent is the item element). A failed read keeps the last items and retries on the next tick;
//     three failed reads in a row drop the slots (ReadFailed) and discovery retries on its timer.
//   - The tooltip source (the game's UI manager) is looked up when the area changes and
//     checked again every 2 s; ReadGameTooltip / ReadTooltipOf (shared by the three readers) live here.
#include "GearReader.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bp {
namespace {

constexpr const char* kIndicatorName = "build_planner_indicator";
constexpr int kFindDepth = 3;
constexpr int kFindMaxResults = 64;
constexpr size_t kMaxSlotChildren = 16;
constexpr int64_t kItemsOpenMs = 250, kItemsClosedMs = 2000;
constexpr int kMaxItemFailures = 3;
constexpr float kMaxCoord = 1e6f;
// The game's tooltip.
constexpr int64_t kTooltipSourceMs = 2000;    // how often the watcher checks the UI manager again
constexpr size_t kMaxTooltipEntries = 4;      // more tooltips on one element: not a tooltip vector
constexpr uint32_t kMaxTooltipStride = 0x100;
constexpr uint32_t kMaxManagerSpan = 0x400;   // the manager's fields are read in one block up to this span
constexpr float kMaxTooltipUnits = 20000.0f;  // a tooltip's own unscaled size beyond this is not a size
constexpr float kMaxTooltipPx = 20000.0f;
constexpr float kMouseSlack = 1.0f;           // the game's mouse is integer; rectangles are not

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

bool UsableRect(const Rect& r) {
    return r.ok && std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h) && r.w > 0.0f &&
           r.h > 0.0f && std::fabs(r.x) < kMaxCoord && std::fabs(r.y) < kMaxCoord && r.w < kMaxCoord && r.h < kMaxCoord;
}

bool SameItem(const EquippedItem& a, const EquippedItem& b) {
    return a.present == b.present && a.entity == b.entity && a.path == b.path && a.baseName == b.baseName &&
           a.uniqueName == b.uniqueName && a.englishName == b.englishName && a.rarity == b.rarity;
}

bool SameSlot(const GearSlotState& a, const GearSlotState& b) {
    return a.known == b.known && a.element == b.element && a.visible == b.visible && a.itemElement == b.itemElement &&
           a.tooltipElement == b.tooltipElement && SameItem(a.item, b.item);
}

bool SameState(const GearState& a, const GearState& b) {
    if (a.problem != b.problem || a.found != b.found || a.panelOpen != b.panelOpen || a.panel != b.panel ||
        a.itemsKnown != b.itemsKnown || a.boundSlots != b.boundSlots || !(a.tooltipSource == b.tooltipSource))
        return false;
    for (size_t i = 0; i < kGearSlotCount; ++i)
        if (!SameSlot(a.slots[i], b.slots[i])) return false;
    return true;
}

bool SameItems(const GearState& a, const GearState& b) {
    if (a.itemsKnown != b.itemsKnown) return false;
    for (size_t i = 0; i < kGearSlotCount; ++i)
        if (!SameItem(a.slots[i].item, b.slots[i].item)) return false;
    return true;
}

enum class ItemsResult : uint8_t { Ok, BindingLost, ReadFailed };

bool SaneTooltipRect(const Rect& r) {
    return r.ok && std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h) && r.w >= 1.0f &&
           r.h >= 1.0f && r.w <= kMaxTooltipPx && r.h <= kMaxTooltipPx && std::fabs(r.x) <= kMaxTooltipPx &&
           std::fabs(r.y) <= kMaxTooltipPx;
}

bool SaneOwnSize(const UiElem& e) {
    return std::isfinite(e.w) && std::isfinite(e.h) && e.w > 0.0f && e.h > 0.0f && e.w <= kMaxTooltipUnits &&
           e.h <= kMaxTooltipUnits;
}

Rect Union(const Rect& a, const Rect& b) {
    if (!a.ok) return b;
    if (!b.ok) return a;
    Rect u;
    u.x = std::min(a.x, b.x);
    u.y = std::min(a.y, b.y);
    u.w = std::max(a.Right(), b.Right()) - u.x;
    u.h = std::max(a.Bottom(), b.Bottom()) - u.y;
    u.ok = true;
    return u;
}

// The manager's hovered element, tooltip owner and mouse: one read when the fields lie close
// together (they do on the reference client: 0x698 .. 0x768), else four.
// The span is computed in 64 bits: an offset near 0xFFFFFFFF (config/offsets.json) would wrap
// `offset + 8` in 32 bits to a small end, pass the span test and copy from ~4 GB past `block`.
bool ReadHover(IGame& g, const TooltipSource& s, GameHover& h) {
    h = GameHover{};
    const uint64_t lo = std::min(std::min(s.hovered, s.owner), std::min(s.mouseX, s.mouseY));
    const uint64_t hi = std::max(std::max(uint64_t{s.hovered} + 8, uint64_t{s.owner} + 8),
                                 std::max(uint64_t{s.mouseX} + 4, uint64_t{s.mouseY} + 4));
    uint64_t hovered = 0, owner = 0;
    int32_t mx = 0, my = 0;
    if (hi > lo && hi - lo <= kMaxManagerSpan) {
        uint8_t block[kMaxManagerSpan];
        if (!g.Read(s.manager + lo, block, static_cast<size_t>(hi - lo))) return false;
        std::memcpy(&hovered, block + (s.hovered - lo), 8);
        std::memcpy(&owner, block + (s.owner - lo), 8);
        std::memcpy(&mx, block + (s.mouseX - lo), 4);
        std::memcpy(&my, block + (s.mouseY - lo), 4);
    } else if (!g.Get(s.manager + s.hovered, hovered) || !g.Get(s.manager + s.owner, owner) || !g.Get(s.manager + s.mouseX, mx) ||
               !g.Get(s.manager + s.mouseY, my)) {
        return false;
    }
    h.ok = true;
    h.hovered = PlausibleObject(hovered) ? hovered : 0;
    h.owner = PlausibleObject(owner) ? owner : 0;
    h.mouse = {static_cast<float>(mx), static_cast<float>(my)};
    return true;
}

GameTooltip TooltipOfImpl(IGame& g, const TooltipSource& s, uint64_t owner, Vec2 mouse) {
    GameTooltip t;
    if (s.manager == 0 || !PlausibleObject(owner) || s.stride < 8 || s.stride > kMaxTooltipStride) return t;
    uint64_t v[2] = {};
    if (!g.Read(owner + s.tooltips, v, sizeof(v))) return t;
    if (v[0] == 0 || v[1] <= v[0] || !PlausibleObject(v[0])) return t;   // no tooltip made for it
    const uint64_t bytes = v[1] - v[0];
    if (bytes % s.stride != 0 || bytes / s.stride > kMaxTooltipEntries) return t;
    uint8_t entries[kMaxTooltipEntries * kMaxTooltipStride];
    if (!g.Read(v[0], entries, static_cast<size_t>(bytes))) return t;
    for (uint64_t k = 0; k < bytes / s.stride; ++k) {
        uint64_t tip = 0;
        std::memcpy(&tip, entries + k * s.stride, 8);
        if (!PlausibleObject(tip)) continue;
        const UiElem te = g.UiRead(tip);
        if (!te.valid || te.parent != owner || !te.visible) continue;
        const UiElem oe = g.UiRead(owner);
        if (!oe.valid || !oe.visible) return t;   // a stale hover: the element is hidden now
        uint64_t addrs[3] = {owner, tip, 0};
        size_t n = 2;
        if (!SaneOwnSize(te)) {   // a passive's tooltip: its title and body give the box
            const std::vector<uint64_t> kids = g.UiChildren(tip);
            if (kids.empty() || !PlausibleObject(kids[0])) continue;
            addrs[1] = kids[0];
            if (kids.size() >= 2 && PlausibleObject(kids[1])) addrs[n++] = kids[1];
        }
        Rect r[3];
        g.UiRects(addrs, n, r);
        const Rect& o = r[0];
        if (!UsableRect(o) || mouse.x < o.x - kMouseSlack || mouse.y < o.y - kMouseSlack || mouse.x > o.Right() + kMouseSlack ||
            mouse.y > o.Bottom() + kMouseSlack)
            return t;   // the mouse is not on it: not shown
        Rect box;
        for (size_t i = 1; i < n; ++i)
            if (SaneTooltipRect(r[i])) box = Union(box, r[i]);
        if (!SaneTooltipRect(box)) continue;   // never laid out (size 0) or garbage
        t.ok = true;
        t.rect = box;
        t.owner = owner;
        t.element = tip;
        return t;
    }
    return t;
}

} // namespace

TooltipSource FindTooltipSource(IGame& g, const Offsets& o, uint64_t root) {
    TooltipSource s;
    try {
        if (!PlausibleObject(root) || o.uiTooltipStride < 8 || o.uiTooltipStride > kMaxTooltipStride) return s;
        const UiElem re = g.UiRead(root);
        if (!re.valid || !PlausibleObject(re.parent)) return s;
        const uint64_t mgr = g.Ptr(root + o.uiElemMgr);
        if (!PlausibleObject(mgr) || g.Ptr(mgr + o.mgrRoot) != re.parent) return s;
        s.manager = mgr;
        s.hovered = o.mgrHovered;
        s.owner = o.mgrTooltipOwner;
        s.mouseX = o.mgrMouseX;
        s.mouseY = o.mgrMouseY;
        s.tooltips = o.uiElemTooltips;
        s.stride = o.uiTooltipStride;
    } catch (...) {
        s = TooltipSource{};
    }
    return s;
}

GameTooltip ReadGameTooltip(IGame& g, const TooltipSource& s, GameHover* hover) {
    try {
        if (hover) *hover = GameHover{};
        if (s.manager == 0) return {};
        GameHover h;
        if (!ReadHover(g, s, h)) return {};
        if (hover) *hover = h;
        if (h.owner != 0) {
            const GameTooltip t = TooltipOfImpl(g, s, h.owner, h.mouse);
            if (t.ok) return t;
        }
        if (h.hovered != 0 && h.hovered != h.owner) return TooltipOfImpl(g, s, h.hovered, h.mouse);
    } catch (...) {
    }
    return {};
}

GameTooltip ReadTooltipOf(IGame& g, const TooltipSource& s, uint64_t owner, Vec2 mouse) {
    try {
        return TooltipOfImpl(g, s, owner, mouse);
    } catch (...) {
        return {};
    }
}

bool ReadInventoryItems(IGame& g, const Offsets& o, uint64_t inv, int boxesX, int boxesY, std::vector<InvCellItem>& out) {
    out.clear();
    if (!PlausibleObject(inv) || boxesX < 1 || boxesY < 1 || boxesX > kMaxInventorySide || boxesY > kMaxInventorySide)
        return false;
    uint64_t v[3] = {};
    if (!g.Read(inv + o.invItems, v, sizeof(v))) return false;
    if (v[0] == 0 && v[1] == 0) return true;   // never allocated: no items
    if (!PlausibleObject(v[0]) || v[1] < v[0] || v[2] < v[1] || (v[1] - v[0]) % 8 != 0) return false;
    const uint64_t n = (v[1] - v[0]) / 8;
    if (n > static_cast<uint64_t>(boxesX) * static_cast<uint64_t>(boxesY)) return false;
    if (n == 0) return true;
    std::vector<uint64_t> entries(static_cast<size_t>(n));
    if (!g.Read(v[0], entries.data(), entries.size() * 8)) return false;
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    for (const uint64_t p : entries) {
        if (p == 0) continue;   // an empty cell
        if (!PlausibleObject(p)) return false;
        uint64_t entity = 0;
        int32_t start[2] = {}, end[2] = {};
        if (!g.Get(p + o.invItemEntity, entity) || !g.Read(p + o.invItemSlotStart, start, sizeof(start)) ||
            !g.Read(p + o.invItemSlotEnd, end, sizeof(end)))
            return false;
        if (entity == 0) continue;   // a placeholder struct (seen in scratch inventories)
        if (!PlausibleObject(entity)) return false;
        if (start[0] < 0 || start[1] < 0 || end[0] <= start[0] || end[1] <= start[1] || end[0] > boxesX || end[1] > boxesY)
            return false;
        out.push_back({p, entity, start[0], start[1], end[0] - start[0], end[1] - start[1]});
    }
    std::sort(out.begin(), out.end(), [](const InvCellItem& a, const InvCellItem& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    return true;
}

// The binding of one GearSlot to its element and inventory cell.
struct SlotBind {
    uint64_t element = 0, inventory = 0;
    int cellX = 0, cellY = 0, boxesX = 0, boxesY = 0;
    bool operator==(const SlotBind&) const = default;
};

struct GearWork {
    GearState cur;                                  // what the next Publish compares (serials unset)
    std::array<SlotBind, kGearSlotCount> bind{};
    bool needDiscovery = true;
    bool haveArea = false;
    bool itemsDue = true;                           // read the items on the next tick whatever the period
    int itemFailures = 0;                           // consecutive failed item reads
    bool sourceDue = true;                          // look the tooltip source up on the next tick
    int64_t lastSourceMs = 0;
};

struct GearReaderImpl {
    static GearWork& Work(GearReader& r) {
        if (!r.m_work) r.m_work = std::make_shared<GearWork>();
        return *r.m_work;
    }

    // Forgets the slots (not the panel or its open bit).
    static void DropSlots(GearReader& r) {
        GearWork& w = Work(r);
        w.bind = {};
        w.itemFailures = 0;
        GearState& s = w.cur;
        s.found = false;
        s.itemsKnown = false;
        s.boundSlots = 0;
        s.slots = {};
        w.itemsDue = true;
    }

    static std::shared_ptr<const GearState> Publish(GearReader& r) {
        const GearState& s = Work(r).cur;
        const GearState* last = r.m_state.get();
        if (last && SameState(*last, s)) return r.m_state;
        auto n = std::make_shared<GearState>(s);
        n->serial = r.m_nextSerial++;
        n->itemsSerial = (last && SameItems(*last, s)) ? last->itemsSerial : r.m_nextSerial++;
        r.m_state = std::move(n);
        return r.m_state;
    }

    // One pass of the discovery; `torn` = a read failed.
    struct Discovery {
        std::array<SlotBind, kGearSlotCount> bind{};
        std::array<bool, kGearSlotCount> vis{};
        int bound = 0;
        bool torn = false;
        bool operator==(const Discovery&) const = default;
    };

    static void DiscoverOnce(GearReader& r, uint64_t panel, Discovery& d) {
        IGame& g = r.m_game;
        const Offsets& o = r.m_off;
        d = Discovery{};
        const std::vector<uint64_t> indicators = g.UiFind(panel, kIndicatorName, kFindDepth, false, kFindMaxResults);
        struct Cand {
            uint64_t slot = 0, container = 0;
            bool visible = false;
        };
        std::vector<Cand> cands;
        for (const uint64_t ind : indicators) {
            const UiElem ie = g.UiRead(ind);
            if (!ie.valid || !PlausibleObject(ie.parent)) {
                d.torn = true;
                continue;
            }
            if (std::any_of(cands.begin(), cands.end(), [&](const Cand& c) { return c.slot == ie.parent; })) continue;
            const UiElem se = g.UiRead(ie.parent);
            if (!se.valid) {
                d.torn = true;
                continue;
            }
            cands.push_back({ie.parent, se.parent, se.visible});
        }
        // The container holding the most slot elements.
        uint64_t container = 0;
        size_t best = 0;
        for (const Cand& c : cands) {
            const size_t n = static_cast<size_t>(
                std::count_if(cands.begin(), cands.end(), [&](const Cand& e) { return e.container == c.container; }));
            if (n > best) {
                best = n;
                container = c.container;
            }
        }
        for (const Cand& c : cands) {
            if (c.container != container) continue;
            uint64_t inv = 0;
            uint32_t cell[4] = {};
            uint32_t row = 0;
            int32_t boxes[2] = {};
            if (!g.Get(c.slot + o.uiInventoryPtr, inv)) {
                d.torn = true;
                continue;
            }
            if (!PlausibleObject(inv)) continue;   // not bound to an inventory
            if (!g.Read(c.slot + o.uiSlotCell, cell, sizeof(cell)) || !g.Get(inv + o.invDatRow, row) ||
                !g.Read(inv + o.invBoxes, boxes, sizeof(boxes))) {
                d.torn = true;
                continue;
            }
            if (boxes[0] < 1 || boxes[1] < 1 || boxes[0] > kMaxInventorySide || boxes[1] > kMaxInventorySide) continue;
            const uint32_t bx = static_cast<uint32_t>(boxes[0]), by = static_cast<uint32_t>(boxes[1]);
            if (cell[2] < 1 || cell[3] < 1 || cell[0] >= bx || cell[1] >= by || cell[2] > bx - cell[0] || cell[3] > by - cell[1])
                continue;
            if (row > 0xFFFFu) continue;
            const int gs = GearSlotFromDatRow(static_cast<int>(row), static_cast<int>(cell[0]), static_cast<int>(cell[1]),
                                              static_cast<int>(o.invRowRing3));
            if (gs == kNoGearSlot) continue;
            const int hostId = kGearSlots[static_cast<size_t>(gs)].hostId;
            if (hostId > 0) {
                const InvInfo info = g.InvGet(hostId);
                if (info.valid && info.address != 0 && info.address != inv) continue;   // the host binds another struct
            }
            const size_t i = static_cast<size_t>(gs);
            SlotBind& b = d.bind[i];
            if (b.element != 0 && (d.vis[i] || !c.visible)) continue;   // first claim, a visible one wins
            if (b.element == 0) ++d.bound;
            b = {c.slot, inv, static_cast<int>(cell[0]), static_cast<int>(cell[1]), boxes[0], boxes[1]};
            d.vis[i] = c.visible;
        }
    }

    // The bindings are kept until the area changes, so a torn read or garbage bytes must not cost a
    // slot for that long: any failed read fails the discovery, and two passes must agree.
    static GearProblem Discover(GearReader& r, uint64_t panel) {
        GearWork& w = Work(r);
        DropSlots(r);
        Discovery a, b;
        DiscoverOnce(r, panel, a);
        GearState& s = w.cur;
        s.boundSlots = a.bound;
        if (a.torn) return GearProblem::ReadFailed;
        if (a.bound < kMinGearSlots) return GearProblem::NoSlots;
        DiscoverOnce(r, panel, b);
        if (!(b == a)) return GearProblem::ReadFailed;
        w.bind = a.bind;
        for (size_t i = 0; i < kGearSlotCount; ++i) {
            GearSlotState& st = s.slots[i];
            st.known = a.bind[i].element != 0;
            st.element = a.bind[i].element;
            st.visible = st.known && a.vis[i];
        }
        s.found = true;
        s.problem = GearProblem::None;
        return GearProblem::None;
    }

    // [itemEl + itemElTooltip] when its parent is the item element; `known` (when non-zero and still
    // the pointer there) is kept without another element read.
    static uint64_t TooltipOf(GearReader& r, uint64_t itemEl, uint64_t known) {
        if (itemEl == 0) return 0;
        const uint64_t tt = r.m_game.Ptr(itemEl + r.m_off.itemElTooltip);
        if (known != 0 && tt == known) return known;
        if (!PlausibleObject(tt)) return 0;
        const UiElem te = r.m_game.UiRead(tt);
        return te.valid && te.parent == itemEl ? tt : 0;
    }

    static ItemsResult ReadItems(GearReader& r) {
        IGame& g = r.m_game;
        const Offsets& o = r.m_off;
        GearWork& w = Work(r);
        std::array<GearSlotState, kGearSlotCount> next = w.cur.slots;
        std::vector<std::pair<uint64_t, std::vector<InvCellItem>>> invs;   // read once per call
        for (size_t i = 0; i < kGearSlotCount; ++i) {
            const SlotBind& b = w.bind[i];
            if (b.element == 0) continue;
            GearSlotState& st = next[i];
            const UiElem se = g.UiRead(b.element);
            if (!se.valid || g.Ptr(b.element + o.uiInventoryPtr) != b.inventory) return ItemsResult::BindingLost;
            st.visible = se.visible;
            auto it = std::find_if(invs.begin(), invs.end(), [&](const auto& e) { return e.first == b.inventory; });
            if (it == invs.end()) {
                std::vector<InvCellItem> items;
                if (!ReadInventoryItems(g, o, b.inventory, b.boxesX, b.boxesY, items)) return ItemsResult::ReadFailed;
                invs.emplace_back(b.inventory, std::move(items));
                it = invs.end() - 1;
            }
            uint64_t entity = 0;
            for (const InvCellItem& item : it->second)
                if (b.cellX >= item.x && b.cellX < item.x + item.w && b.cellY >= item.y && b.cellY < item.y + item.h) {
                    entity = item.entity;
                    break;
                }
            if (entity == 0) {
                st.item = EquippedItem{};
                st.itemElement = 0;
                st.tooltipElement = 0;
                continue;
            }
            const ItemInfo info = g.ItemRead(entity);
            if (!info.valid || !StartsWith(info.path, "Metadata/Items/")) return ItemsResult::ReadFailed;
            EquippedItem item;
            item.present = true;
            item.entity = entity;
            item.path = info.path;
            item.baseName = info.baseName;
            item.uniqueName = info.uniqueName;
            item.rarity = info.rarity;
            const EquippedItem& was = st.item;
            const bool sameItem = was.present && was.entity == entity && was.path == item.path &&
                                  was.uniqueName == item.uniqueName && was.rarity == item.rarity;
            item.englishName = sameItem ? was.englishName : g.ItemEnglishName(entity);
            uint64_t itemEl = 0;
            const std::vector<uint64_t> kids = g.UiChildren(b.element);
            for (size_t k = 0; k < kids.size() && k < kMaxSlotChildren; ++k)
                if (PlausibleObject(kids[k]) && g.Ptr(kids[k] + o.itemElEntity) == entity) {
                    itemEl = kids[k];
                    break;
                }
            st.tooltipElement = TooltipOf(r, itemEl, itemEl == st.itemElement ? st.tooltipElement : 0);
            st.itemElement = itemEl;
            st.item = std::move(item);
        }
        w.cur.slots = next;
        w.cur.itemsKnown = true;
        return ItemsResult::Ok;
    }

    // While the panel is open: the tooltip of every occupied slot that has none yet.
    static void PollTooltips(GearReader& r) {
        for (GearSlotState& st : Work(r).cur.slots)
            if (st.known && st.itemElement != 0 && st.tooltipElement == 0) st.tooltipElement = TooltipOf(r, st.itemElement, 0);
    }

    static void Refresh(GearReader& r, int64_t now, uint64_t panel, bool force) {
        GearWork& w = Work(r);
        GearState& s = w.cur;
        for (int pass = 0; pass < 2; ++pass) {
            if (w.needDiscovery && r.m_retry.Due(now)) {
                const GearProblem p = Discover(r, panel);
                if (p != GearProblem::None) {   // Discover left the slots dropped and boundSlots for diagnostics
                    r.m_retry.Failed(now);
                    s.problem = p;
                    return;
                }
                r.m_retry.Succeeded();
                w.needDiscovery = false;
            }
            if (!s.found) return;
            const int64_t period = s.panelOpen ? kItemsOpenMs : kItemsClosedMs;
            if (!force && !w.itemsDue && now - r.m_lastItemsMs < period) {
                if (s.panelOpen) PollTooltips(r);
                return;
            }
            r.m_lastItemsMs = now;
            w.itemsDue = false;
            const ItemsResult res = ReadItems(r);
            if (res == ItemsResult::Ok) {
                w.itemFailures = 0;
                return;
            }
            if (res == ItemsResult::BindingLost) {
                // An element was replaced: rediscover at once (the retry timer still limits failures).
                DropSlots(r);
                w.needDiscovery = true;
                r.m_retry.Kick();
                continue;
            }
            if (++w.itemFailures < kMaxItemFailures) {
                w.itemsDue = true;   // a torn read while the player equips: try again next tick
                return;
            }
            DropSlots(r);
            s.problem = GearProblem::ReadFailed;
            w.needDiscovery = true;
            r.m_retry.Failed(now);
            return;
        }
    }
};

GearReader::GearReader(IGame& game, const Offsets& offsets) : m_game(game), m_off(offsets) {
    m_state = std::make_shared<const GearState>();
    m_work = std::make_shared<GearWork>();
}

void GearReader::Kick() {
    GearReaderImpl::Work(*this).needDiscovery = true;
    m_retry.Kick();
}

void GearReader::Reset() {
    m_state = std::make_shared<const GearState>();
    m_retry = RetryTimer{};
    m_areaSerial = 0;
    m_lastItemsMs = 0;
    m_wasOpen = false;
    m_work = std::make_shared<GearWork>();
}

std::shared_ptr<const GearState> GearReader::Tick(const WatchTick& t) {
    try {
        GearWork& w = GearReaderImpl::Work(*this);
        GearState& s = w.cur;
        if (!t.status.inGame) {
            GearReaderImpl::DropSlots(*this);
            s = GearState{};
            w.haveArea = false;
            w.needDiscovery = true;
            w.sourceDue = true;
            m_wasOpen = false;
            return GearReaderImpl::Publish(*this);
        }
        if (!w.haveArea || t.status.areaSerial != m_areaSerial) {
            m_areaSerial = t.status.areaSerial;
            w.haveArea = true;
            GearReaderImpl::DropSlots(*this);
            w.needDiscovery = true;
            w.sourceDue = true;
            m_retry.Kick();
        }
        if (w.sourceDue || t.nowMs - w.lastSourceMs >= kTooltipSourceMs || t.nowMs < w.lastSourceMs) {
            s.tooltipSource = FindTooltipSource(m_game, m_off, m_game.GameUiRoot());
            w.lastSourceMs = t.nowMs;
            w.sourceDue = false;
        }
        const uint64_t panel = m_game.InventoryPanelRoot();
        const UiElem pe = PlausibleObject(panel) ? m_game.UiRead(panel) : UiElem{};
        if (!pe.valid) {
            GearReaderImpl::DropSlots(*this);
            s.problem = GearProblem::NoPanel;
            s.panel = 0;
            s.panelOpen = false;
            w.needDiscovery = true;
            m_wasOpen = false;
            return GearReaderImpl::Publish(*this);
        }
        if (panel != s.panel) {
            GearReaderImpl::DropSlots(*this);
            s.panel = panel;
            w.needDiscovery = true;
            m_retry.Kick();
        }
        if (s.problem == GearProblem::NoPanel || s.problem == GearProblem::NotInGame) s.problem = GearProblem::None;
        const bool opened = pe.visible && !m_wasOpen;
        m_wasOpen = pe.visible;
        s.panelOpen = pe.visible;
        if (opened) m_retry.Kick();   // a failed discovery is retried at once when the panel opens
        GearReaderImpl::Refresh(*this, t.nowMs, panel, opened);
        return GearReaderImpl::Publish(*this);
    } catch (...) {
        return m_state;   // out of memory: keep the last state
    }
}

GearFrame ReadGearFrame(IGame& game, const GearState& state, Vec2 mouse, bool mouseValid) {
    GearFrame f;
    try {
        if (!state.found || !state.panelOpen) return f;
        // [0] the panel, then the visible slots.
        std::array<uint64_t, kGearSlotCount + 1> addrs{};
        std::array<size_t, kGearSlotCount + 1> slotOf{};
        size_t n = 0;
        if (PlausibleObject(state.panel)) addrs[n++] = state.panel;
        const size_t first = n;
        for (size_t i = 0; i < kGearSlotCount; ++i) {
            const GearSlotState& st = state.slots[i];
            if (!st.known || !st.visible || st.element == 0) continue;
            addrs[n] = st.element;
            slotOf[n] = i;
            ++n;
        }
        if (n > 0) {
            std::array<Rect, kGearSlotCount + 1> rects{};
            game.UiRects(addrs.data(), n, rects.data());
            if (first == 1 && UsableRect(rects[0])) f.panel = rects[0];
            float bestArea = 0.0f;
            for (size_t k = first; k < n; ++k) {
                if (!UsableRect(rects[k])) continue;
                const size_t i = slotOf[k];
                f.slotRect[i] = rects[k];
                f.ok = true;
                const float area = rects[k].w * rects[k].h;
                if (mouseValid && rects[k].Contains(mouse) && (f.hovered == kNoGearSlot || area < bestArea)) {
                    f.hovered = static_cast<int>(i);
                    bestArea = area;
                }
            }
        }
        if (f.hovered != kNoGearSlot) {
            const GearSlotState& h = state.slots[static_cast<size_t>(f.hovered)];
            f.hoveredHasItem = h.item.present;
            if (f.hoveredHasItem && h.tooltipElement != 0) {
                const Rect t = game.UiRect(h.tooltipElement);
                if (UsableRect(t)) f.gameTooltip = t;
            }
        }
        f.tooltip = ReadGameTooltip(game, state.tooltipSource);
    } catch (...) {
        f = GearFrame{};
    }
    return f;
}

} // namespace bp
