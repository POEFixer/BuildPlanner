#pragma once
// BuildPlanner - the state owner. The public API and the view structs below are what the settings
// tabs and the overlays (src/ui) code against.
// BP_TESTS_EXCLUDE: includes the plugin SDK; not built into tests/.
// App.h - the plugin's state owner: settings, the build store, tree and gem data, the
// selection, the plan, what the watcher saw of the game, and the per-frame data of the overlays.
//
// THREADS (CLAUDE.md "Threads"). Three threads touch an App:
//   RENDER thread   the host's DrawUI / DrawSettings / SaveSettings / OnEnable / OnDisable. EVERY
//                   public method of App is render-thread only, except WantsOverlay(). Both DrawUI
//                   and DrawSettings start with Pump(). The two run in DIFFERENT ImGui contexts
//                   (separate menu window): never keep an ImGui id, window pointer or frame count
//                   across them.
//   WATCHER thread  owned by App: discovery and slow reads through the four readers. It publishes
//                   one immutable GameView (shared_ptr<const>), swapped under a mutex. Period: 16 ms
//                   while one of our panels is open, 100 ms otherwise in game, 1 s out of game.
//   JOB thread      owned by App: one job at a time (data load, import, tree update, .build file),
//                   cancellable. A job works on COPIES and its result is applied in Pump().
//   Both threads are stopped and joined in Stop() (OnDisable and the destructor): hot reload unloads
//   the DLL, so no thread may outlive the App.
//
// LIVE STATE (a hard rule): the in-memory store, selection and plan are the only live state.
// An import becomes visible on the very next Pump(): the store is updated, the build selected, the
// plan rebuilt. Files are write-behind copies (saved 1 s after the last change and in Stop()).
//
// POINTER VALIDITY: references and raw pointers returned by the accessors below (Active().build,
// Plan(), Frame(), ...) are valid until the next Pump() or the next mutating call (SelectBuild,
// DeleteBuild, StartImport result, ...), whichever comes first. UI code reads them within one draw
// call and never stores them.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../../../../POEFixer/plugin_sdk/PluginSDK.h"

#include "../core/BuildStore.h"
#include "../core/CharReader.h"
#include "../core/Game.h"
#include "../core/GearMatch.h"
#include "../core/GearReader.h"
#include "../core/GemData.h"
#include "../core/Loadouts.h"
#include "../core/Offsets.h"
#include "../core/Planner.h"
#include "../core/Settings.h"
#include "../core/SkillMatch.h"
#include "../core/SkillsReader.h"
#include "../core/TreeData.h"
#include "../core/TreeReader.h"
#include "../core/Types.h"

namespace bp {

// ---------------------------------------------------------------------------------------------
// Published view structs (what the overlays and the settings tabs read)
// ---------------------------------------------------------------------------------------------

enum class JobKind : uint8_t {
    None = 0,
    LoadData,          // tree.json + gems.json at start (and after "Use bundled data")
    Import,            // a pasted code / link
    UpdateTree,        // "Update tree data": download the newest tree.json
    WriteBuildFile,    // the game's own Build Planner file
    RemoveBuildFile
};
inline constexpr size_t kJobKindCount = 6;

// What the UI shows about the last job of a kind. The text is already in the program language (made
// on the render thread when the job ended); it names the cause and what to do.
struct JobNote {
    enum class Tone : uint8_t { None = 0, Busy, Ok, Error };
    Tone tone = Tone::None;
    std::string text;          // localized, "" = nothing to show
    std::string detail;        // English, for the Diagnostics card and the log ("" none)
};

// The tree data in use.
struct TreeInfo {
    bool loading = false;      // a LoadData / UpdateTree job is running
    bool loaded = false;
    std::string from;          // "bundled" | "downloaded" ("" = none loaded)
    std::string treeVersion;   // PassiveTree::treeVersion ("0_5")
    std::string source, commit;
    int nodes = 0;
    int64_t fileTime = 0;      // unix seconds of the downloaded file, 0 = n/a
    std::string error;         // the last load problem (English), "" none
};

// What the watcher saw of the game. Immutable; View() never returns null.
struct GameView {
    GameStatus status;
    std::shared_ptr<const TreeTable> tree;        // never null
    bool treeOpen = false;                        // the passive tree panel is visible
    std::shared_ptr<const CharState> character;   // never null
    std::shared_ptr<const GearState> gear;        // never null; gear->panelOpen = inventory open
    std::shared_ptr<const SkillsState> skills;    // never null; skills->panelOpen = Skills panel open
    PlayerInfo player;                            // refreshed about once a second
    uint64_t serial = 0;                          // a new value for every published view
    int64_t publishedMs = 0;                      // steady clock, milliseconds
};

// The selection in effect, resolved (Loadouts.h). Rebuilt in Pump() when its inputs change.
struct ActiveView {
    const StoredBuild* build = nullptr;   // the selected build, nullptr = none
    std::vector<Stage> stages;            // BuildStages(*build, NeverListedIds(tree data, *build), tree data)
    StageChoice choice;                   // the stage followed and why
    ActiveSets sets;                      // the tree / skill set / gear set in effect

