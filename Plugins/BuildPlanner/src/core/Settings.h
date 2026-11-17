#pragma once
// BuildPlanner - settings.json
// Settings.h - the plugin's settings (<plugin>/config/settings.json) as a plain struct with JSON text
// conversion, and two small state machines the App runs on them (SkillsWindowState, WriteBehind).
// Pure, no SDK, no ImGui.
//
// settings.json: one flat JSON object; keys are the member names below; colours are the object
// "colors" { "missing", "next", "wrong", "adjust", "ok" } as "#RRGGBBAA". The file is untrusted (hand
// edits): unknown keys are ignored and every value of the wrong type or out of range is replaced by
// its default (named in `warnings`).
//
// Threads: the Settings object belongs to the render thread (the App). The watcher and the job thread
// never read it: the App hands them the few values they need.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "Types.h"

namespace bp {

// When the "Build skills" info window shows by itself. Never = only when the user opens
// it (the hotkey, or the button of the Display tab): the default, because with the game's Skills
// panel open the socket marks and the card column already say the same.
enum class SkillsWindowMode : uint8_t { Always = 0, WithSkillsPanel = 1, Never = 2 };

// The "Build skills" window, shown or not (the App owns one; pure, so the rule is unit-tested).
// The mode gives its own answer; the hotkey, the Display tab's button and the window's own close
// flip that answer, and the flip lasts until the mode's answer changes by itself (the Skills panel
// opens or closes, the mode is changed). Without a followed build the window never shows and a
// toggle does nothing (it would flip a state nobody sees).
struct SkillsWindowState {
    // Once per App::Pump(). `toggle`: the hotkey went down since the last call. Returns Shown().
    bool Update(SkillsWindowMode mode, bool skillsPanelOpen, bool hasBuild, bool toggle);
    void Toggle();                       // the button (applies at once, like the hotkey)
    void Close();                        // the window's own menu: hides it when it shows
    bool Shown() const { return m_shown; }

private:
    bool m_natural = false, m_flip = false, m_hasBuild = false, m_shown = false;
};

// Write-behind state of one of the plugin's files (settings.json, builds.json). A change is saved
// once kSaveDebounceMs passed without another one; a save that failed is tried again every
// kSaveRetryMs for as long as the plugin runs (a full disk or a locked file must not lose what the
// user did), and its error stays for the settings page until a save works.
inline constexpr int64_t kSaveDebounceMs = 1000;
inline constexpr int64_t kSaveRetryMs = 10000;
struct WriteBehind {
    void Changed(int64_t nowMs);                    // there is something new to save
    bool Due(int64_t nowMs, bool force) const;      // save now? `force`: SaveSettings / Stop
    void Saved();                                   // the save worked
    void Failed(int64_t nowMs, std::string why);    // it did not: `why` = the writer's error (English)
    bool Dirty() const { return m_dirty; }
    const std::string& Error() const { return m_error; }   // "" = the last save worked, or none was needed

private:
    bool m_dirty = false;
    int64_t m_dueMs = 0;
    std::string m_error;
};

struct Settings {
    // Ranges (the Display tab's sliders use them; FromJson clamps to them).
    static constexpr int kNumbersMin = 0, kNumbersMax = 30;
    static constexpr float kLineWidthMin = 0.0f, kLineWidthMax = 8.0f;
    static constexpr float kRingScaleMin = 0.5f, kRingScaleMax = 2.0f;
    static constexpr float kCardOpacityMin = 0.3f, kCardOpacityMax = 1.0f;

    // ---- selection ----
    std::string selectedBuild;            // a BuildStore id, "" = none
    bool autoStageByLevel = true;         // "Follow my level": automatic stage by the level range in the titles
    bool useTiers = true;                 // "Earlier trees first": objectives an earlier tree already had come first

