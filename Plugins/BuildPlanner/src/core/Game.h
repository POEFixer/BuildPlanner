#pragma once
// BuildPlanner - the game seam
// Game.h - IGame, the one seam between the plugin's readers and the game.
//
// The readers (TreeReader, CharReader, GearReader, SkillsReader) never include the plugin SDK: they
// read through IGame. Production: host/HostGame (SDK calls). Tests and demo: core/FixtureGame over a .mem
// capture. Everything the HOST decodes (UI element fields, screen rectangles, inventory listings,
// item names) comes through the typed calls; Read() is only for the private offsets of Offsets.h.
//
// Threads: an IGame is called from the render thread AND the watcher thread at the same time. An
// implementation must be safe for that (HostGame is stateless; the SDK allows calls from plugin
// threads). Every call may fail at any time (the game closes, an area loads): a failed call returns
// the documented "nothing" value and never throws.
//
// Addresses are game-process addresses; 0 is never valid. Coordinates are game client pixels.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Types.h"

namespace bp {

// A UI element as the host decodes it (the host owns the UiElementBase offsets; the plugin never
// hard-codes them).
struct UiElem {
    uint64_t parent = 0;          // 0 = none / unreadable
    int childCount = 0;           // as the game reports it (the host caps it at 4096)
    float relX = 0, relY = 0;     // RelativePosition, unscaled units of the element's own space
    float modX = 0, modY = 0;     // PositionModifier: added to the position of every child that has modPos
    float w = 0, h = 0;           // UnscaledSize
    float scaleMul = 1.0f;        // LocalScaleMultiplier
    uint32_t flags = 0;           // raw flags (diagnostics; use `visible` / `modPos`, not bit numbers)
    uint16_t type = 0;
    uint8_t scaleIndex = 0;
    bool visible = false;         // the element's OWN visible bit (parents are not consulted)
    bool modPos = false;          // "should modify position": the parent's PositionModifier applies to it
    bool valid = false;           // false = the element could not be read; every other field is then unset
};

// What the watcher needs to know about the game as a whole. One cheap call.
struct GameStatus {
    bool attached = false;        // the host is attached to a game process
    bool inGame = false;          // a character is in an area (not login / character select / loading)
    bool treeVisible = false;     // the host's own "passive tree is open" flag (false with a controller); with
                                  // HostGame it is refreshed at most every 100 ms: per tick, readers use the panel's own visible bit
    uint64_t areaSerial = 0;      // moves on every area load (the SDK's area change counter)
    float clientW = 0, clientH = 0;   // game client size in pixels, 0 when unknown
};

// The player as the host reads it.
struct PlayerInfo {
    bool valid = false;
    int level = 0;                // 1..100
    std::string name;             // UTF-8
    std::string path;             // the player entity's metadata path ("Metadata/Characters/Int/IntFourb")
};

// One inventory of the character, without its items.
struct InvInfo {
    bool valid = false;           // false = the host does not publish this id
    int hostId = 0;
    uint64_t address = 0;         // the InventoryStruct (what UI slot elements point at)
    int boxesX = 0, boxesY = 0;
    // The on-screen grid of the last scan that saw the inventory's UI element (client px, square
    // cells). Wrong for the Flask1 inventory (docs/OFFSETS.md section 4.5): use the slot elements for
    // flasks / charms.
    bool gridValid = false;
    float gridX = 0, gridY = 0, cell = 0;
};

struct InvItem {
    uint64_t entity = 0;          // the item entity (pass it to ItemRead)
    int slotX = 0, slotY = 0, w = 0, h = 0;   // cells
    std::string path;             // BaseItemTypes Id ("Metadata/Items/Armours/BodyArmours/FourBodyStrDexInt1")
    std::string baseName;         // localized base type name (game language), UTF-8
    std::string uniqueName;       // localized name words: a unique's name AND a rare's two-word name
};

// The items of one inventory as of its last completed scan.
struct InvSnapshot {
    bool supported = false;       // false = the host is too old for scan stamps
    bool ready = false;           // false = no usable scan yet; `items` is authoritative only when true
    uint64_t scanStamp = 0;       // identifies the scan (changes when a new scan of this id completes)
    uint64_t areaSerial = 0;      // GameStatus::areaSerial the scan belongs to
    InvInfo info;
    std::vector<InvItem> items;
};

// One item, by its entity address.
struct ItemInfo {
    bool valid = false;           // false = not an item entity / unreadable
    std::string path;             // BaseItemTypes Id; a gem's equals Path of Building's gemId byte for byte
    std::string baseName;         // localized
    std::string uniqueName;       // localized; also set for rares (gate "unique" on rarity)
    Rarity rarity = Rarity::Normal;   // never Relic
};

class IGame {
public:
    virtual ~IGame() = default;

    // ---- the game as a whole ----
    virtual GameStatus Status() = 0;
    virtual PlayerInfo Player() = 0;
    // The game module (for "is this pointer inside the image" checks). 0 when not attached.
    virtual uint64_t ModuleBase() = 0;
    virtual uint64_t ModuleSize() = 0;