    const BuildSpec* Spec() const {
        return (build && sets.spec >= 0 && sets.spec < static_cast<int>(build->specs.size()))
                   ? &build->specs[static_cast<size_t>(sets.spec)] : nullptr;
    }
    const BuildSkillSet* SkillSet() const {
        return (build && sets.skillSet >= 0 && sets.skillSet < static_cast<int>(build->skillSets.size()))
                   ? &build->skillSets[static_cast<size_t>(sets.skillSet)] : nullptr;
    }
    const BuildItemSet* ItemSet() const {
        return (build && sets.itemSet >= 0 && sets.itemSet < static_cast<int>(build->itemSets.size()))
                   ? &build->itemSets[static_cast<size_t>(sets.itemSet)] : nullptr;
    }
    const Stage* StageInfo() const {
        return (choice.stage >= 0 && choice.stage < static_cast<int>(stages.size()))
                   ? &stages[static_cast<size_t>(choice.stage)] : nullptr;
    }
};

// The plan and what the overlay and the Passives tab derive from it; rebuilt together in Pump()
// when the build, the stage, the tiers setting, the tree data or the character's passives change.
struct PlanView {
    bool valid = false;               // a build, a tree of it and the tree data are there
    Plan plan;
    PlanNumbers numbers;
    bool charKnown = false;           // the plan compares against the character (else against nothing:
                                      // every build node is Missing)
    std::string className;            // whose start anchors the plan (ClassInfo::name)
    std::string ascendancy;           // the ascendancy ordered (AscendancyInfo::nodesName), "" none
    // Its display name. Two ascendancies can share their nodes (Lich / Abyssal Lich): the build's
    // name when the build's ascendancy has that nodesName, else the first ascendancy that has it.
    // className + ascendancyName are what Node::NameFor / StatsFor take (a Witch, Druid or Huntress
    // sees other passives on kNodeSwitchable nodes).
    std::string ascendancyName;
    // The character's class is not the build's, or its ascendancy has other NODES than the build's
    // (compared by nodesName: an Abyssal Lich character on an Abyssal Lich build reads as Lich in
    // the game's data and must not differ).
    bool classDiffers = false;
    bool treeOlder = false;           // the stage's treeVersion is older than the tree data
    uint64_t serial = 0;              // moves on every rebuild
};

// Everything the overlays need for ONE frame. Filled by ReadOverlayFrame() (DrawUI only).
struct OverlayFrame {
    bool inGame = false;
    // The host's overlay runs this frame (ctx->Game.IsOverlayMode()): DrawUI draws over the GAME.
    // While it is false the host calls DrawUI inside the menu window, where game client pixels mean
    // nothing: the three draw-list overlays (tree, gear, Skills panel) draw NOTHING then, and
    // ReadOverlayFrame reads nothing of the game. WantsOverlay() asks the host for the overlay, but
    // it lags the watcher by one tick, so a panel can be open for a frame or two before the overlay
    // runs. (The "Build skills" info window does not depend on it: the host places that window.)
    bool overlayActive = false;
    bool menuVisible = false;         // the program's menu is shown in this overlay: draw on the
                                      // BACKGROUND draw list then, else on the foreground one
    float clientW = 0, clientH = 0;   // game client size (= the overlay's display size)
    Vec2 mouse;                       // cursor in client pixels (GetCursorPos + ScreenToClient)
    bool mouseValid = false;          // false = unknown or outside the client: no hover

