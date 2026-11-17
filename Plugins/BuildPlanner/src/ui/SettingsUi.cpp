// BuildPlanner - the settings page
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// SettingsUi.cpp - the settings page: the tab bar (Builds | Passives | Gear | Skills |
// Display); every tab's page ends with the Diagnostics card.
//
// Exceptions: everything below draws through the RAII scopes of SettingsCommon.h, so an exception
// (out of memory while a text is built) unwinds every Begin / Push it passed; the catch here is then
// at a point where the ImGui stacks are balanced, and the page simply misses the rest of that frame.
#include "SettingsCommon.h"
#include "Ui.h"

namespace bp {
namespace {

// One tab: its page, then the Diagnostics card that closes every page (the kit's rule).
// `name` is the tab's name in demo.json ("tab": "gear"), which lets a preview tool open any tab.
void Tab(const char* label, const char* name, void (*draw)(App&), App& app) {
    const bool select = app.DemoMode() && app.DemoTab() == name;
    const sui::TabItem tab(label, select ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None);
    if (!tab) return;
    ImGui::Spacing();
    draw(app);
    ImGui::Spacing();
    DrawDiagnostics(app);
}

} // namespace

void DrawSettingsPage(App& app) {
    try {
        const sui::Id id("BuildPlannerSettings");
        if (!app.HostCompatible()) {
            sui::WarnLine(I18n::Tr("settings_incompatible"));
            ImGui::Spacing();
        }
        // A file that cannot be saved concerns every tab: what the user changes is lost when
        // POEFixer closes unless a later try works (App retries on its own).
        if (!app.StoreSaveError().empty()) {
            sui::ErrorLine(I18n::TrF("save_failed_builds", {{"detail", app.StoreSaveError()}}));
            ImGui::Spacing();
        }
        if (!app.SettingsSaveError().empty()) {
            sui::ErrorLine(I18n::TrF("save_failed_settings", {{"detail", app.SettingsSaveError()}}));
            ImGui::Spacing();
        }
        if (const sui::TabBar bar{"##bpTabs"}) {
            Tab(I18n::TrId("tab_builds"), "builds", &DrawTabBuilds, app);
            Tab(I18n::TrId("tab_passives"), "passives", &DrawTabPassives, app);
            Tab(I18n::TrId("tab_gear"), "gear", &DrawTabGear, app);
            Tab(I18n::TrId("tab_skills"), "skills", &DrawTabSkills, app);
            Tab(I18n::TrId("tab_display"), "display", &DrawTabDisplay, app);
        }
    } catch (...) {
    }
}

} // namespace bp