    // ---- raw memory: only for the private offsets of Offsets.h ----
    // true only when all n bytes were read. n == 0 or addr == 0 -> false.
    virtual bool Read(uint64_t addr, void* buf, size_t n) = 0;

    // ---- UI, as the host decodes it ----
    virtual UiElem UiRead(uint64_t addr) = 0;
    // The element's children in child-index order; empty when unreadable or childless.
    virtual std::vector<uint64_t> UiChildren(uint64_t addr) = 0;
    // The element's rectangle in client pixels by the host's formula (ancestors, scale, cull).
    virtual Rect UiRect(uint64_t addr) = 0;
    // n rectangles in one host call (ancestors are read once per batch). out[i] answers addrs[i];
    // an unreadable element gives ok == false. n == 0 is a no-op.
    virtual void UiRects(const uint64_t* addrs, size_t n, Rect* out) = 0;
    // The locale-independent control name ("build_planner_indicator"); "" when it has none.
    virtual std::string UiControlName(uint64_t addr) = 0;
    // The element's LOCALIZED display text, UTF-8; "" when it has none. Never match game state on it.
    virtual std::string UiText(uint64_t addr) = 0;
    // Breadth-first search below `root` (root included) for elements with this control name,
    // shallowest first, at most maxResults. visibleOnly: the element and its ancestors up to root must
    // be visible. The host reads at most 4000 elements per call: keep maxDepth small and never call
    // it per frame. Empty when nothing matches or the host has no such call.
    virtual std::vector<uint64_t> UiFind(uint64_t root, const char* controlName, int maxDepth, bool visibleOnly,
                                         int maxResults) = 0;
    // The in-game panel root (UiRootStruct: the passive tree panel, the Skills panel and the character
    // data hang off it, Offsets.h). 0 when not in game.
    virtual uint64_t GameUiRoot() = 0;
    // The inventory panel (a child of the panel root; visible while the inventory is open). 0 when
    // not in game.
    virtual uint64_t InventoryPanelRoot() = 0;

    // ---- inventories and items ----
    // The inventory with this host id, without items. Cheap.
    virtual InvInfo InvGet(int hostId) = 0;
    // Asks the host to scan one inventory (items of equipment inventories are read only by such a
    // targeted scan). The host keeps ONE pending request and drops it while its own full scan is
    // pending: ask for one id, wait until InvRead(id).scanStamp moves, then ask for the next.
    virtual void InvScan(int hostId) = 0;
    virtual InvSnapshot InvRead(int hostId) = 0;
    // Names and rarity of an item entity (an inventory item, or the entity an item element or a gem
    // socket points at). Works without any inventory scan.
    virtual ItemInfo ItemRead(uint64_t entity) = 0;
    // The item's English name as the host's price data knows it (a unique's name, else the base),
    // "" when the host cannot tell (older host, unknown item). May be slow: watcher thread only.
    virtual std::string ItemEnglishName(uint64_t entity) = 0;

    // ---- helpers over Read (not virtual) ----
    template <class T>
    bool Get(uint64_t addr, T& out) {
        T tmp{};
        if (!Read(addr, &tmp, sizeof(T))) return false;
        out = tmp;
        return true;
    }
    // A pointer-sized value; 0 on failure.
    uint64_t Ptr(uint64_t addr) {
        uint64_t p = 0;
        return Get(addr, p) ? p : 0;
    }
};

// The discovery retry policy every reader shares: after a failure try again in 500 ms,
// in 2 s once ten attempts in a row failed; at once after Kick() (the panel opened, a build was
// imported, the area changed) and after a success. Never the POE1 60 s backoff.
struct RetryTimer {
    static constexpr int64_t kFastMs = 500, kSlowMs = 2000;
    static constexpr int kFastAttempts = 10;

    int failures = 0;            // consecutive failed attempts
    int64_t nextAtMs = 0;        // the next attempt is allowed at this time (0 = now)

    bool Due(int64_t nowMs) const { return nowMs >= nextAtMs; }
    void Failed(int64_t nowMs) {
        ++failures;
        nextAtMs = nowMs + (failures >= kFastAttempts ? kSlowMs : kFastMs);
    }
    void Succeeded() {
        failures = 0;
        nextAtMs = 0;
    }
    void Kick() { nextAtMs = 0; }   // keeps the failure count: a kick does not shorten later waits
};

// What the watcher hands every reader on each of its ticks.
struct WatchTick {
    int64_t nowMs = 0;           // a steady clock in milliseconds (tests pass their own)
    GameStatus status;           // read once per tick by the watcher
};

// A plausible user-mode address (never read through anything that fails this). Dat rows are not
// aligned (a PassiveSkills row is 487 bytes), so alignment is a separate test.
inline bool PlausiblePtr(uint64_t p) { return p >= 0x10000ull && p < 0x00007FFFFFFFFFFFull; }
// A plausible heap object or UI element: 8-byte aligned.
inline bool PlausibleObject(uint64_t p) { return PlausiblePtr(p) && (p & 0x7ull) == 0; }

} // namespace bp
