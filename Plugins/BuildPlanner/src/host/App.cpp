// BuildPlanner - the state owner
// BP_TESTS_EXCLUDE: includes the plugin SDK; not built into tests/.
// App.cpp - see App.h. Files, the two threads, the job queue, the published GameView, Pump() and the
// derived views built from the core modules.
//
// Who touches what:
//   render thread   every member of App.
//   job thread      only the closure it was handed (copies and shared_ptr<const> snapshots) and
//                   Shared::cancel; its JobResult goes into Shared::results under the mutex and is
//                   applied in Pump().
//   watcher thread  its own readers, the IGame, Shared (under the mutex) and m_wantsOverlay.
// Stop() joins both before anything they use is destroyed.
#include "App.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <nlohmann/json.hpp>

#include "../../../../POEFixer/plugin_sdk/PluginUi.h"

#include "../core/BuildFile.h"
#include "../core/FixtureGame.h"
#include "../core/Http.h"
#include "../core/JsonFile.h"
#include "../core/PobImport.h"
#include "../ui/Ui.h"
#include "HostGame.h"

namespace bp {
namespace {

using PluginSDK::I18n::Tr;
using PluginSDK::I18n::TrF;

constexpr size_t kMaxProblems = 50;
// Watcher periods (CLAUDE.md "Threads").
constexpr int64_t kPeriodPanelMs = 16, kPeriodGameMs = 100, kPeriodIdleMs = 1000;
constexpr int64_t kCharPeriodMs = 500, kCharPeriodTreeMs = 100, kPlayerPeriodMs = 1000;
// "Keep it updated": the .build file is rewritten once the stage in effect stood still this long.
constexpr int64_t kBuildFileSettleMs = 1500;
// What may be pasted: a code of a very large build is a few hundred KiB.
constexpr size_t kMaxImportTextBytes = 24u << 20;
constexpr size_t kMaxDemoFileBytes = 64u << 10;
constexpr size_t kTreeListingMaxBytes = 2u << 20;
constexpr uint32_t kTreeListingTimeoutMs = 20000;

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t UnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The file's last write time in unix seconds, 0 when it cannot be read.
int64_t FileUnixSeconds(const std::filesystem::path& file) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data)) return 0;
    const uint64_t ticks = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    const uint64_t epoch = 116444736000000000ull;   // 1601 -> 1970 in 100 ns ticks
    return ticks > epoch ? static_cast<int64_t>((ticks - epoch) / 10000000ull) : 0;
}

std::filesystem::path PathFromUtf8(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), n);
    return std::filesystem::path(wide);
}

// A GameView whose pointers are all set (the "never null" promise of App.h).
std::shared_ptr<GameView> EmptyView() {
    auto v = std::make_shared<GameView>();
    v->tree = std::make_shared<const TreeTable>();
    v->character = std::make_shared<const CharState>();
    v->gear = std::make_shared<const GearState>();
    v->skills = std::make_shared<const SkillsState>();
    return v;
}

std::string HttpCause(const HttpResult& r) {
    if (!r.error.empty()) return r.error;
    return "HTTP " + std::to_string(r.status);
}

const char* YesNo(bool v) { return v ? "yes" : "no"; }

// The problem codes by name, for "Copy diagnostics" (English, like every log line).
const char* Name(TreeProblem p) {
    switch (p) {
    case TreeProblem::None: return "none";
    case TreeProblem::NotInGame: return "not in game";
    case TreeProblem::NoTreeData: return "no tree data";
    case TreeProblem::NoUiRoot: return "no UI root";
    case TreeProblem::NoPanel: return "no panel";
    case TreeProblem::NoCanvas: return "no canvas";
    case TreeProblem::NoNodes: return "no nodes";
    case TreeProblem::DataMismatch: return "data mismatch";
    case TreeProblem::ReadFailed: return "read failed";
    }
    return "?";
}
const char* Name(CharProblem p) {
    switch (p) {
    case CharProblem::None: return "none";
    case CharProblem::NotInGame: return "not in game";
    case CharProblem::NoTreeData: return "no tree data";
    case CharProblem::NoUiRoot: return "no UI root";
    case CharProblem::NoPlayerData: return "no player data";
    case CharProblem::NoPassiveObject: return "no passive object";
    case CharProblem::BadList: return "bad list";
    case CharProblem::UnknownIds: return "unknown ids";
    case CharProblem::BadScalars: return "bad scalars";
    case CharProblem::LevelMismatch: return "level mismatch";
    case CharProblem::ReadFailed: return "read failed";
    }
    return "?";
}
const char* Name(GearProblem p) {
    switch (p) {
    case GearProblem::None: return "none";
    case GearProblem::NotInGame: return "not in game";
    case GearProblem::NoPanel: return "no panel";
    case GearProblem::NoSlots: return "no slots";
    case GearProblem::ReadFailed: return "read failed";
    }
    return "?";
}
const char* Name(SkillsProblem p) {
    switch (p) {
    case SkillsProblem::None: return "none";
    case SkillsProblem::NotInGame: return "not in game";
    case SkillsProblem::NoPlayerData: return "no player data";
    case SkillsProblem::NoGems: return "no gems";
    case SkillsProblem::NoPanel: return "no panel";
    case SkillsProblem::ReadFailed: return "read failed";
    }
    return "?";
}
const char* Name(StageHow h) {
    switch (h) {
    case StageHow::None: return "none";
    case StageHow::Pinned: return "pinned";
    case StageHow::ByLevel: return "by level";
    case StageHow::FirstUnfinished: return "first unfinished";
    case StageHow::PobActive: return "PoB active";
    case StageHow::Last: return "last";
    }
    return "?";
}

} // namespace

struct App::JobResult {
    JobKind kind = JobKind::None;
    std::string error;                           // English, "" = fine
    std::string warning;                         // English, "" = none: logged, the job still succeeded
    bool cancelled = false;
    // LoadData / UpdateTree
    std::shared_ptr<const PassiveTree> tree;
    std::shared_ptr<const GemData> gems;
    std::string treeFrom, treeVersion;
    int64_t treeFileTime = 0;
    std::string gemError;
    bool bundledAgain = false;                   // LoadData after "Use bundled data"
    // Import
    ImportResult import;
    // WriteBuildFile / RemoveBuildFile
    BuildFileResult file;
    std::string fileName, fileKey;
    bool automatic = false;                      // written by "Keep it updated", not by the button
};

// What the threads share. `mu` guards everything that is not atomic.
struct App::Shared {
    std::mutex mu;
    std::condition_variable jobCv, watchCv;
    bool stop = false;

    // render -> job thread
    std::function<JobResult()> job;
    bool hasJob = false;
    std::atomic<bool> cancel{false};
    // job thread -> render
    std::vector<JobResult> results;

    // render -> watcher
    std::shared_ptr<const PassiveTree> tree;
    uint64_t treeRev = 0;
    bool kick = false;
    std::atomic<bool> hasBuild{false};
    std::atomic<bool> windowShown{false};
    // watcher -> render
    std::shared_ptr<const GameView> view;

    std::thread jobThread, watcherThread;
};

App::App() : m_view(EmptyView()) {
    m_frame.table = m_view->tree;
    m_frame.character = m_view->character;
    m_frame.gear = m_view->gear;
    m_frame.skills = m_view->skills;
}

App::~App() { Stop(); }

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