    // Passive tree. Fail closed: rings, numbers, lines, badges, the summary and the
    // tooltip are drawn only when DrawTree() says so, which needs the table, the pose AND a valid
    // CharState. Without the character's passives every build node would read as Missing: green rings
    // and step numbers on points the player already has. When the tree is open and DrawTree() is
    // false, the overlay draws ONE warning line (TreeWarning(): the cause and what to do) and nothing
    // else.
    // Node indices: Plan() indexes App::Tree()->nodes, TreeTable::uiOfNode indexes table->tree->nodes.
    // ReadOverlayFrame leaves pose.ok false unless both are the SAME tree object (they differ for a
    // moment after "Update tree data"), so with pose.ok the two index spaces are one.
    bool treeOpen = false;
    std::shared_ptr<const TreeTable> table;         // never null
    TreePose pose;
    std::shared_ptr<const CharState> character;     // never null

    // Inventory: draw when DrawGear().
    bool inventoryOpen = false;
    std::shared_ptr<const GearState> gear;          // never null
    GearFrame gearFrame;

    // Skills panel: draw when DrawSkills(). Socket marks only on blocks with
    // SkillBlockUi::socketsAgree.
    bool skillsOpen = false;
    std::shared_ptr<const SkillsState> skills;      // never null
    SkillsFrame skillsFrame;

    bool DrawTree() const { return overlayActive && treeOpen && table->found && pose.ok && character->valid; }
    // The tree is open over the game but DrawTree() is false: which of the two problem texts to show
    // (Ui.h TreeProblemText / CharProblemText). 0 = no warning (the tree is closed, the overlay does
    // not run, the pose is not readable for a frame, or everything is fine), 1 = the table's problem,
    // 2 = the character's problem.
    int TreeWarning() const {
        if (!overlayActive || !treeOpen || DrawTree()) return 0;
        if (!table->found) return 1;
        if (!character->valid) return 2;
        return 0;
    }
    bool DrawGear() const { return overlayActive && inventoryOpen && gear->found && gearFrame.ok; }
    bool DrawSkills() const { return overlayActive && skillsOpen && skillsFrame.ok; }
};

// ---------------------------------------------------------------------------------------------
// The App
// ---------------------------------------------------------------------------------------------

class App {
public:
    App();
    ~App();                                    // calls Stop()
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // ---- lifecycle (Plugin.cpp forwards) ----
    // OnEnable: loads offsets, settings and builds, starts both threads, posts the LoadData job.
    // `ctx` outlives the App. `hostCompatible` false: nothing reads the game; the settings page
    // still works. Calling Start twice without Stop is a no-op.
    void Start(const PluginSDK::Context* ctx, const std::filesystem::path& pluginDir, bool hostCompatible);
    // OnDisable: cancels the running job, stops and joins both threads, flushes unsaved files.
    // Idempotent; never throws.
    void Stop();
    bool Started() const { return m_started; }
    // The start of DrawUI and DrawSettings: drains finished jobs (an import is applied HERE), takes
    // the watcher's newest GameView, resolves the selection, rebuilds the plan and the gear / skill
    // advice when their inputs changed, saves files whose debounce elapsed. Cheap and idempotent:
    // calling it twice in one frame does no extra work.
    void Pump();
    // DrawUI only, after Pump(): the exact per-frame reads (the tree pose, the visible bits of the
    // inventory and Skills panels, the slot and socket rects, the cursor). The ONLY game reads on the
    // render thread. A panel this frame reads as hidden counts as closed (treeOpen / inventoryOpen /
    // skillsOpen false), whatever the watcher's view still says.
    void ReadOverlayFrame();
    // SaveSettings: writes settings.json and builds.json now when they have unsaved changes.
    void SaveNow();
    // The writer's error (English) of the last attempt to save settings.json / builds.json, "" while
    // the last save worked. A failed save is tried again every kSaveRetryMs (core/Settings.h
    // WriteBehind), so nothing the user did is dropped while the plugin runs; the page shows the line.
    const std::string& SettingsSaveError() const { return m_settingsFile.Error(); }
    const std::string& StoreSaveError() const { return m_storeFile.Error(); }
    // ANY thread (the host polls it): a build is selected and one of our panels is open or the
    // skills window is to be shown.
    bool WantsOverlay() const { return m_wantsOverlay.load(std::memory_order_relaxed); }