    // ---- passive tree overlay ----
    bool showTree = true;                 // the whole tree overlay
    int numbersShown = 8;                 // step numbers drawn (0..30)
    float lineWidth = 2.5f;               // px at font size 13 (0..8)
    float ringScale = 1.0f;               // x the node's own size (0.5..2)
    bool showOk = false;                  // rings on nodes that are allocated and wanted
    bool showWrong = true;                // rings on nodes to refund
    bool showAdjust = true;               // rings on nodes taken with another weapon set / attribute
    bool showLines = true;
    bool curvedLines = true;              // arcs along an orbit, the way the game draws them
    bool showSummary = true;              // the summary box at the top left of the tree
    bool showTreeTooltip = true;          // our tooltip on a ring
    bool showAscendancy = true;           // rings on ascendancy nodes
    bool showSetBadges = true;            // "I" / "II" badges on weapon set nodes
    bool showAttrBadges = true;           // S / D / I badges on attribute nodes
    uint32_t colorMissing = Rgba(0x46, 0xDC, 0x5A, 235);
    uint32_t colorNext = Rgba(0x3C, 0xE1, 0xFF, 255);
    uint32_t colorWrong = Rgba(0xFF, 0x46, 0x3C, 235);
    uint32_t colorAdjust = Rgba(0xFF, 0xA0, 0x28, 235);
    uint32_t colorOk = Rgba(0xC8, 0xAA, 0x5A, 130);

    // ---- gear hints ----
    bool showGearCard = true;             // the "build item" card on a hovered slot
    bool showSlotMarks = true;            // the corner marks on the slots
    float cardOpacity = 0.92f;            // background opacity of every card drawn on the game (0.3..1)

    // ---- skills ----
    bool showSkillMarks = true;           // marks on the Skills panel's support sockets + the "Add:" line
    bool showMissingSkills = true;        // the "Missing skills" box above the panel's list
    bool showGrantedSkills = false;       // include skills granted by items / passives / default attacks
    SkillsWindowMode skillsWindowMode = SkillsWindowMode::Never;
    int skillsWindowHotkey = 0;           // virtual-key code that toggles the window, 0 = none

    // ---- data ----
    std::string treeSource = "bundled";   // "bundled" (data/tree.json) | "downloaded" (config/tree.json)
    std::string treeDownloadedVersion;    // the version folder config/tree.json came from ("0_6"); the
                                          // file itself names none, so LoadTreeFile gets this as its hint.
                                          // Only [0-9_], at most 16 characters; "" = unknown

    // ---- the game's own Build Planner ----
    bool buildFileKeepUpdated = false;    // rewrite the .build file whenever the stage in effect changes
    std::string buildFileName;            // the file last written ("" none): what "Remove" removes

    bool operator==(const Settings&) const = default;
};

// The display part (everything under "passive tree overlay", "gear hints" card opacity and marks,
// the skills toggles except the hotkey) back to its defaults; selection, data and file fields kept.
void ResetDisplaySettings(Settings& s);

// "#RRGGBBAA" (upper case) from an IM_COL32-layout colour.
std::string ColorToHex(uint32_t color);
// Accepts "#RRGGBBAA" and "#RRGGBB" (alpha 255), either case. false (and `out` untouched) otherwise.
bool ColorFromHex(std::string_view text, uint32_t& out);

// settings.json text (2-space indent).
std::string SettingsToJsonText(const Settings& s);
// Never throws. Text that is not a JSON object gives the defaults and one warning. `warnings`
// (optional, English) names every key whose value was replaced by its default.
Settings SettingsFromJsonText(std::string_view json, std::vector<std::string>* warnings = nullptr);

// Reads <configDir>/settings.json (at most 1 MiB). A missing file gives the defaults silently; an
// unreadable or malformed one gives the defaults and a warning. Never throws.
Settings LoadSettings(const std::filesystem::path& file, std::vector<std::string>* warnings = nullptr);
// Writes it atomically (JsonFile.h). false with `error` (English, may be null) on failure.
bool SaveSettings(const std::filesystem::path& file, const Settings& s, std::string* error);

} // namespace bp