void App::Start(const PluginSDK::Context* ctx, const std::filesystem::path& pluginDir, bool hostCompatible) {
    if (m_started) return;
    m_ctx = ctx;
    m_hostCompatible = hostCompatible && ctx != nullptr;
    m_pluginDir = pluginDir;
    m_dataDir = pluginDir / L"data";
    m_configDir = pluginDir / L"config";
    m_problems.clear();
    for (JobNote& n : m_notes) n = JobNote{};
    m_lastImportedId.clear();
    m_buildFileKey.clear();
    m_buildFileWanted.clear();
    m_buildFileSeen = 0;
    m_tree.reset();
    m_gems.reset();
    m_treeInfo = TreeInfo{};

    m_offsets = LoadOffsets(m_dataDir, m_configDir);
    for (const std::string& w : m_offsets.warnings) LogProblem(w);
    std::vector<std::string> warnings;
    m_settings = LoadSettings(m_configDir / L"settings.json", &warnings);
    LoadBuildStore(m_configDir / L"builds.json", m_store, &warnings);
    for (const std::string& w : warnings) LogProblem(w);
    if (!m_settings.selectedBuild.empty() && !m_store.Find(m_settings.selectedBuild)) m_settings.selectedBuild.clear();
    m_settingsFile = WriteBehind{};
    m_storeFile = WriteBehind{};
    m_skillsWindow = SkillsWindowState{};
    m_hotkeyWasDown = false;
    ++m_storeRev;

    // Demo mode (a capture instead of the live game) or the host.
    m_game.reset();
    LoadDemo();
    if (!m_game) m_game = std::make_unique<HostGame>(ctx, m_hostCompatible && !m_demo);
    m_shared = std::make_unique<Shared>();
    m_shared->view = m_view = EmptyView();
    m_job = JobKind::None;
    m_started = true;

    try {
        m_shared->jobThread = std::thread([this] { JobMain(); });
        if (m_readsGame) m_shared->watcherThread = std::thread([this] { WatcherMain(); });
    } catch (...) {
        // No thread could be started: the page still shows, nothing loads and nothing reads the game.
        LogProblem("the plugin's worker threads could not be started");
    }

    // The data files load on the job thread: a 2 MB tree.json never parses on the render thread.
    PostLoadData(false);
    if (m_demo) SettleDemo();
}

void App::Stop() {
    if (!m_started) return;
    try {
        if (m_shared) {
            {
                std::lock_guard<std::mutex> lock(m_shared->mu);
                m_shared->stop = true;
                m_shared->cancel.store(true);
            }
            m_shared->jobCv.notify_all();
            m_shared->watchCv.notify_all();
            // A download in flight ends within ~50 ms of the cancel flag (Http.h); the watcher's
            // tick is bounded by its reads. Both are joined: no thread may outlive the DLL. The job
            // thread first (it may hold the tree data the watcher's readers point into only through
            // shared_ptr, so the order between the two does not matter for memory, only for speed).
            if (m_shared->jobThread.joinable()) m_shared->jobThread.join();
            if (m_shared->watcherThread.joinable()) m_shared->watcherThread.join();
        }
        FlushSaves(true);
    } catch (...) {
    }
    m_shared.reset();   // after the joins: the threads used it
    m_game.reset();     // after the watcher: its readers hold references to it
    m_job = JobKind::None;
    m_wantsOverlay.store(false);
    m_active = ActiveView{};
    m_started = false;
    m_readsGame = false;
    m_demo = false;
}

void App::SaveNow() {
    if (m_started) FlushSaves(true);
}

// A save that fails stays due (WriteBehind: again every kSaveRetryMs, and at Stop): a locked file or a
// full disk must not drop the build the user just imported. The page shows the error
// (SettingsSaveError / StoreSaveError); the log gets it once per distinct cause, not every retry.
void App::FlushSaves(bool force) {
    const int64_t now = NowMs();
    const auto flush = [&](WriteBehind& file, const char* name, const auto& save) {
        if (!file.Due(now, force)) return;
        std::string error;
        if (save(&error)) {
            if (!file.Error().empty()) LogInfo(std::string(name) + " is saved again");
            file.Saved();
            return;
        }
        const std::string before = file.Error();
        file.Failed(now, std::move(error));
        if (file.Error() != before) LogProblem(std::string(name) + " was not saved: " + file.Error());
    };
    flush(m_settingsFile, "settings.json", [this](std::string* e) { return SaveSettings(m_configDir / L"settings.json", m_settings, e); });
    flush(m_storeFile, "builds.json", [this](std::string* e) { return SaveBuildStore(m_configDir / L"builds.json", m_store, e); });
}

// ---------------------------------------------------------------------------------------------
// Demo mode (CLAUDE.md "Demo mode")
// ---------------------------------------------------------------------------------------------

void App::LoadDemo() {
    m_demo = m_demoOpenAll = m_demoMouseSet = m_demoWindow = false;
    m_demoMouse = Vec2{};
    m_demoTab.clear();
    m_demoBuild.clear();
    m_readsGame = m_hostCompatible;

    const FileRead file = ReadFileCapped(m_configDir / L"demo.json", kMaxDemoFileBytes);
    if (file.status == FileStatus::Missing) return;
    if (!file.Ok()) {
        LogProblem("demo.json could not be read: " + file.error);
        return;
    }
    const nlohmann::json j = nlohmann::json::parse(file.bytes.begin(), file.bytes.end(), nullptr, false);
    if (!j.is_object()) {
        LogProblem("demo.json is not a JSON object: the demo mode stays off");
        return;
    }
    m_demo = true;
    m_readsGame = false;
    const auto text = [&j](const char* key) -> std::string {
        const auto it = j.find(key);
        return (it != j.end() && it->is_string() && it->get_ref<const std::string&>().size() <= 1024) ? it->get<std::string>()
                                                                                                  : std::string();
    };
    const auto resolve = [this](const std::string& value) {
        std::filesystem::path p = PathFromUtf8(value);
        if (!p.empty() && p.is_relative()) p = m_pluginDir / p;
        return p;
    };
    if (const std::filesystem::path fixture = resolve(text("fixture")); !fixture.empty()) {
        std::string error;
        m_game = OpenFixtureGame(fixture, &error);
        if (m_game) m_readsGame = true;
        else LogProblem("demo: the capture " + PathUtf8(fixture) + " could not be opened: " + error);
    }
    m_demoBuild = resolve(text("build"));
    m_demoTab = text("tab");
    if (const auto it = j.find("open"); it != j.end() && it->is_boolean()) m_demoOpenAll = it->get<bool>();
    if (const auto it = j.find("window"); it != j.end() && it->is_boolean()) m_demoWindow = it->get<bool>();
    if (const auto it = j.find("mouse"); it != j.end() && it->is_array() && it->size() == 2 && (*it)[0].is_number() &&
                                         (*it)[1].is_number()) {
        const double x = (*it)[0].get<double>(), y = (*it)[1].get<double>();
        if (std::isfinite(x) && std::isfinite(y) && std::fabs(x) < 1e6 && std::fabs(y) < 1e6) {
            m_demoMouse = {static_cast<float>(x), static_cast<float>(y)};
            m_demoMouseSet = true;
        }
    }
    LogInfo("demo mode: a capture replaces the live game");
}

