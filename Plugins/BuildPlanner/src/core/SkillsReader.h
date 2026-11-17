#pragma once
// BuildPlanner - the character's gems and the Skills panel (reader)
// SkillsReader.h - the character's skill gems with their socketed support gems (character data), and
// the blocks and sockets of the game's Skills panel (UI). The offsets it reads, and how to find them
// again after a patch: docs/OFFSETS.md sections 4.5 to 4.7. No SDK: everything goes through IGame.
//
// Two halves, two threads:
//   WATCHER thread   SkillsReader::Tick: the character's skills (also while the panel is closed) and,
//                    while the panel is open, its visible blocks. Publishes an immutable SkillsState.
//   RENDER thread    ReadSkillsFrame: one rect batch over the visible blocks' elements.
//
// Character data: the player inventories hang off the PSD ([+psdInventories], one
// record per inventory); an inventory is identified by [+invDatRow] (Offsets::invRowSkillSlots,
// invRowAscendancySkills, the equipment rows for item-granted skills, plus every row a Skills panel
// block named this session), never by its host id (host ids equal row + 1 only up to id 39). A
// gem is an item entity; its "Sockets" component holds six entity pointers ([+socketsEntities]) and
// the socket count ([+socketsCounts] vector size). An item without a "SkillGem" component grants
// skills through its "Mods" component ([+modsGrantedSkills]). Names come from IGame::ItemRead.
// A gem's identity is its entity path: byte-identical to Path of Building's gemId. The list is
// accepted only when every gem path starts with "Metadata/Items/Gem"; otherwise NoGems.
// Never search "all inventories": a scratch inventory holds copies of equipped items (docs/OFFSETS.md
// section 4.5).
//
// Panel (docs/OFFSETS.md section 4.7): the Skills panel is [GameUiRoot + skillsPanelSlot], validated
// structurally (children named close_button / label_title / client_area; client_area/[0]/[1] is the
// scroll view with 3 children; its [1]/[0] is the list of pooled blocks; a live block has exactly 7
// children: [0] header, [1] big socket, [2..6] support sockets). The panel is built lazily on its first
// open.
// Block <-> skill: the big socket's [+bigSocketRow] / [+bigSocketSlot] name the gem's inventory row
// and column; fallback: the header's name text against the gem's localized name.
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Game.h"
#include "GearReader.h"   // TooltipSource, GameTooltip
#include "Offsets.h"
#include "Types.h"

namespace bp {

enum class SkillsProblem : uint8_t {
    None = 0,
    NotInGame,
    NoPlayerData,    // the PSD or its inventories are unreadable: the character's gems are unknown
    NoGems,          // the gem inventories were not found by their Inventories.dat rows
    NoPanel,         // the Skills panel element does not validate (only the per-socket marks are lost)
    ReadFailed
};

inline constexpr int kMaxSupportSockets = 5;   // sockets the Skills panel shows per skill

struct CharGem {
    bool present = false;         // false = an empty socket / no gem (the other fields are unset)
    uint64_t entity = 0;
    std::string gemId;            // the entity path ("Metadata/Items/Gem/SupportGemMuster")
    std::string name;             // localized display name (game language)
    int level = 0;                // the gem's own level, 0 unknown
};

// One skill of the character with what is socketed into it.
struct CharSkill {
    CharGem gem;                  // always present
    int datRow = -1;              // Inventories.dat row of the inventory that holds it (62 SkillSlots1,
                                  // 95 AscendancySkills1, an equipment row for an item-granted skill)
    int slotX = 0;                // its column there
    bool itemGranted = false;     // granted by an equipped item (Mods grantedSkills), not a gem of the player
    int socketCount = 0;          // 0..6
    std::vector<CharGem> supports;   // one entry per socket (size == socketCount); present == false = empty
};

// One live block of the Skills panel.
struct SkillBlockUi {
    uint64_t element = 0;         // the block
    uint64_t header = 0;          // child [0]: the header row (name, level)
    uint64_t bigSocket = 0;       // child [1]
    std::array<uint64_t, kMaxSupportSockets> sockets{};   // children [2..6]; 0 = that socket is not shown
    int socketCount = 0;          // how many of `sockets` are shown
    std::array<bool, kMaxSupportSockets> filled{};        // socket k holds a gem (its child [2] is visible)
    int skill = -1;               // index into SkillsState::skills, -1 = not matched to character data
    // The cross-check against the matched skill's character data: the number of shown
    // sockets equals its socket count AND every `filled` flag equals "that socket holds a gem"
    // (CharSkill::supports[k].present). false also when skill == -1. A block that does not agree gets
    // NO socket marks (its card may still be drawn from the character data).
    bool socketsAgree = false;
    std::string name;             // the header's name text (localized; the fallback identity)
    uint64_t headerRow = 0;       // the header container's child [0]: the 30 px row (`header` itself spans the
                                  // whole block); SkillsFrame::Block::header is its rect. 0 = not found
};

// The character's skills and the panel as of one watcher tick. Immutable once published.
struct SkillsState {
    SkillsProblem problem = SkillsProblem::NotInGame;
    bool skillsKnown = false;               // `skills` is the character's current list
    std::vector<CharSkill> skills;          // gem inventories in row order, then item-granted skills