    // ---- host ----
    const PluginSDK::Context* Ctx() const { return m_ctx; }       // nullptr before Start
    bool HostCompatible() const { return m_hostCompatible; }
    const std::filesystem::path& PluginDir() const { return m_pluginDir; }
    const std::filesystem::path& DataDir() const { return m_dataDir; }       // <plugin>/data
    const std::filesystem::path& ConfigDir() const { return m_configDir; }   // <plugin>/config

    // ---- settings ----
    const Settings& Config() const { return m_settings; }
    // Edit through the reference, then call ConfigChanged() once (it schedules the save and applies
    // what other threads depend on).
    Settings& EditConfig() { return m_settings; }
    void ConfigChanged();
    void ResetDisplay();                       // ResetDisplaySettings + ConfigChanged

    // ---- builds ----
    const BuildStore& Store() const { return m_store; }
    void SelectBuild(const std::string& id);   // "" = none
    void DeleteBuild(const std::string& id);
    bool RenameBuild(const std::string& id, const std::string& name);
    // Pins of the SELECTED build (-1 = automatic). No-ops without a selected build.
    void PinStage(int spec);
    void PinSkillSet(int skillSet);
    void PinItemSet(int itemSet);
    const ActiveView& Active() const { return m_active; }

    // ---- jobs ----
    // Imports pasted text on the job thread (PobImport.h). Refused (false) while another job runs.
    // On success the build is stored, selected and its hints show on the next frame.
    bool StartImport(std::string text);
    void CancelJob();                          // asks the running job to stop
    bool JobRunning() const { return m_job != JobKind::None; }
    JobKind RunningJob() const { return m_job; }
    const JobNote& Note(JobKind kind) const { return m_notes[static_cast<size_t>(kind)]; }
    const std::string& LastImportedId() const { return m_lastImportedId; }

    // ---- data ----
    std::shared_ptr<const PassiveTree> Tree() const { return m_tree; }   // null until loaded
    std::shared_ptr<const GemData> Gems() const { return m_gems; }       // null until loaded
    const TreeInfo& TreeStatus() const { return m_treeInfo; }
    bool UpdateTreeData();                     // posts UpdateTree; false while another job runs
    bool UseBundledData();                     // deletes the downloaded copy, reloads the bundled file

    // ---- the game's own Build Planner ----
    bool WriteBuildFile();                     // posts WriteBuildFile for the stage in effect
    bool RemoveBuildFile();                    // posts RemoveBuildFile for Settings::buildFileName
    void OpenBuildFolder();                    // opens the BuildPlanner folder in Explorer (ShellExecuteW)

    // ---- what the plugin sees and derives ----
    std::shared_ptr<const GameView> View() const { return m_view; }      // never null
    const PlanView& Plan() const { return m_plan; }
    const GearAdvice& Gear() const { return m_gearAdvice; }              // also out of game (Unknown marks)
    const SkillAdvice& Skills() const { return m_skillAdvice; }
    const OverlayFrame& Frame() const { return m_frame; }

    // ---- the "Build skills" window ----
    // Should DrawSkillsWindow show the window this frame: the mode (always / with the Skills panel /
    // never = only when the user opens it), flipped by the hotkey, the button or CloseSkillsWindow
    // until the mode's own answer changes (core/Settings.h SkillsWindowState). Never without a build.
    bool SkillsWindowShown() const { return m_skillsWindow.Shown(); }
    void ToggleSkillsWindow();                 // the Display tab's button (the hotkey is polled in Pump)
    void CloseSkillsWindow();                  // the user closed it through the window's own menu

    // ---- diagnostics ----
    const OffsetsInfo& OffsetsInUse() const { return m_offsets; }
    // The last warnings and errors (English, newest last, at most 50), also written to the host log.
    const std::vector<std::string>& RecentProblems() const { return m_problems; }
    void LogInfo(const std::string& line);     // host log only
    void LogProblem(const std::string& line);  // host log (warning) + RecentProblems
    void KickReaders();                        // "Retry now": every reader rediscovers on its next tick
    std::string DiagnosticsText() const;       // "Copy diagnostics": English, multi-line

