#pragma once
// BuildPlanner - the draw entry points
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// Ui.h - the entry points of everything the plugin draws, and which file implements each.
//
// Rules for every function here:
//   - RENDER thread only. They read the App through its accessors and call its action methods; they
//     never read the game (App::ReadOverlayFrame did it) and never block.
//   - Balance every Begin / End and Push / Pop on every path, and let no exception out: a draw
//     function catches everything at a point where the ImGui stacks are balanced.
//   - DrawUI and DrawSettings run in different ImGui contexts (separate menu window): UI-only state
//     (text buffers, the row being renamed) lives in file-static variables of the .cpp that needs it,
//     never ImGui ids, window pointers or frame counts.
//   - Every user-visible text goes through PluginSDK::I18n (Tr / TrId / TrF) with a key of
//     lang/en.json; game names (passives, gems, items) stay as the build has them. No translated
//     text in a static. Sizes derive from ImGui::GetFontSize() and measured text.
//   - No ImGui::BeginPopupModal and no OS dialog (PluginUi::ConfirmPopup instead).
#include <imgui.h>

#include "../../../../POEFixer/plugin_sdk/PluginSDK.h"
#include "../../../../POEFixer/imgui/IconsFontAwesome6.h"
#include "../../../../POEFixer/plugin_sdk/PluginUi.h"

#include "../core/PobImport.h"
#include "../host/App.h"

namespace bp {

namespace I18n = PluginSDK::I18n;   // I18n::Tr / TrId / TrF / Key

// ---- overlays over the game: DrawUI, after App::Pump() and App::ReadOverlayFrame() ----
// Each one returns at once when its part is off, no build is selected or its panel is closed. They
// draw on `OverlayDrawList(app)` in game client pixels, and ONLY while OverlayFrame::overlayActive:
// without the host's overlay DrawUI runs inside the menu window. The three draw-list overlays gate on
// OverlayFrame::DrawTree() / DrawGear() / DrawSkills(); the tree overlay shows one warning line when
// OverlayFrame::TreeWarning() names a problem (fail closed: no rings without the character's passives).
void DrawTreeOverlay(App& app);     // ui/OverlayTree.cpp   - rings, numbers, lines, badges, summary, tooltip
void DrawGearOverlay(App& app);     // ui/OverlayGear.cpp   - slot marks and the build item card
void DrawSkillsOverlay(App& app);   // ui/OverlaySkills.cpp - socket marks, "Add:" chips, "Missing skills"
void DrawSkillsWindow(App& app);    // ui/SkillsWindow.cpp  - the "Build skills" info window;
                                    //                        the only one that opens a window (PluginSDK::InfoWindow)

// ---- the settings page: DrawSettings, after App::Pump() ----
void DrawSettingsPage(App& app);    // ui/SettingsUi.cpp    - the tab bar; calls the tab functions below, each
                                    //                        followed by DrawDiagnostics
void DrawTabBuilds(App& app);       // ui/TabBuilds.cpp     - import row, saved builds, stages, .build card
void DrawTabPassives(App& app);     // ui/TabPassives.cpp   - the plan: counts, next objectives, refunds
void DrawTabGear(App& app);         // ui/TabGear.cpp       - the gear set, slot by slot
void DrawTabSkills(App& app);       // ui/TabSkills.cpp     - the skill set: groups, levels, supports
void DrawTabDisplay(App& app);      // ui/TabDisplay.cpp    - toggles, sliders, colours, the skills window mode and hotkey
void DrawDiagnostics(App& app);     // ui/Diagnostics.cpp   - the closed "Diagnostics" card that ends every tab

// ---- texts shared by the overlays and the settings page (ui/Diagnostics.cpp) ----
// One sentence per problem code, in the program language: it names the cause and what to do. The
// pointer stays valid for the DLL's lifetime (I18n::Tr). None -> "".
const char* TreeProblemText(TreeProblem p);
const char* CharProblemText(CharProblem p);
const char* GearProblemText(GearProblem p);
const char* SkillsProblemText(SkillsProblem p);
// The display name of an equipment slot ("Main hand", "Charm 2") in the program language: the ONE
// place slot names come from, for the Gear tab and the build item card alike. Keys
// slot_* of lang/en.json. Count / out of range -> "".
const char* GearSlotText(GearSlot slot);
// The red line of a failed import: the cause and the fix, with `detail` (ImportResult::detail, English)
// in brackets where it helps. None -> "". App.cpp uses it for the Import job's JobNote.
std::string ImportProblemText(ImportProblem problem, const std::string& detail);

// The draw list of the overlays this frame: the background list while the program's menu is shown in
// the overlay (so hints stay behind the menu), else the foreground list.
inline ImDrawList* OverlayDrawList(const App& app) {
    return app.Frame().menuVisible ? ImGui::GetBackgroundDrawList() : ImGui::GetForegroundDrawList();
}

} // namespace bp
