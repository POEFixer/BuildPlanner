// BuildPlanner - IGame over the plugin SDK
// BP_TESTS_EXCLUDE: includes the plugin SDK; not built into tests/.
// HostGame.cpp - IGame over the plugin SDK (see HostGame.h): plain forwarders. Every SDK call
// answers empty on failure (an older host, the game closing, an area loading), which is exactly the
// "nothing" value IGame documents; nothing here throws except through allocation.
#include "HostGame.h"

#include <chrono>
#include <cmath>

namespace bp {
namespace {

// Game paths are ASCII: anything else becomes '?'.
std::string Narrow(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (const wchar_t c : w) out.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
    return out;
}

Rarity RarityOf(int v) {
    switch (v) {
    case 1: return Rarity::Magic;
    case 2: return Rarity::Rare;
    case 3: return Rarity::Unique;
    default: return Rarity::Normal;
    }
}

InvInfo InfoOfInventory(const PluginSDK::Inventory& inv) {
    InvInfo i;
    i.valid = inv.Address != 0;
    i.hostId = inv.InventoryId;
    i.address = static_cast<uint64_t>(inv.Address);
    i.boxesX = inv.TotalBoxesX;
    i.boxesY = inv.TotalBoxesY;
    // The grid is the host's guess from the bound UI element; every targeted scan of another
    // inventory zeroes it and it is wrong for the Flask1 inventory (docs/OFFSETS.md section 4.5):
    // readers use the slot elements instead.
    i.gridValid = inv.Grid.Valid && std::isfinite(inv.Grid.GridScreenX) && std::isfinite(inv.Grid.GridScreenY) &&
                  std::isfinite(inv.Grid.CellSize) && inv.Grid.CellSize > 1.0f;
    if (i.gridValid) {
        i.gridX = inv.Grid.GridScreenX;
        i.gridY = inv.Grid.GridScreenY;
        i.cell = inv.Grid.CellSize;
    }
    return i;
}

// A rectangle the overlays may draw at: finite and not negative in size.
bool FiniteRect(const Rect& r) {
    return std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.w) && std::isfinite(r.h) && r.w >= 0.0f && r.h >= 0.0f;
}

} // namespace

HostGame::HostGame(const PluginSDK::Context* ctx, bool hostCompatible) : m_ctx(ctx), m_ok(ctx != nullptr && hostCompatible) {}

GameStatus HostGame::Status() {
    GameStatus s;
    if (!m_ok) return s;
    s.attached = m_ctx->Game.IsAttached();
    s.inGame = s.attached && m_ctx->Game.IsInGame();
    if (!s.attached) return s;
    const PluginSDK::ScreenSize size = m_ctx->Game.GetScreenSize();
    if (std::isfinite(size.Width) && std::isfinite(size.Height) && size.Width > 0.0f && size.Height > 0.0f) {
        s.clientW = size.Width;
        s.clientH = size.Height;
    }
    s.areaSerial = m_ctx->Game.GetAreaChangeCounter();   // the raw snapshot: scalars only
    if (!s.inGame) {
        m_treeFlag.store(false, std::memory_order_relaxed);
        m_treeFlagAtMs.store(0, std::memory_order_relaxed);
        return s;
    }
    // The host's tree flag is only in the full snapshot, and the SDK's GetSnapshot walks every entity
    // of the area whatever the filter says: at 60 ticks a second that would be the plugin's largest
    // cost in a crowded map. The flag is a hint for the tree reader (it reads the panel's own visible
    // bit), so a tenth of a second of age is fine.
    const int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const int64_t at = m_treeFlagAtMs.load(std::memory_order_relaxed);
    if (at == 0 || now - at >= kTreeFlagPeriodMs) {
        const PluginSDK::Snapshot snap = m_ctx->Game.GetSnapshot(PluginSDK::EntityType::Player);
        m_treeFlag.store(snap.IsSkillTreeVisible, std::memory_order_relaxed);
        m_treeFlagAtMs.store(now, std::memory_order_relaxed);
    }
    s.treeVisible = m_treeFlag.load(std::memory_order_relaxed);
    return s;
}