    // ---- demo mode (CLAUDE.md "Demo mode") ----
    // true while <plugin>/config/demo.json drives the App: the game is a capture (or nothing), the
    // overlay counts as running and the cursor comes from the file. Never on in normal use.
    bool DemoMode() const { return m_demo; }
    // Preview aids of demo.json, for tools/plugin_ui_preview (it renders only the selected tab and
    // keeps the mouse off): "tab" = the settings tab to select ("builds" | "passives" | "gear" |
    // "skills" | "display"; "" = the user's), "open" = draw every expandable part opened,
    // "window" = show the "Build skills" window whatever its mode says (a preview has no hotkey).
    const std::string& DemoTab() const { return m_demoTab; }
    bool DemoOpenAll() const { return m_demoOpenAll; }

private:
    struct Shared;                             // App.cpp: what the threads share (mutex-protected)
    struct JobResult;                          // App.cpp
    void WatcherMain();
    void JobMain();
    bool PostJob(JobKind kind, std::function<JobResult()> job);
    void PostLoadData(bool removeDownloaded);
    void DrainResults();
    void ApplyResult(JobResult& r);
    void ApplyLoadData(JobResult& r);
    void ApplyImport(JobResult& r);
    void ApplyUpdateTree(JobResult& r);
    void ApplyBuildFile(JobResult& r);
    void PublishTree();
    void PollSkillsWindow();
    void KeepBuildFileUpdated();
    bool PostWriteBuildFile(bool automatic);
    std::string BuildFileKey() const;
    void LoadDemo();
    void SettleDemo();
    bool WaitForJob(int64_t timeoutMs);
    void TakeView();
    void ResolveActive();
    void RebuildDerived();
    void UpdateWantsOverlay();
    void FlushSaves(bool force);
    void StoreChanged();
    void InvalidateDerived();                  // m_active may dangle: resolve and rebuild everything now

    const PluginSDK::Context* m_ctx = nullptr;
    bool m_hostCompatible = false;
    bool m_started = false;
    bool m_readsGame = false;                  // the watcher runs: a compatible host, or a demo capture
    std::filesystem::path m_pluginDir, m_dataDir, m_configDir;

    Settings m_settings;
    BuildStore m_store;
    OffsetsInfo m_offsets;
    std::shared_ptr<const PassiveTree> m_tree;
    std::shared_ptr<const GemData> m_gems;
    TreeInfo m_treeInfo;

    ActiveView m_active;
    PlanView m_plan;
    GearAdvice m_gearAdvice;
    SkillAdvice m_skillAdvice;
    std::shared_ptr<const GameView> m_view;
    OverlayFrame m_frame;

    JobKind m_job = JobKind::None;
    JobNote m_notes[kJobKindCount];
    std::string m_lastImportedId;
    std::vector<std::string> m_problems;

    SkillsWindowState m_skillsWindow;          // shown or not: the mode, the hotkey, the button, the close
    bool m_hotkeyWasDown = false;

    // "Keep it updated": what the .build file was last written (or tried) from, and
    // when the wanted content last changed (the write waits for it to settle).
    std::string m_buildFileKey, m_buildFileWanted;
    int64_t m_buildFileWantedMs = 0;
    uint64_t m_buildFileSeen = 0;              // what the last look was made from (no text is built per frame)

    // Demo mode (demo.json).
    bool m_demo = false, m_demoOpenAll = false, m_demoMouseSet = false, m_demoWindow = false;
    Vec2 m_demoMouse;
    std::string m_demoTab;
    std::filesystem::path m_demoBuild;

    uint64_t m_storeRev = 1;                   // moves on every store / selection change
    uint64_t m_derivedKey[4] = {};             // what the derived views were built from
    WriteBehind m_settingsFile, m_storeFile;   // settings.json, builds.json: debounce, retry, last error
    int64_t m_lastPumpMs = 0;

    std::atomic<bool> m_wantsOverlay{false};
    // HostGame, or the capture of the demo mode; shared by the render and the watcher thread.
    std::unique_ptr<IGame> m_game;
    std::unique_ptr<Shared> m_shared;          // threads, queues, the published view
};

} // namespace bp