    bool panelFound = false;                // the panel element validates (it may be closed or not built yet)
    bool panelBuilt = false;                // the panel was opened once this session (its list exists)
    bool panelOpen = false;                 // visible now
    uint64_t panel = 0;
    uint64_t scrollView = 0;                // its rect is the clip rect of the list
    std::vector<SkillBlockUi> blocks;       // the live (visible) blocks, in no particular order; empty
                                            // unless panelOpen

    uint64_t serial = 0;                    // moves when anything above changes
    uint64_t skillsSerial = 0;              // moves only when `skills` changes (the App's match key)
    // ---- the game's tooltip ----
    TooltipSource tooltipSource;            // for ReadSkillsFrame's `tooltip` (found per area, checked every 2 s)
};

// WATCHER thread only. Holds references: `game` must outlive the reader.
class SkillsReader {
public:
    SkillsReader(IGame& game, const Offsets& offsets);

    void Kick();
    void Reset();

    // One watcher tick: the panel's visible bit every time; the character's skills when the area
    // changes, when a gem inventory's request counter moves and at most every 500 ms otherwise (2 s
    // while the panel is closed).
    // The live blocks while the panel is open: the list REUSES its pooled block elements while the
    // player scrolls, so which skill a block element shows can change at any moment. On EVERY tick
    // (16 ms) the cheap identity is read: the list's children and, per live block, the big socket's
    // row and slot (one 8-byte read each, about 15 reads). The full block read (sockets shown,
    // filled flags, header text) runs at once when that identity changed and at most every 250 ms
    // otherwise. A mark is therefore never drawn on a block that shows another skill for longer
    // than a tick. Returns the previous pointer when nothing changed; never null.
    std::shared_ptr<const SkillsState> Tick(const WatchTick& t);

private:
    IGame& m_game;
    Offsets m_off;
    std::shared_ptr<const SkillsState> m_state;
    RetryTimer m_retry;
    uint64_t m_areaSerial = 0;
    int64_t m_lastSkillsMs = 0, m_lastBlocksMs = 0;
    bool m_wasOpen = false;
    uint64_t m_nextSerial = 1;
    friend struct SkillsReaderImpl;   // helpers live in SkillsReader.cpp
    // ---- implementation state (defined in SkillsReader.cpp) ----
    std::shared_ptr<struct SkillsWork> m_work;
};

// ---------------------------------------------------------------------------------------------
// What SkillsReader.cpp adds to the contract above:
//   - The PSD is [GameUiRoot + psdSlot]; its inventory records map Inventories.dat rows to
//     InventoryStructs once per area (rebuilt when a walked inventory no longer reports its row).
//   - Items of a gem inventory with a "SkillGem" component are skills; items without one that have
//     "Mods" contribute the gems of [Mods + modsGrantedSkills] (itemGranted). The equipment rows of
//     kGearSlots (Ring3 by Offsets::invRowRing3) are walked for granted skills only.
//   - Component lookups are cached per lookup object and item names per (entity, EntityDetails)
//     for the area, so a periodic read costs a few reads per gem.
//   - A support socket's identity ([+supSocketOwnerRow], [+supSocketOwnerSlot], [+supSocketIndex])
//     must name the block's own row, slot and its index k, and the shown sockets must be the first
//     ones: otherwise socketsAgree is false.
//   - Block <-> skill: the (row, slot) of the big socket; several skills there (one item granting
//     several) or none: the header text equal to the gem's name (a "{0}" template matches by the
//     text before it).
//   - A failed character read keeps nothing (skillsKnown false, the problem code) and is retried
//     after 250 ms, after 2 s once ten reads in a row failed; Kick() retries at once.

// The panel on screen this frame.
struct SkillsFrame {
    bool ok = false;                        // false = nothing may be drawn
    Rect panel;                             // the Skills panel
    Rect clip;                              // the scroll view: every mark must be clipped to it
    struct Block {
        Rect block;                         // the whole block
        Rect header;                        // the header row (SkillBlockUi::headerRow; ok == false without one)
        Rect bigSocket;
        std::array<Rect, kMaxSupportSockets> sockets;   // ok == false for sockets that are not shown
        bool inView = false;                // the block intersects `clip`
    };
    std::vector<Block> blocks;              // parallel to SkillsState::blocks
    // ---- the game's tooltip ----
    // The tooltip the game shows now, whatever it belongs to (GearReader.h ReadGameTooltip): a gem's
    // tooltip opens RIGHT of its socket, top clamped to the client (over the inventory panel when it
    // is open); an item's opens left of the item. Read whenever the panel is open (also when `ok` is
    // false because a rectangle failed); it does not depend on SkillsState::blocks.
    GameTooltip tooltip;
};

// RENDER thread, once per frame while the Skills panel is open: one UiRects batch (the panel, the
// scroll view and, per live block, the block, its header, big socket and shown support sockets).
// The host formula already includes the scroll offset. And ReadGameTooltip (before the batch).
SkillsFrame ReadSkillsFrame(IGame& game, const SkillsState& state);

} // namespace bp
