// BuildPlanner - the plugin entry
// BP_TESTS_EXCLUDE: the plugin entry (ImGui + the plugin SDK); not built into tests/.
// Plugin.cpp - the PluginSDK::Plugin subclass, the plugin info and the factory exports. It only
// forwards the host's callbacks to bp::App (host/App.h) and to the draw functions of ui/Ui.h.
//
// No C++ exception leaves the plugin: every entry runs inside Guard. (The draw functions themselves
// catch at points where the ImGui stacks are balanced; Guard is the last line of defence.)
#include <imgui.h>

#include "../../../../POEFixer/plugin_sdk/PluginSDK.h"
#include "lang_en.generated.h"   // the embedded English texts: once, after PluginSDK.h

#include "../ui/Ui.h"
#include "App.h"

namespace {

class BuildPlannerPlugin final : public PluginSDK::Plugin {
public:
    const char* GetName() const override { return "BuildPlanner"; }

    void OnEnable(bool /*gameAttached*/) override {
        Guard([this] { m_app.Start(ctx(), DirectoryPath(), HostCompatible()); });
    }
    // Stops and joins the plugin's threads: hot reload unloads the DLL right after.
    void OnDisable() override {
        Guard([this] { m_app.Stop(); });
    }
    void SaveSettings() override {
        Guard([this] { m_app.SaveNow(); });
    }
    bool WantsOverlay() const override { return m_app.WantsOverlay(); }

    // Every frame, in the overlay's ImGui context (or the menu window's while no overlay runs).
    void DrawUI() override {
        if (!Bind()) return;
        Guard([this] {
            m_app.Pump();
            m_app.ReadOverlayFrame();
            bp::DrawTreeOverlay(m_app);
            // Every frame and BEFORE the Skills overlay: it resets and records this frame's build
            // item card (theme::HoverCardBox), which the Skills overlay keeps its socket marks off.
            bp::DrawGearOverlay(m_app);
            bp::DrawSkillsOverlay(m_app);
            bp::DrawSkillsWindow(m_app);
        });
    }
    // In the settings window's ImGui context (a different one with "Separate menu window").
    void DrawSettings() override {
        if (!Bind()) return;
        Guard([this] {
            m_app.Pump();
            bp::DrawSettingsPage(m_app);
        });
    }

private:
    bool Bind() const {
        if (!ctx()->ImGuiContext) return false;
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx()->ImGuiContext));
        return true;
    }
    template <class F>
    static void Guard(F&& f) noexcept {
        try {
            f();
        } catch (...) {
        }
    }

    bp::App m_app;   // its destructor stops the threads too
};

} // namespace

extern "C" PLUGIN_API PluginSDK::Plugin* CreatePlugin() {
    try {
        return new BuildPlannerPlugin();
    } catch (...) {
        return nullptr;
    }
}
extern "C" PLUGIN_API void DestroyPlugin(PluginSDK::Plugin* p) { delete p; }

// Plugin info: the host checks it before CreatePlugin; the publish tool reads it.
POEFIXER_PLUGIN_INFO(
    "BuildPlanner",
    "Build Planner",
    "1.0.0",
    "POEFixer",
    "Shows a Path of Building build in the game: the next passive points on the tree, the recommended item on each equipment slot and the skills with their support gems.")