PlayerInfo HostGame::Player() {
    PlayerInfo p;
    if (!m_ok) return p;
    const PluginSDK::Entity player = m_ctx->Entities.GetPlayer();
    if (!player.IsValid || !player.Components.HasPlayer()) return p;
    const PluginSDK::Player info = m_ctx->Components.ReadPlayer(player.Components.Player);
    if (!info.Valid || info.Level < 1 || info.Level > 100) return p;
    p.valid = true;
    p.level = info.Level;
    p.name = info.Name;
    p.path = Narrow(player.Path);
    return p;
}

uint64_t HostGame::ModuleBase() { return m_ok ? static_cast<uint64_t>(m_ctx->Memory.GetBaseAddress()) : 0; }
uint64_t HostGame::ModuleSize() { return m_ok ? static_cast<uint64_t>(m_ctx->Memory.GetModuleSize()) : 0; }

bool HostGame::Read(uint64_t addr, void* buf, size_t n) {
    if (!m_ok || addr == 0 || n == 0 || buf == nullptr) return false;
    return m_ctx->Memory.Read(static_cast<uintptr_t>(addr), buf, n);
}

UiElem HostGame::UiRead(uint64_t addr) {
    UiElem e;
    if (!m_ok || addr == 0) return e;
    const PluginSDK::UiElement u = m_ctx->Ui.Read(static_cast<uintptr_t>(addr));
    if (!u.Valid) return e;
    e.parent = static_cast<uint64_t>(u.ParentAddress);
    e.childCount = u.ChildCount > 0 ? u.ChildCount : 0;
    e.relX = u.RelativeX;
    e.relY = u.RelativeY;
    e.modX = u.PositionModX;
    e.modY = u.PositionModY;
    e.w = u.UnscaledWidth;
    e.h = u.UnscaledHeight;
    e.scaleMul = u.LocalScaleMultiplier;
    e.flags = u.Flags;
    e.type = u.ElementType;
    e.scaleIndex = u.ScaleIndex;
    e.visible = u.IsVisible;
    e.modPos = u.HasPositionModifier;
    e.valid = true;
    return e;
}

std::vector<uint64_t> HostGame::UiChildren(uint64_t addr) {
    std::vector<uint64_t> out;
    if (!m_ok || addr == 0) return out;
    const std::vector<uintptr_t> kids = m_ctx->Ui.GetChildren(static_cast<uintptr_t>(addr));
    out.reserve(kids.size());
    for (const uintptr_t k : kids) out.push_back(static_cast<uint64_t>(k));
    return out;
}

Rect HostGame::UiRect(uint64_t addr) {
    Rect r;
    if (!m_ok || addr == 0) return r;
    if (!m_ctx->Ui.ComputeScreenRect(static_cast<uintptr_t>(addr), r.x, r.y, r.w, r.h)) return Rect{};
    r.ok = FiniteRect(r);
    return r;
}

void HostGame::UiRects(const uint64_t* addrs, size_t n, Rect* out) {
    if (!addrs || !out || n == 0) return;
    for (size_t i = 0; i < n; ++i) out[i] = Rect{};
    if (!m_ok) return;
    std::vector<uintptr_t> in(n);
    for (size_t i = 0; i < n; ++i) in[i] = static_cast<uintptr_t>(addrs[i]);
    const std::vector<PluginSDK::UiService::ScreenRect> rects = m_ctx->Ui.ComputeScreenRects(in);
    for (size_t i = 0; i < n && i < rects.size(); ++i) {
        if (!rects[i].ok || addrs[i] == 0) continue;
        out[i].x = rects[i].x;
        out[i].y = rects[i].y;
        out[i].w = rects[i].w;
        out[i].h = rects[i].h;
        out[i].ok = FiniteRect(out[i]);
    }
}

std::string HostGame::UiControlName(uint64_t addr) {
    if (!m_ok || addr == 0) return {};
    return m_ctx->Ui.GetControlName(static_cast<uintptr_t>(addr));
}