bool App::WaitForJob(int64_t timeoutMs) {
    const int64_t until = NowMs() + timeoutMs;
    for (;;) {
        DrainResults();
        if (m_job == JobKind::None) return true;
        if (NowMs() >= until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// The demo's start is synchronous on purpose: a preview tool draws a handful of frames and takes its
// picture, so the data, the demo build and the readers' first pass must be there before frame one.
// Never reached in normal use.
void App::SettleDemo() {
    WaitForJob(60000);   // the data files
    if (!m_demoBuild.empty()) {
        const std::string source = "demo: " + PathUtf8(m_demoBuild.filename());
        const StoredBuild* stored = nullptr;
        for (const StoredBuild& b : m_store.builds)
            if (b.source == source) stored = &b;
        if (stored) {
            SelectBuild(stored->id);
        } else {
            FileRead text = ReadFileCapped(m_demoBuild, kMaxImportTextBytes);
            if (!text.Ok()) {
                LogProblem("demo: the build " + PathUtf8(m_demoBuild) + " could not be read: " + text.error);
            } else if (StartImport(std::move(text.bytes)) && WaitForJob(60000)) {
                if (StoredBuild* b = m_store.Find(m_lastImportedId)) {
                    b->source = source;
                    StoreChanged();
                }
            }
        }
    }
    if (m_readsGame && m_shared->watcherThread.joinable()) {
        // A few full passes of the readers over the capture, with the tree data in place.
        uint64_t base = 0;
        {
            std::lock_guard<std::mutex> lock(m_shared->mu);
            base = m_shared->view ? m_shared->view->serial : 0;
        }
        KickReaders();
        const int64_t until = NowMs() + 5000;
        for (;;) {
            uint64_t serial = 0;
            {
                std::lock_guard<std::mutex> lock(m_shared->mu);
                serial = m_shared->view ? m_shared->view->serial : 0;
            }
            if (serial >= base + 5 || NowMs() >= until) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    TakeView();
    InvalidateDerived();
}

// ---------------------------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------------------------

void App::JobMain() {
    Shared& sh = *m_shared;
    for (;;) {
        std::function<JobResult()> job;
        {
            std::unique_lock<std::mutex> lock(sh.mu);
            sh.jobCv.wait(lock, [&sh] { return sh.stop || sh.hasJob; });
            if (sh.stop) return;
            job = std::move(sh.job);
            sh.job = nullptr;
            sh.hasJob = false;
        }
        JobResult r;
        try {
            r = job();
        } catch (const std::exception& e) {
            r = JobResult{};
            r.error = std::string("the job failed: ") + e.what();
        } catch (...) {
            r = JobResult{};
            r.error = "the job failed";
        }
        job = nullptr;   // the closure's copies die here, not under the lock
        try {
            std::lock_guard<std::mutex> lock(sh.mu);
            sh.results.push_back(std::move(r));
        } catch (...) {
            // Out of memory while queueing: an empty result still ends the job for the render thread.
            std::lock_guard<std::mutex> lock(sh.mu);
            sh.results.emplace_back();
            sh.results.back().error = "the job's result was lost (out of memory)";
        }
    }
}

void App::WatcherMain() {
    Shared& sh = *m_shared;
    IGame& game = *m_game;
    TreeReader treeReader(game, m_offsets.values);
    CharReader charReader(game, m_offsets.values);
    GearReader gearReader(game, m_offsets.values);
    SkillsReader skillsReader(game, m_offsets.values);
    std::shared_ptr<const CharState> character = std::make_shared<const CharState>();
    PlayerInfo player;
    uint64_t treeRevSeen = 0, serial = 0;
    int64_t lastCharMs = 0, lastPlayerMs = 0;
    bool wasAttached = false;

    for (;;) {
        int64_t period = kPeriodIdleMs;
        try {
            {
                std::lock_guard<std::mutex> lock(sh.mu);
                if (sh.stop) return;
                if (sh.treeRev != treeRevSeen) {
                    treeRevSeen = sh.treeRev;
                    treeReader.SetTree(sh.tree);
                    charReader.SetTree(sh.tree);
                    lastCharMs = 0;
                }
                if (sh.kick) {
                    // "At once" (the RetryTimer rule, core/Game.h): every reader rediscovers on this
                    // very tick, the character's passives included (they are otherwise read at 2 Hz).
                    sh.kick = false;
                    treeReader.Kick();
                    charReader.Kick();
                    gearReader.Kick();
                    skillsReader.Kick();
                    lastCharMs = 0;
                    lastPlayerMs = 0;
                }
            }
            WatchTick t;
            t.nowMs = NowMs();
            t.status = game.Status();
            if (wasAttached && !t.status.attached) {
                // Another game process may come next: nothing cached may survive it.
                treeReader.Reset();
                charReader.Reset();
                gearReader.Reset();
                skillsReader.Reset();
                character = std::make_shared<const CharState>();
                player = PlayerInfo{};
                lastCharMs = lastPlayerMs = 0;
            }
            wasAttached = t.status.attached;

            auto view = std::make_shared<GameView>();
            view->status = t.status;
            const TreeWatch tw = treeReader.Tick(t);
            view->tree = tw.table ? tw.table : std::make_shared<const TreeTable>();
            view->treeOpen = tw.open;
            if (lastCharMs == 0 || t.nowMs - lastCharMs >= (tw.open ? kCharPeriodTreeMs : kCharPeriodMs)) {
                lastCharMs = t.nowMs;
                if (auto c = charReader.Tick(t, view->tree.get(), tw.open)) character = std::move(c);
            }
            view->character = character;
            view->gear = gearReader.Tick(t);
            view->skills = skillsReader.Tick(t);
            if (!view->gear) view->gear = std::make_shared<const GearState>();
            if (!view->skills) view->skills = std::make_shared<const SkillsState>();
            if (!t.status.inGame) {
                player = PlayerInfo{};
                lastPlayerMs = 0;
            } else if (lastPlayerMs == 0 || t.nowMs - lastPlayerMs >= kPlayerPeriodMs) {
                lastPlayerMs = t.nowMs;
                player = game.Player();
            }
            view->player = player;
            view->serial = ++serial;
            view->publishedMs = t.nowMs;

            const bool panelOpen = view->treeOpen || view->gear->panelOpen || view->skills->panelOpen;
            period = !t.status.inGame ? kPeriodIdleMs : panelOpen ? kPeriodPanelMs : kPeriodGameMs;
            // The host polls WantsOverlay() also while it draws no plugin frame: keep it current here.
            m_wantsOverlay.store(t.status.inGame && sh.hasBuild.load() && (panelOpen || sh.windowShown.load()),
                                 std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(sh.mu);
            sh.view = std::move(view);
        } catch (...) {
            period = kPeriodIdleMs;   // out of memory or a reader bug: keep the last view, try again later
        }
        std::unique_lock<std::mutex> lock(sh.mu);
        sh.watchCv.wait_for(lock, std::chrono::milliseconds(period), [&sh] { return sh.stop || sh.kick; });
        if (sh.stop) return;
    }
}

// ---------------------------------------------------------------------------------------------
// Jobs
// ---------------------------------------------------------------------------------------------

bool App::PostJob(JobKind kind, std::function<JobResult()> job) {
    if (!m_started || !m_shared || m_job != JobKind::None || !m_shared->jobThread.joinable()) return false;
    {
        std::lock_guard<std::mutex> lock(m_shared->mu);
        m_shared->cancel.store(false);
        m_shared->job = std::move(job);
        m_shared->hasJob = true;
    }
    // Results are drained only on this thread, so setting m_job after the hand-over cannot be late.
    m_job = kind;
    m_shared->jobCv.notify_all();
    return true;
}

void App::PostLoadData(bool removeDownloaded) {
    const std::filesystem::path dataDir = m_dataDir, configDir = m_configDir;
    const bool downloaded = m_settings.treeSource == "downloaded";
    // A downloaded (unstripped) tree.json names no version: the update job stored the folder it came from.
    const std::string downloadedVersion = m_settings.treeDownloadedVersion;
    const bool posted = PostJob(JobKind::LoadData, [dataDir, configDir, downloaded, downloadedVersion, removeDownloaded]() {
        JobResult r;
        r.kind = JobKind::LoadData;
        r.bundledAgain = removeDownloaded;
        std::string err;
        if (removeDownloaded && !RemoveFile(configDir / L"tree.json", &err))
            r.warning = "the downloaded tree data was not removed: " + err;
        auto tree = std::make_shared<PassiveTree>();
        if (downloaded && LoadTreeFile(configDir / L"tree.json", *tree, &err, downloadedVersion)) {
            r.treeFrom = "downloaded";
            r.treeFileTime = FileUnixSeconds(configDir / L"tree.json");
        } else {
            if (downloaded) r.warning = "the downloaded tree data could not be loaded (" + err + "): the bundled data is used";
            tree = std::make_shared<PassiveTree>();
            if (LoadTreeFile(dataDir / L"tree.json", *tree, &err)) {
                r.treeFrom = "bundled";
            } else {
                r.error = "tree data: " + err;
                tree.reset();
            }
        }
        r.tree = std::move(tree);
        auto gems = std::make_shared<GemData>();
        if (LoadGemFile(dataDir / L"gems.json", *gems, &err)) r.gems = std::move(gems);
        else r.gemError = "gem data: " + err;
        return r;
    });
    m_treeInfo.loading = posted;
}

bool App::StartImport(std::string text) {
    if (!m_started || m_job != JobKind::None) return false;
    JobNote& note = m_notes[static_cast<size_t>(JobKind::Import)];
    if (text.size() > kMaxImportTextBytes) {
        note = JobNote{};
        note.tone = JobNote::Tone::Error;
        note.text = ImportProblemText(ImportProblem::Unrecognized, {});
        note.detail = "the pasted text is larger than 24 MiB";
        return false;
    }
    const std::shared_ptr<const GemData> gems = m_gems;
    const std::atomic<bool>* cancel = &m_shared->cancel;   // lives until Stop() has joined the job thread
    const bool offline = m_demo;                           // a demo never uses the network
    const bool posted = PostJob(JobKind::Import, [text = std::move(text), gems, cancel, offline]() {
        JobResult r;
        r.kind = JobKind::Import;
        const HttpGetFn http = [offline](const std::string& url, size_t maxBytes, uint32_t timeoutMs, const std::atomic<bool>* c) {
            if (offline) {
                HttpResult none;
                none.error = "the demo mode does not download";
                return none;
            }
            return HttpGet(url, maxBytes, timeoutMs, c, kImportHosts);
        };
        r.import = ImportFromText(text, cancel, http, gems.get());
        return r;
    });
    if (!posted) return false;
    note = JobNote{};
    note.tone = JobNote::Tone::Busy;
    note.text = Tr("import_busy");
    return true;
}

void App::CancelJob() {
    if (m_started && m_shared && m_job != JobKind::None) m_shared->cancel.store(true);
}

bool App::UpdateTreeData() {
    if (!m_started || m_job != JobKind::None) return false;
    if (m_demo) {
        // A demo never uses the network (the guide's "Demo mode"; StartImport is offline too): its
        // renders must not depend on what GitHub serves today, nor replace the demo's tree data.
        JobNote& note = m_notes[static_cast<size_t>(JobKind::UpdateTree)];
        note = JobNote{};
        note.tone = JobNote::Tone::Error;
        note.text = Tr("tree_update_demo");
        note.detail = "the demo mode does not download";
        return false;
    }
    const std::filesystem::path configDir = m_configDir;
    const std::atomic<bool>* cancel = &m_shared->cancel;
    const bool posted = PostJob(JobKind::UpdateTree, [configDir, cancel]() {
        JobResult r;
        r.kind = JobKind::UpdateTree;
        // 1. Which version folder is the newest (the GitHub listing of Path of Building's TreeData).
        const HttpResult listing = HttpGet(TreeListingUrl(), kTreeListingMaxBytes, kTreeListingTimeoutMs, cancel, kTreeUpdateHosts);
        if (!listing.Ok()) {
            r.cancelled = listing.error == "cancelled";
            r.error = "the list of tree versions could not be downloaded: " + HttpCause(listing);
            return r;
        }
        const std::string version = NewestTreeVersion(listing.body);
        if (version.empty()) {
            r.error = "the list of tree versions names no version";
            return r;
        }
        // 2. That folder's tree.json.
        const HttpResult file = HttpGet(TreeFileUrl(version), kTreeDownloadMaxBytes, kTreeDownloadTimeoutMs, cancel, kTreeUpdateHosts);
        if (!file.Ok()) {
            r.cancelled = file.error == "cancelled";
            r.error = "the tree data of version " + version + " could not be downloaded: " + HttpCause(file);
            return r;
        }
        // 3. It must load before it replaces anything.
        auto tree = std::make_shared<PassiveTree>();
        std::string err;
        if (!ParseTree(file.body, *tree, &err, version)) {
            r.error = "the downloaded tree data of version " + version + " is not usable: " + err;
            return r;
        }
        if (cancel->load()) {
            r.cancelled = true;
            r.error = "cancelled";
            return r;
        }
        if (!WriteFileAtomic(configDir / L"tree.json", file.body, &err)) {
            r.error = "the tree data could not be saved: " + err;
            return r;
        }
        r.treeVersion = version;
        r.treeFrom = "downloaded";
        r.treeFileTime = UnixSeconds();
        r.tree = std::move(tree);
        return r;
    });
    if (!posted) return false;
    m_treeInfo.loading = true;
    JobNote& note = m_notes[static_cast<size_t>(JobKind::UpdateTree)];
    note = JobNote{};
    note.tone = JobNote::Tone::Busy;
    note.text = Tr("tree_update_busy");
    return true;
}

bool App::UseBundledData() {
    if (!m_started || m_job != JobKind::None) return false;
    m_settings.treeSource = "bundled";
    m_settings.treeDownloadedVersion.clear();
    ConfigChanged();
    PostLoadData(true);
    if (m_job != JobKind::LoadData) return false;
    m_notes[static_cast<size_t>(JobKind::UpdateTree)] = JobNote{};
    return true;
}

std::string App::BuildFileKey() const {
    const StoredBuild* b = m_active.build;
    if (!b) return {};
    std::string key = b->id;
    key += '|';
    key += b->name;
    for (const int v : {m_active.sets.spec, m_active.sets.skillSet, m_active.sets.itemSet}) {
        key += '|';
        key += std::to_string(v);
    }
    key += '|';
    key += m_tree ? m_tree->treeVersion + "/" + std::to_string(m_tree->nodes.size()) : std::string("-");
    return key;
}

bool App::PostWriteBuildFile(bool automatic) {
    if (!m_started || m_job != JobKind::None || !m_active.build) return false;
    // Copies: the store may change while the job runs.
    auto build = std::make_shared<const StoredBuild>(*m_active.build);
    const ActiveSets sets = m_active.sets;
    const std::shared_ptr<const PassiveTree> tree = m_tree;
    const Stage* stage = m_active.StageInfo();
    const std::string stageTitle = stage ? stage->title : std::string();
    const std::string previous = m_settings.buildFileName;
    const std::string key = BuildFileKey();
    const bool posted = PostJob(JobKind::WriteBuildFile, [build, sets, tree, stageTitle, previous, key, automatic]() {
        JobResult r;
        r.kind = JobKind::WriteBuildFile;
        r.fileKey = key;
        r.automatic = automatic;
        const std::filesystem::path folder = BuildPlannerFolder();
        if (folder.empty()) {
            r.file.error = "the Documents folder could not be found";
            return r;
        }
        const std::string json = BuildFileJson(*build, sets, tree.get(), stageTitle);
        if (json.empty()) {
            r.file.error = "this stage has nothing to write";
            return r;
        }
        r.fileName = BuildFileName(build->name);
        r.file = ::bp::WriteBuildFile(folder, r.fileName, json, previous);
        return r;
    });
    if (!posted) return false;
    m_buildFileKey = key;   // tried: a failure is not retried until the stage changes or the user asks
    if (!automatic) {
        JobNote& note = m_notes[static_cast<size_t>(JobKind::WriteBuildFile)];
        note = JobNote{};
        note.tone = JobNote::Tone::Busy;
        note.text = Tr("buildfile_busy");
    }
    return true;
}

bool App::WriteBuildFile() { return PostWriteBuildFile(false); }

bool App::RemoveBuildFile() {
    if (!m_started || m_job != JobKind::None || m_settings.buildFileName.empty()) return false;
    const std::string name = m_settings.buildFileName;
    const bool posted = PostJob(JobKind::RemoveBuildFile, [name]() {
        JobResult r;
        r.kind = JobKind::RemoveBuildFile;
        r.fileName = name;
        const std::filesystem::path folder = BuildPlannerFolder();
        if (folder.empty()) {
            r.file.error = "the Documents folder could not be found";
            return r;
        }
        r.file = ::bp::RemoveBuildFile(folder, name);
        return r;
    });
    if (!posted) return false;
    JobNote& note = m_notes[static_cast<size_t>(JobKind::WriteBuildFile)];
    note = JobNote{};
    note.tone = JobNote::Tone::Busy;
    note.text = Tr("buildfile_busy");
    return true;
}

void App::KeepBuildFileUpdated() {
    if (!m_settings.buildFileKeepUpdated || m_demo || m_job != JobKind::None || !m_active.build || !m_tree) return;
    // Nothing the file depends on moved since the last look and no write is waiting: no key text is
    // built on an ordinary frame.
    const uint64_t seen = m_storeRev ^ (static_cast<uint64_t>(m_active.sets.spec + 1) << 40) ^
                          (static_cast<uint64_t>(m_active.sets.skillSet + 1) << 48) ^
                          (static_cast<uint64_t>(m_active.sets.itemSet + 1) << 56);
    if (seen == m_buildFileSeen && m_buildFileWanted == m_buildFileKey) return;
    m_buildFileSeen = seen;
    const std::string key = BuildFileKey();
    if (key == m_buildFileKey) {
        m_buildFileWanted = key;
        return;
    }
    // The stage follows the character (a level-up, a point spent): wait until it stood still.
    if (key != m_buildFileWanted) {
        m_buildFileWanted = key;
        m_buildFileWantedMs = m_lastPumpMs;
        return;
    }
    if (m_lastPumpMs - m_buildFileWantedMs < kBuildFileSettleMs) return;
    PostWriteBuildFile(true);
}

void App::OpenBuildFolder() {
    try {
        const std::filesystem::path folder = BuildPlannerFolder();
        if (folder.empty()) {
            LogProblem("the game's Build Planner folder could not be found (Documents is not available)");
            return;
        }
        // The game creates the folder on its first use of the Build Planner; opening it earlier
        // creates it, so Explorer has something to show.
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------------------------
// Pump
// ---------------------------------------------------------------------------------------------

void App::Pump() {
    if (!m_started) return;
    m_lastPumpMs = NowMs();
    DrainResults();
    TakeView();
    RebuildDerived();
    PollSkillsWindow();
    UpdateWantsOverlay();
    KeepBuildFileUpdated();
    FlushSaves(false);
}

// The "Build skills" window (SkillsWindowState, core/Settings.h): the mode's own answer, flipped by
// the hotkey, the button or the window's close until that answer changes. Pump runs twice in a frame
// that draws both the overlay and the settings page: the key's edge is taken once (m_hotkeyWasDown).
void App::PollSkillsWindow() {
    const int vk = m_settings.skillsWindowHotkey;
    const bool down = vk > 0 && vk < 256 && m_ctx && m_hostCompatible && !m_demo && m_ctx->Game.IsForeground() &&
                      !PluginUi::IsCapturingHotkey() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !m_hotkeyWasDown;
    m_hotkeyWasDown = down;
    // A preview has no key to press: demo.json's "window" shows it whatever the mode says.
    const SkillsWindowMode mode = (m_demo && m_demoWindow) ? SkillsWindowMode::Always : m_settings.skillsWindowMode;
    m_skillsWindow.Update(mode, m_view->skills->panelOpen, m_active.build != nullptr, pressed);
}

void App::DrainResults() {
    if (!m_shared) return;
    std::vector<JobResult> results;
    {
        std::lock_guard<std::mutex> lock(m_shared->mu);
        results.swap(m_shared->results);
    }
    for (JobResult& r : results) ApplyResult(r);
}

void App::ApplyResult(JobResult& r) {
    // A job that threw has no kind: it is the one that was running.
    if (r.kind == JobKind::None) r.kind = m_job;
    m_job = JobKind::None;
    if (!r.warning.empty()) LogProblem(r.warning);
    switch (r.kind) {
    case JobKind::LoadData: ApplyLoadData(r); break;
    case JobKind::Import: ApplyImport(r); break;
    case JobKind::UpdateTree: ApplyUpdateTree(r); break;
    case JobKind::WriteBuildFile:
    case JobKind::RemoveBuildFile: ApplyBuildFile(r); break;
    case JobKind::None:
        if (!r.error.empty()) LogProblem(r.error);
        break;
    }
}

void App::PublishTree() {
    {
        std::lock_guard<std::mutex> lock(m_shared->mu);
        m_shared->tree = m_tree;
        ++m_shared->treeRev;
    }
    m_shared->watchCv.notify_all();
    ++m_storeRev;          // the stages' point counts, the plan and the advice depend on the data
    InvalidateDerived();   // the plan indexes the old tree's nodes: never let a frame see both
}

void App::ApplyLoadData(JobResult& r) {
    JobNote& note = m_notes[static_cast<size_t>(JobKind::LoadData)];
    note = JobNote{};
    m_tree = r.tree;
    m_gems = r.gems;
    m_treeInfo = TreeInfo{};
    m_treeInfo.loaded = m_tree != nullptr;
    m_treeInfo.from = m_tree ? r.treeFrom : std::string();
    m_treeInfo.error = r.error;
    if (m_tree) {
        m_treeInfo.treeVersion = m_tree->treeVersion;
        m_treeInfo.source = m_tree->source;
        m_treeInfo.commit = m_tree->commit;
        m_treeInfo.nodes = static_cast<int>(m_tree->nodes.size());
        m_treeInfo.fileTime = r.treeFileTime;
    }
    // The downloaded copy did not load: stay on the bundled data until the user updates again.
    if (m_tree && r.treeFrom == "bundled" && m_settings.treeSource != "bundled") {
        m_settings.treeSource = "bundled";
        m_settings.treeDownloadedVersion.clear();
        ConfigChanged();
    }
    if (!r.error.empty()) LogProblem(r.error);
    if (!r.gemError.empty()) LogProblem(r.gemError);
    if (!r.error.empty() || !r.gemError.empty()) {
        const std::string& cause = !r.error.empty() ? r.error : r.gemError;
        note.tone = JobNote::Tone::Error;
        note.text = TrF("data_load_failed", {{"detail", cause}});
        note.detail = cause;
    } else if (r.bundledAgain) {
        note.tone = JobNote::Tone::Ok;
        note.text = Tr("tree_bundled_ok");
    }
    PublishTree();
}

void App::ApplyUpdateTree(JobResult& r) {
    JobNote& note = m_notes[static_cast<size_t>(JobKind::UpdateTree)];
    note = JobNote{};
    m_treeInfo.loading = false;
    if (!r.error.empty() || !r.tree) {
        // The data in use stays as it is.
        if (r.cancelled) {
            note.text = Tr("tree_update_cancelled");
            return;
        }
        const std::string cause = r.error.empty() ? std::string("no tree data came back") : r.error;
        note.tone = JobNote::Tone::Error;
        note.text = TrF("tree_update_failed", {{"detail", cause}});
        note.detail = cause;
        LogProblem("tree update: " + cause);
        return;
    }
    m_tree = r.tree;
    m_settings.treeSource = "downloaded";
    m_settings.treeDownloadedVersion = r.treeVersion;
    ConfigChanged();
    m_treeInfo = TreeInfo{};
    m_treeInfo.loaded = true;
    m_treeInfo.from = "downloaded";
    m_treeInfo.treeVersion = m_tree->treeVersion;
    m_treeInfo.source = m_tree->source;
    m_treeInfo.commit = m_tree->commit;
    m_treeInfo.nodes = static_cast<int>(m_tree->nodes.size());
    m_treeInfo.fileTime = r.treeFileTime;
    note.tone = JobNote::Tone::Ok;
    note.text = TrF("tree_update_ok", {{"version", m_tree->treeVersion}, {"nodes", std::to_string(m_tree->nodes.size())}});
    LogInfo("tree data updated to version " + m_tree->treeVersion);
    PublishTree();
    KickReaders();
}

void App::ApplyImport(JobResult& r) {
    JobNote& note = m_notes[static_cast<size_t>(JobKind::Import)];
    note = JobNote{};
    if (!r.error.empty()) {   // the import itself threw
        note.tone = JobNote::Tone::Error;
        note.text = ImportProblemText(ImportProblem::Parse, r.error);
        note.detail = r.error;
        LogProblem("import: " + r.error);
        return;
    }
    if (!r.import.ok) {
        const bool cancelled = r.import.problem == ImportProblem::Cancelled;
        note.tone = cancelled ? JobNote::Tone::None : JobNote::Tone::Error;
        note.text = cancelled ? std::string(Tr("import_cancelled")) : ImportProblemText(r.import.problem, r.import.detail);
        note.detail = r.import.detail;
        if (!cancelled) LogInfo("import failed: " + r.import.detail);
        return;
    }
    // The same build again (the trees, skills and items of a build the list already has): no second
    // row. The build of the list is followed, as a fresh import would be. Copies first: SelectBuild
    // rebuilds what points into the store.
    if (const StoredBuild* same = m_store.FindSame(r.import.build)) {
        const std::string id = same->id, name = same->name;
        for (const std::string& w : r.import.warnings) LogInfo("import: " + w);
        LogInfo("import: the build is already in the list (" + id + "), nothing was added");
        m_lastImportedId = id;
        SelectBuild(id);
        note.tone = JobNote::Tone::Ok;
        note.text = TrF("import_same", {{"name", name}});
        return;
    }
    std::vector<std::string> warnings;
    bool tooLarge = false;
    // AddWithinLimit may reallocate the store's vector on EVERY path (it adds the build and, when
    // the file guard refuses it, takes it out again): m_active.build would dangle. Drop it before
    // and resolve it again right after, whatever the outcome.
    m_active = ActiveView{};
    const std::string id = m_store.AddWithinLimit(r.import.build, r.import.source, UnixSeconds(), &warnings, &tooLarge);
    InvalidateDerived();
    for (const std::string& w : r.import.warnings) LogInfo("import: " + w);
    for (const std::string& w : warnings) LogInfo("import: " + w);
    if (id.empty()) {
        note.tone = JobNote::Tone::Error;
        note.text = tooLarge ? Tr("import_file_full") : Tr("import_store_full");
        note.detail = tooLarge ? "builds.json would grow past its size limit" : "the build could not be stored";
        return;
    }
    // The in-memory store is the live state: the build is selected here and its hints show on
    // the next frame. Nothing is reloaded from disk.
    m_lastImportedId = id;
    m_settings.selectedBuild = id;
    ConfigChanged();
    StoreChanged();
    KickReaders();
    const StoredBuild* b = m_store.Find(id);
    note.tone = JobNote::Tone::Ok;
    note.text = TrF("import_ok", {{"name", b ? b->name : std::string()},
                                  {"trees", std::to_string(b ? b->specs.size() : 0)},
                                  {"skills", std::to_string(b ? b->skillSets.size() : 0)},
                                  {"gear", std::to_string(b ? b->itemSets.size() : 0)}});
}

// The Builds tab shows ONE result line for the game's Build Planner card: both file jobs write it.
void App::ApplyBuildFile(JobResult& r) {
    JobNote& note = m_notes[static_cast<size_t>(JobKind::WriteBuildFile)];
    const bool removing = r.kind == JobKind::RemoveBuildFile;
    std::string cause = !r.error.empty() ? r.error : r.file.error;
    if (!r.file.ok && cause.empty()) cause = "the file could not be written";
    const std::string shown = PathUtf8(r.file.file.empty() ? PathFromUtf8(r.fileName) : r.file.file.filename());
    if (!r.file.ok) {
        note = JobNote{};
        note.tone = JobNote::Tone::Error;
        note.text = removing ? TrF("buildfile_remove_failed", {{"detail", cause}}) : TrF("buildfile_failed", {{"detail", cause}});
        note.detail = cause;
        LogProblem(std::string(removing ? "the .build file was not removed: " : "the .build file was not written: ") + cause);
        return;
    }
    if (removing) {
        m_settings.buildFileName.clear();
        m_settings.buildFileKeepUpdated = false;   // it would write the file again at once
        m_buildFileKey.clear();
        m_buildFileWanted.clear();
        ConfigChanged();
        note = JobNote{};
        note.tone = JobNote::Tone::Ok;
        note.text = TrF("buildfile_removed", {{"file", shown}});
        return;
    }
    if (m_settings.buildFileName != r.fileName) {
        m_settings.buildFileName = r.fileName;
        ConfigChanged();
    }
    note = JobNote{};
    note.tone = JobNote::Tone::Ok;
    note.text = TrF("buildfile_written", {{"file", shown}});
}

void App::TakeView() {
    if (!m_shared) return;
    std::lock_guard<std::mutex> lock(m_shared->mu);
    if (m_shared->view) m_view = m_shared->view;
}

void App::ResolveActive() {
    m_active = ActiveView{};
    m_active.build = m_store.Find(m_settings.selectedBuild);
    if (!m_active.build) return;
    // What the character's list can never hold: the class and ascendancy starts, and the ids of the
    // build's trees that the tree data does not know (an older tree version's passives). Without them
    // such a tree would never count as finished and the automatic stage would stay on it. The same
    // list and the tree data count the stage's points as the player spends them, the ascendancy apart.
    const std::vector<uint16_t> neverListed = m_tree ? NeverListedIds(*m_tree, *m_active.build) : std::vector<uint16_t>();
    m_active.stages = BuildStages(*m_active.build, &neverListed, m_tree.get());
    const CharState& c = *m_view->character;
    const std::vector<uint16_t> allocated = c.valid ? c.AllocatedIds() : std::vector<uint16_t>();
    StageContext sc;
    sc.followLevel = m_settings.autoStageByLevel;
    sc.level = c.valid ? c.level : (m_view->player.valid ? m_view->player.level : 0);
    sc.allocated = c.valid ? &allocated : nullptr;
    sc.neverListed = &neverListed;
    m_active.choice = ChooseStage(*m_active.build, m_active.stages, sc);
    m_active.sets = ResolveSets(*m_active.build, m_active.stages, m_active.choice.stage);
}

void App::RebuildDerived() {
    // Everything derived is rebuilt together when any input moved: the store / selection / settings /
    // tree and gem data (m_storeRev), the character's passives and level, the equipped items, the
    // character's skills. "Known" bits are part of the key: a list that turns from unknown to known
    // (or back) changes every verdict even when its serial did not move.
    const GameView& v = *m_view;
    const uint64_t key[4] = {
        m_storeRev,
        (v.character->serial << 1) | (v.character->valid ? 1u : 0u),
        (v.gear->itemsSerial << 2) | (v.gear->itemsKnown ? 2u : 0u) | (v.gear->found ? 1u : 0u),
        (v.skills->skillsSerial << 8) | (v.skills->skillsKnown ? 0x80u : 0u) |
            static_cast<uint64_t>(v.player.valid ? (v.player.level & 0x7F) : 0)};
    if (key[0] == m_derivedKey[0] && key[1] == m_derivedKey[1] && key[2] == m_derivedKey[2] && key[3] == m_derivedKey[3]) return;
    for (int i = 0; i < 4; ++i) m_derivedKey[i] = key[i];

    ResolveActive();
    const uint64_t planSerial = m_plan.serial + 1;
    m_plan = PlanView{};
    m_plan.serial = planSerial;
    m_gearAdvice = GearAdvice{};
    m_skillAdvice = SkillAdvice{};
    const StoredBuild* build = m_active.build;
    if (!build) return;

    const BuildSpec* spec = m_active.Spec();
    if (spec && m_tree && !m_tree->Empty()) {
        const CharState& c = *v.character;
        PlanInput in;
        in.tree = m_tree.get();
        in.target = spec;
        in.useTiers = m_settings.useTiers;
        if (in.useTiers)
            for (int i = 0; i < m_active.sets.spec; ++i) in.earlierSpecs.push_back(&build->specs[static_cast<size_t>(i)]);
        const ClassInfo* buildClass = m_tree->ClassById(spec->classId);
        const AscendancyInfo* buildAsc = m_tree->AscendancyById(spec->ascendancyId);
        // Ascendancies are compared by the nodes they use (nodesName), never by internalId: an
        // Abyssal Lich has only Lich nodes, so the game's data reads as "Witch3" for it.
        const PlanIdentity who = ResolvePlanIdentity(spec->classId, buildClass ? buildClass->name : std::string(),
                                                     buildAsc ? buildAsc->nodesName : std::string(), c.valid, c.classId,
                                                     c.className, c.ascendancy);
        in.className = who.className;
        in.ascendancy = who.ascendancy;
        m_plan.classDiffers = who.classDiffers;
        if (c.valid) {
            in.allocated = c.allocated;
            in.allocatedAttrs = c.attributes;
        }
        m_plan.charKnown = c.valid;
        m_plan.className = in.className;
        m_plan.ascendancy = in.ascendancy;
        if (buildAsc && buildAsc->nodesName == in.ascendancy) m_plan.ascendancyName = buildAsc->name;
        else if (const AscendancyInfo* a = m_tree->AscendancyByNodesName(in.ascendancy)) m_plan.ascendancyName = a->name;
        m_plan.treeOlder = !spec->treeVersion.empty() && !m_tree->treeVersion.empty() &&
                           CompareTreeVersions(spec->treeVersion, m_tree->treeVersion) < 0;
        m_plan.plan = BuildPlan(in);
        m_plan.valid = m_plan.plan.Valid();
        if (m_plan.valid) m_plan.numbers = Annotate(*m_tree, m_plan.plan);
    }
    m_gearAdvice = MatchGear(*build, m_active.sets.itemSet, v.gear.get());
    m_skillAdvice = MatchSkills(*build, m_active.sets.skillSet, v.skills.get(), m_gems.get(), m_settings.showGrantedSkills);
}

void App::UpdateWantsOverlay() {
    const bool hasBuild = m_active.build != nullptr;
    m_shared->hasBuild.store(hasBuild);
    const bool windowShown = m_skillsWindow.Shown();
    m_shared->windowShown.store(windowShown);
    const GameView& v = *m_view;
    const bool panelOpen = v.treeOpen || v.gear->panelOpen || v.skills->panelOpen;
    m_wantsOverlay.store(m_hostCompatible && v.status.inGame && hasBuild && (panelOpen || windowShown),
                         std::memory_order_relaxed);
}

void App::ReadOverlayFrame() {
    m_frame = OverlayFrame{};
    const GameView& v = *m_view;
    m_frame.table = v.tree;
    m_frame.character = v.character;
    m_frame.gear = v.gear;
    m_frame.skills = v.skills;
    if (!m_started || !m_game || !m_readsGame) return;
    if (!m_demo && !m_ctx) return;
    m_frame.inGame = v.status.inGame;
    // Without the host's overlay DrawUI runs inside the menu window: nothing is drawn over the game,
    // so nothing of the game is read either. A demo has no host overlay: it counts as running.
    m_frame.overlayActive = m_demo || m_ctx->Game.IsOverlayMode();
    m_frame.menuVisible = !m_demo && m_ctx->Game.IsMenuVisible();
    m_frame.clientW = v.status.clientW;
    m_frame.clientH = v.status.clientH;
    if (!m_frame.inGame) return;

    if (m_demo) {
        m_frame.mouse = m_demoMouse;
        m_frame.mouseValid = m_demoMouseSet;
    } else {
        // The cursor in game client pixels: works while the overlay is click-through.
        POINT p{};
        const HWND window = m_ctx->Game.GetGameWindow();
        if (window && GetCursorPos(&p) && ScreenToClient(window, &p)) {
            m_frame.mouse = {static_cast<float>(p.x), static_cast<float>(p.y)};
            m_frame.mouseValid = true;
        }
    }
    m_frame.mouseValid = m_frame.mouseValid && m_frame.mouse.x >= 0 && m_frame.mouse.y >= 0 &&
                         m_frame.mouse.x < m_frame.clientW && m_frame.mouse.y < m_frame.clientH;
    m_frame.treeOpen = v.treeOpen;
    m_frame.inventoryOpen = v.gear->panelOpen;
    m_frame.skillsOpen = v.skills->panelOpen;
    if (!m_active.build || !m_frame.overlayActive) return;   // nothing is drawn, so nothing is read

    // The only game reads on the render thread (CLAUDE.md "Threads").
    // The watcher's "open" flags are a period old (16 ms while a panel is open, plus its tick's own
    // reads): for a frame or more after the player closes a panel they still say open. This frame's
    // own read of the panel's visible bit wins, so no ring, mark or card is drawn where a closed panel
    // was (fail closed: an element that cannot be read counts as closed).
    // The plan's node indices and the table's are one index space only when both use the same tree data.
    if (m_frame.treeOpen && v.tree->found && v.tree->tree == m_tree && m_settings.showTree) {
        m_frame.pose = ReadTreePose(*m_game, *v.tree);
        if (!m_frame.pose.panelVisible) m_frame.treeOpen = false;
    }
    const bool gearHints = m_settings.showGearCard || m_settings.showSlotMarks;
    const bool skillHints = m_settings.showSkillMarks || m_settings.showMissingSkills;
    // One element read per panel, and only for a panel whose frame is read below or whose warning
    // line could show.
    const auto stillOpen = [this](uint64_t panel) {
        if (panel == 0) return false;
        const UiElem e = m_game->UiRead(panel);
        return e.valid && e.visible;
    };
    // The warning lines too: a table with a problem that still knows its panel (a data mismatch, no
    // slots) is asked the same way. With no panel address at all the watcher's flag stands, so the
    // "panel not found" warning of the tree can show.
    if (m_frame.treeOpen && m_settings.showTree && !v.tree->found && v.tree->panel != 0 && !stillOpen(v.tree->panel))
        m_frame.treeOpen = false;
    if (m_frame.skillsOpen && skillHints && !stillOpen(v.skills->panel)) m_frame.skillsOpen = false;
    if (m_frame.inventoryOpen && (v.gear->found || v.gear->panel != 0) && (gearHints || (skillHints && m_frame.skillsOpen)) &&
        !stillOpen(v.gear->panel))
        m_frame.inventoryOpen = false;
    // The Skills overlay places its cards by the inventory panel's rect (GearFrame::panel) while both
    // panels are open, so the gear frame is read for it too, also with the gear hints off.
    if (m_frame.inventoryOpen && v.gear->found && (gearHints || (skillHints && m_frame.skillsOpen)))
        m_frame.gearFrame = ReadGearFrame(*m_game, *v.gear, m_frame.mouse, m_frame.mouseValid);
    if (m_frame.skillsOpen && skillHints) m_frame.skillsFrame = ReadSkillsFrame(*m_game, *v.skills);
}

// ---------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------

void App::ConfigChanged() {
    m_settingsFile.Changed(NowMs());
    ++m_storeRev;
}

void App::ResetDisplay() {
    ResetDisplaySettings(m_settings);
    ConfigChanged();
}

void App::StoreChanged() {
    m_storeFile.Changed(NowMs());
    ++m_storeRev;
    // Pointers into the store may have moved: resolve again before anyone reads m_active.
    InvalidateDerived();
}

void App::InvalidateDerived() {
    for (uint64_t& k : m_derivedKey) k = ~0ull;
    if (m_started) RebuildDerived();
    else m_active = ActiveView{};
}

void App::SelectBuild(const std::string& id) {
    if (!id.empty() && !m_store.Find(id)) return;
    if (m_settings.selectedBuild == id) return;
    m_settings.selectedBuild = id;
    ConfigChanged();
    InvalidateDerived();
    KickReaders();
}

void App::DeleteBuild(const std::string& id) {
    m_active = ActiveView{};   // it may point at the build that goes away
    const bool removed = m_store.Remove(id);
    if (removed && m_settings.selectedBuild == id) {
        m_settings.selectedBuild.clear();
        ConfigChanged();
    }
    if (removed && m_lastImportedId == id) m_lastImportedId.clear();
    if (removed) {
        StoreChanged();
    } else {
        InvalidateDerived();
    }
}

bool App::RenameBuild(const std::string& id, const std::string& name) {
    if (!m_store.Rename(id, name)) return false;
    StoreChanged();
    return true;
}

void App::PinStage(int spec) {
    StoredBuild* b = m_store.Find(m_settings.selectedBuild);
    if (!b) return;
    const int pin = (spec >= 0 && spec < static_cast<int>(b->specs.size())) ? spec : -1;
    if (b->selectedSpec == pin) return;
    b->selectedSpec = pin;
    StoreChanged();
}

void App::PinSkillSet(int skillSet) {
    StoredBuild* b = m_store.Find(m_settings.selectedBuild);
    if (!b) return;
    const int pin = (skillSet >= 0 && skillSet < static_cast<int>(b->skillSets.size())) ? skillSet : -1;
    if (b->selectedSkillSet == pin) return;
    b->selectedSkillSet = pin;
    StoreChanged();
}

void App::PinItemSet(int itemSet) {
    StoredBuild* b = m_store.Find(m_settings.selectedBuild);
    if (!b) return;
    const int pin = (itemSet >= 0 && itemSet < static_cast<int>(b->itemSets.size())) ? itemSet : -1;
    if (b->selectedItemSet == pin) return;
    b->selectedItemSet = pin;
    StoreChanged();
}

void App::ToggleSkillsWindow() {
    m_skillsWindow.Toggle();
    if (m_started) UpdateWantsOverlay();   // the host asks before the next Pump: the window may be all there is to draw
}

void App::CloseSkillsWindow() {
    m_skillsWindow.Close();
    if (m_started) UpdateWantsOverlay();
}

// ---------------------------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------------------------

void App::LogInfo(const std::string& line) {
    if (m_ctx && m_hostCompatible) m_ctx->Log.Info(("Build Planner: " + line).c_str());
}

void App::LogProblem(const std::string& line) {
    if (m_ctx && m_hostCompatible) m_ctx->Log.Warn(("Build Planner: " + line).c_str());
    // The same line again and again (a file that cannot be saved) must not push the others out.
    if (!m_problems.empty() && m_problems.back() == line) return;
    if (m_problems.size() >= kMaxProblems) m_problems.erase(m_problems.begin());
    m_problems.push_back(line);
}

void App::KickReaders() {
    if (!m_started || !m_shared) return;
    {
        std::lock_guard<std::mutex> lock(m_shared->mu);
        m_shared->kick = true;
    }
    m_shared->watchCv.notify_all();
}

std::string App::DiagnosticsText() const {
    const GameView& v = *m_view;
    std::string s = "Build Planner diagnostics\n";
    const auto line = [&s](const std::string& key, const std::string& value) { s += key + ": " + value + "\n"; };
    const auto num = [](long long n) { return std::to_string(n); };

    line("host", std::string(m_hostCompatible ? "compatible" : "NOT compatible") + (m_demo ? ", demo mode" : ""));
    line("game", std::string("attached ") + YesNo(v.status.attached) + ", in game " + YesNo(v.status.inGame) + ", client " +
                     num(static_cast<long long>(v.status.clientW)) + "x" + num(static_cast<long long>(v.status.clientH)) +
                     ", area serial " + num(static_cast<long long>(v.status.areaSerial)));
    line("player", v.player.valid ? "level " + num(v.player.level) + ", " + v.player.path : std::string("unknown"));

    // The data.
    if (m_treeInfo.loaded) {
        line("tree data", m_treeInfo.from + ", version " + m_treeInfo.treeVersion + ", " + num(m_treeInfo.nodes) + " nodes" +
                              (m_treeInfo.source.empty() ? std::string() : ", source " + m_treeInfo.source) +
                              (m_treeInfo.commit.empty() ? std::string() : ", commit " + m_treeInfo.commit) +
                              (m_treeInfo.fileTime ? ", file time " + num(m_treeInfo.fileTime) : std::string()));
    } else {
        line("tree data", m_treeInfo.loading ? "loading" : "not loaded");
    }
    if (!m_treeInfo.error.empty()) line("tree data error", m_treeInfo.error);
    line("gem data", m_gems ? num(static_cast<long long>(m_gems->Size())) + " gems" : std::string("not loaded"));

    // The four readers.
    const TreeTable& t = *v.tree;
    line("tree panel", std::string("open ") + YesNo(v.treeOpen) + ", found " + YesNo(t.found) + ", problem " + Name(t.problem) +
                           ", nodes " + num(static_cast<long long>(t.nodes.size())) + ", candidates " + num(t.candidates) +
                           ", resolved " + num(t.resolved) + ", coverage " + num(static_cast<long long>(t.coverage * 100.0f + 0.5f)) +
                           "%, via " + (t.viaShortcut ? "the slot" : "the structure") +
                           (t.ascendancy.empty() ? std::string() : ", ascendancy " + t.ascendancy) +
                           (t.found && t.tree != m_tree ? ", OTHER tree data than the plan's" : ""));
    const CharState& c = *v.character;
    line("character", std::string("valid ") + YesNo(c.valid) + ", problem " + Name(c.problem) + ", level " + num(c.level) +
                          ", class " + (c.className.empty() ? "?" : c.className) + ", ascendancy " +
                          (c.ascendancy.empty() ? "-" : c.ascendancy) + ", allocated " + num(static_cast<long long>(c.allocated.size())) +
                          " (main " + num(c.mainAllocated) + ", ascendancy " + num(c.ascAllocated) + ", set I " + num(c.set1Count) +
                          ", set II " + num(c.set2Count) + "), free " + num(c.freePoints) + " / set " + num(c.freeSet1) + " " +
                          num(c.freeSet2) + " / ascendancy " + num(c.freeAscPoints) + ", quest points " + num(c.questPoints) +
                          ", unknown ids " + num(c.unknownIds) + ", via scan " + YesNo(c.psdViaScan || c.objectViaScan) +
                          ", tree cross-check " + (c.uiChecked ? num(c.uiDiffers) + " differ" : std::string("not run")));
    const GearState& g = *v.gear;
    line("equipment", std::string("found ") + YesNo(g.found) + ", problem " + Name(g.problem) + ", inventory open " +
                          YesNo(g.panelOpen) + ", slots bound " + num(g.boundSlots) + ", items known " + YesNo(g.itemsKnown));
    const SkillsState& k = *v.skills;
    line("skills", std::string("problem ") + Name(k.problem) + ", skills known " + YesNo(k.skillsKnown) + " (" +
                       num(static_cast<long long>(k.skills.size())) + "), panel found " + YesNo(k.panelFound) + ", built " +
                       YesNo(k.panelBuilt) + ", open " + YesNo(k.panelOpen) + ", blocks " + num(static_cast<long long>(k.blocks.size())));

    // The selection and what follows from it.
    line("builds", num(static_cast<long long>(m_store.builds.size())) + " stored" +
                       (m_store.PendingFileVerdict() == StoreFileVerdict::Unusable ? ", builds.json was NOT usable at the start (kept, moved aside at the next save)"
                        : m_store.PendingFileVerdict() == StoreFileVerdict::Newer  ? ", builds.json is from a NEWER plugin (kept, moved aside at the next save)"
                                                                                    : ""));
    if (!m_storeFile.Error().empty()) line("builds.json not saved", m_storeFile.Error());
    if (!m_settingsFile.Error().empty()) line("settings.json not saved", m_settingsFile.Error());
    if (const StoredBuild* b = m_active.build) {
        line("build", b->className + (b->ascendName.empty() ? std::string() : " / " + b->ascendName) + ", level " + num(b->level) +
                          ", trees " + num(static_cast<long long>(b->specs.size())) + ", skill sets " +
                          num(static_cast<long long>(b->skillSets.size())) + ", gear sets " +
                          num(static_cast<long long>(b->itemSets.size())) + ", items " + num(static_cast<long long>(b->items.size())));
        line("stage", num(m_active.choice.stage + 1) + " of " + num(static_cast<long long>(m_active.stages.size())) + " (" +
                          Name(m_active.choice.how) + "), skill set " + num(m_active.sets.skillSet + 1) +
                          (m_active.sets.skillSetPinned ? " (pinned)" : "") + ", gear set " + num(m_active.sets.itemSet + 1) +
                          (m_active.sets.itemSetPinned ? " (pinned)" : ""));
        if (const BuildSpec* spec = m_active.Spec())
            line("stage tree", "version " + (spec->treeVersion.empty() ? std::string("?") : spec->treeVersion) + ", class id " +
                                   num(spec->classId) + ", ascendancy " + (spec->ascendancyId.empty() ? "-" : spec->ascendancyId) +
                                   ", nodes " + num(static_cast<long long>(spec->nodes.size())));
    } else {
        line("build", "none selected");
    }
    if (m_plan.valid) {
        const ::bp::Plan& p = m_plan.plan;   // App::Plan() hides the type here
        line("plan", "take " + num(p.missing) + ", refund " + num(p.wrong) + ", adjust " + num(p.adjust) + ", ok " + num(p.ok) +
                         ", ascendancy take " + num(p.ascMissing) + " refund " + num(p.ascWrong) + ", unreachable " +
                         num(p.unreachable) + ", unknown build ids " + num(static_cast<long long>(p.unknownTarget.size())) +
                         ", unknown character ids " + num(static_cast<long long>(p.unknownAllocated.size())) + ", against the character " +
                         YesNo(m_plan.charKnown) + ", class differs " + YesNo(m_plan.classDiffers) + ", build tree older " +
                         YesNo(m_plan.treeOlder));
    } else {
        line("plan", "none");
    }
    line("gear advice", "set " + num(m_gearAdvice.itemSet + 1) + ", with item " + num(m_gearAdvice.withItem) + ", matching " +
                            num(m_gearAdvice.matching));
    line("skill advice", "set " + num(m_skillAdvice.skillSet + 1) + ", groups " + num(static_cast<long long>(m_skillAdvice.groups.size())) +
                             ", missing " + num(static_cast<long long>(m_skillAdvice.missing.size())) + ", character known " +
                             YesNo(m_skillAdvice.charKnown));

    // Offsets: the files read and every value that is not the built-in one.
    line("offsets", std::string("data file ") + (m_offsets.dataFileRead ? "read" : "not read") + ", config file " +
                        (m_offsets.configFileRead ? "read" : "none"));
    const std::span<const OffsetField> fields = OffsetFields();
    for (size_t i = 0; i < fields.size() && i < m_offsets.source.size(); ++i) {
        const uint32_t value = m_offsets.values.*(fields[i].member);
        if (m_offsets.source[i] == OffsetSource::ConfigFile || value != fields[i].builtIn)
            line(std::string("offset ") + fields[i].name,
                 OffsetHex(value) + (m_offsets.source[i] == OffsetSource::ConfigFile ? " (config file)" : " (data file)") +
                     ", built in " + OffsetHex(fields[i].builtIn));
    }
    for (const std::string& w : m_offsets.warnings) line("offsets warning", w);

    for (size_t i = 0; i < kJobKindCount; ++i)
        if (!m_notes[i].detail.empty()) line("last job problem", m_notes[i].detail);
    line("plugin folder", PathUtf8(m_pluginDir));
    for (const std::string& p : m_problems) line("problem", p);
    return s;
}

} // namespace bp
