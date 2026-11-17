#pragma once
// BuildPlanner - IGame over the plugin SDK
// BP_TESTS_EXCLUDE: includes the plugin SDK; not built into tests/.
// HostGame.h - IGame (core/Game.h) over the plugin SDK: ctx()->Game / Entities / Components / Ui /
// Memory / Inventory / Prices. The production side of the game seam; tests and the demo mode use core/FixtureGame.
//
// Threads: the SDK allows calls from plugin threads, and the only state here besides the context
// pointer is two atomics (the cached tree flag): one HostGame is shared by the render thread and the
// watcher thread.
// Lifetime: the Context belongs to the plugin object and outlives the App that owns this HostGame.
//
// Cost notes (what each host call walks), for the callers' cadences:
//   Status()        four scalar host calls. The host's "passive tree is open" flag exists only in the
//                   full snapshot, whose SDK wrapper walks every entity of the area: it is refreshed
//                   at most every kTreeFlagPeriodMs and only in game, never per watcher tick.
//   Player()        one entity + one component read (no snapshot).
//   UiRect(s)       one element read per element and per ancestor; the batch reads ancestors once.
//   UiFind          up to 4000 element reads: discovery only.
//   InvGet          the SDK's Inventory.Get copies the item listing too (the wrapper offers no header-
//                   only call): discovery only, never per tick.
//   ItemRead        four small host calls; ItemEnglishName goes through the price data (watcher only).
#include <atomic>

#include "../../../../POEFixer/plugin_sdk/PluginSDK.h"

#include "../core/Game.h"

namespace bp {

class HostGame final : public IGame {
public:
    // `hostCompatible` false (an older host: the SDK services are inert): every call answers
    // "nothing" without touching the context.
    HostGame(const PluginSDK::Context* ctx, bool hostCompatible);

    GameStatus Status() override;
    PlayerInfo Player() override;
    uint64_t ModuleBase() override;
    uint64_t ModuleSize() override;
    bool Read(uint64_t addr, void* buf, size_t n) override;
    UiElem UiRead(uint64_t addr) override;
    std::vector<uint64_t> UiChildren(uint64_t addr) override;
    Rect UiRect(uint64_t addr) override;
    void UiRects(const uint64_t* addrs, size_t n, Rect* out) override;
    std::string UiControlName(uint64_t addr) override;
    std::string UiText(uint64_t addr) override;
    std::vector<uint64_t> UiFind(uint64_t root, const char* controlName, int maxDepth, bool visibleOnly,
                                 int maxResults) override;
    uint64_t GameUiRoot() override;
    uint64_t InventoryPanelRoot() override;
    InvInfo InvGet(int hostId) override;
    void InvScan(int hostId) override;
    InvSnapshot InvRead(int hostId) override;
    ItemInfo ItemRead(uint64_t entity) override;
    std::string ItemEnglishName(uint64_t entity) override;

    static constexpr int64_t kTreeFlagPeriodMs = 100;

private:
    const PluginSDK::Context* m_ctx = nullptr;
    bool m_ok = false;
    std::atomic<int64_t> m_treeFlagAtMs{0};    // steady clock of the last full snapshot, 0 = never
    std::atomic<bool> m_treeFlag{false};
};

} // namespace bp