std::string HostGame::UiText(uint64_t addr) {
    if (!m_ok || addr == 0) return {};
    return m_ctx->Ui.GetText(static_cast<uintptr_t>(addr));
}

std::vector<uint64_t> HostGame::UiFind(uint64_t root, const char* controlName, int maxDepth, bool visibleOnly, int maxResults) {
    std::vector<uint64_t> out;
    // root 0 would mean "the whole game UI" to the host: IGame promises a search BELOW `root` only.
    if (!m_ok || root == 0 || !controlName || !controlName[0] || maxDepth < 0 || maxResults <= 0) return out;
    const std::vector<uintptr_t> found = m_ctx->Ui.FindElements(static_cast<uintptr_t>(root), controlName,
                                                                PluginSDK::UiMatchKind::ControlName, maxDepth, visibleOnly, maxResults);
    out.reserve(found.size());
    for (const uintptr_t f : found) out.push_back(static_cast<uint64_t>(f));
    return out;
}

uint64_t HostGame::GameUiRoot() { return m_ok ? static_cast<uint64_t>(m_ctx->Ui.GetGameUiRoot()) : 0; }
uint64_t HostGame::InventoryPanelRoot() { return m_ok ? static_cast<uint64_t>(m_ctx->Ui.GetUiRoot()) : 0; }

InvInfo HostGame::InvGet(int hostId) {
    if (!m_ok || hostId <= 0) return {};
    const InvInfo info = InfoOfInventory(m_ctx->Inventory.Get(hostId));
    // An id the host does not publish answers a zeroed struct (id 0, address 0).
    return info.hostId == hostId ? info : InvInfo{};
}

void HostGame::InvScan(int hostId) {
    if (m_ok && hostId > 0) m_ctx->Inventory.Scan(hostId);
}

InvSnapshot HostGame::InvRead(int hostId) {
    InvSnapshot s;
    if (!m_ok || hostId <= 0) return s;
    const PluginSDK::InventorySnapshot snap = m_ctx->Inventory.ReadSnapshot(hostId);
    s.supported = snap.Supported;
    s.ready = snap.Ready;
    s.scanStamp = snap.ScanStamp;
    s.areaSerial = snap.AreaChangeCounter;
    if (!snap.Ready) return s;
    s.info = InfoOfInventory(snap.Value);
    s.items.reserve(snap.Value.Items.size());
    for (const PluginSDK::InventoryItem& it : snap.Value.Items) {
        InvItem i;
        i.entity = static_cast<uint64_t>(it.Address);
        i.slotX = it.SlotX;
        i.slotY = it.SlotY;
        i.w = it.Width;
        i.h = it.Height;
        i.path = it.Path;
        i.baseName = it.BaseTypeName;
        i.uniqueName = it.UniqueName;
        s.items.push_back(std::move(i));
    }
    return s;
}

ItemInfo HostGame::ItemRead(uint64_t entity) {
    ItemInfo i;
    if (!m_ok || entity == 0) return i;
    const uintptr_t e = static_cast<uintptr_t>(entity);
    i.path = m_ctx->Inventory.ReadItemPath(e);
    // Every item's path starts with "Metadata/": anything else is not an item entity (or a torn read).
    if (i.path.rfind("Metadata/", 0) != 0) return ItemInfo{};
    i.baseName = m_ctx->Inventory.ReadItemBaseTypeName(e);
    i.uniqueName = m_ctx->Inventory.ReadItemUniqueName(e);
    i.rarity = RarityOf(m_ctx->Inventory.ReadItemRarity(e));
    i.valid = true;
    return i;
}

std::string HostGame::ItemEnglishName(uint64_t entity) {
    std::string name;
    if (!m_ok || entity == 0 || !m_ctx->Prices.CanLookupByItem()) return name;
    m_ctx->Prices.LookupPriceByItem(static_cast<uintptr_t>(entity), &name);
    return name;
}

} // namespace bp
