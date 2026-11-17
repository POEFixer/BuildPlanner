// BuildPlanner - the Display tab
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// TabDisplay.cpp - the Display tab: every overlay part as a toggle, the sliders, the
// state colours with their resets, the "Build skills" window's mode and hotkey, and the reset of
// the whole tab. A control that depends on another one is dimmed while that one is off, never hidden.
#include "SettingsCommon.h"
#include "Theme.h"
#include "Ui.h"

namespace bp {
namespace {

using I18n::Tr;
using I18n::TrF;
using I18n::TrId;

// One checkbox bound to a setting; returns true when it changed.
bool Toggle(const char* labelKey, const char* id, bool& value, const char* tipKey) {
    return PluginUi::Check(Tr(labelKey), id, &value, Tr(tipKey));
}

// A block of checkboxes in two columns when the page is wide enough, else stacked. `draw(i)` draws
// the i-th of `count` checkboxes and returns true when it changed. The columns are filled top-down
// (first half left), so the reading order is the same as when stacked.
template <class Draw>
bool Checks(const char* id, int count, Draw draw) {
    bool changed = false;
    if (count > 3 && PluginUi::WideEnoughForTwoColumns()) {
        if (const sui::Table table{id, 2, ImGuiTableFlags_SizingStretchSame}) {
            const int half = (count + 1) / 2;
            for (int row = 0; row < half; ++row) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                changed |= draw(row);
                if (row + half < count) {
                    ImGui::TableSetColumnIndex(1);
                    changed |= draw(row + half);
                }
            }
            return changed;
        }
    }
    for (int i = 0; i < count; ++i) changed |= draw(i);
    return changed;
}

bool TreeCard(Settings& s) {
    bool changed = false;
    const sui::Card card("##bp_display_tree");
    PluginUi::Heading(ICON_FA_DIAGRAM_PROJECT, Tr("display_tree_title"), Tr("display_tree_help"));
    changed |= Toggle(I18n::Key("display_show_tree"), "bpShowTree", s.showTree, I18n::Key("display_show_tree_tip"));

    const sui::Disabled off(!s.showTree);
    changed |= Checks("##bpTreeChecks", 10, [&s](int i) {
        switch (i) {
        case 0: return Toggle(I18n::Key("display_summary"), "bpSummary", s.showSummary, I18n::Key("display_summary_tip"));
        case 1: return Toggle(I18n::Key("display_tooltip"), "bpTreeTip", s.showTreeTooltip, I18n::Key("display_tooltip_tip"));
        case 2: return Toggle(I18n::Key("display_lines"), "bpLines", s.showLines, I18n::Key("display_lines_tip"));
        case 3: {
            // Curved lines are a way to draw the lines: nothing to curve while they are off.
            const sui::Disabled noLines(!s.showLines);
            return Toggle(I18n::Key("display_curved"), "bpCurved", s.curvedLines, I18n::Key("display_curved_tip"));
        }
        case 4: return Toggle(I18n::Key("display_wrong"), "bpWrong", s.showWrong, I18n::Key("display_wrong_tip"));
        case 5: return Toggle(I18n::Key("display_adjust"), "bpAdjust", s.showAdjust, I18n::Key("display_adjust_tip"));
        case 6: return Toggle(I18n::Key("display_ok"), "bpOk", s.showOk, I18n::Key("display_ok_tip"));
        case 7: return Toggle(I18n::Key("display_ascendancy"), "bpAsc", s.showAscendancy, I18n::Key("display_ascendancy_tip"));
        case 8: return Toggle(I18n::Key("display_set_badges"), "bpSetBadges", s.showSetBadges, I18n::Key("display_set_badges_tip"));
        default: return Toggle(I18n::Key("display_attr_badges"), "bpAttrBadges", s.showAttrBadges, I18n::Key("display_attr_badges_tip"));
        }
    });

    ImGui::Spacing();
    const PluginUi::LabelColumn col({Tr("display_numbers"), Tr("display_line_width"), Tr("display_ring_scale")});
    changed |= PluginUi::SliderIntRow(col, Tr("display_numbers"), "##bpNumbers", &s.numbersShown, Settings::kNumbersMin,
                                      Settings::kNumbersMax, "%d", Tr("display_numbers_tip"));
    // The width also sets the rings' outline (OverlayTree baseThick), so it stays usable while the
    // lines are off.
    changed |= PluginUi::SliderFloatRow(col, Tr("display_line_width"), "##bpLineWidth", &s.lineWidth, Settings::kLineWidthMin,
                                        Settings::kLineWidthMax, "%.1f", Tr("display_line_width_tip"));
    changed |= PluginUi::SliderFloatRow(col, Tr("display_ring_scale"), "##bpRingScale", &s.ringScale, Settings::kRingScaleMin,
                                        Settings::kRingScaleMax, "%.2f", Tr("display_ring_scale_tip"));
    return changed;
}

bool ColorsCard(Settings& s) {
    bool changed = false;
    const sui::Card card("##bp_display_colors");
    PluginUi::Heading(ICON_FA_PALETTE, Tr("display_colors_title"), Tr("display_colors_help"));
    const Settings defaults;
    struct Row {
        const char* labelKey;
        const char* tipKey;
        const char* id;
        uint32_t Settings::* member;
    };
    static constexpr Row kRows[] = {
        {I18n::Key("display_color_next"), I18n::Key("display_color_next_tip"), "next", &Settings::colorNext},
        {I18n::Key("display_color_missing"), I18n::Key("display_color_missing_tip"), "missing", &Settings::colorMissing},
        {I18n::Key("display_color_wrong"), I18n::Key("display_color_wrong_tip"), "wrong", &Settings::colorWrong},
        {I18n::Key("display_color_adjust"), I18n::Key("display_color_adjust_tip"), "adjust", &Settings::colorAdjust},
        {I18n::Key("display_color_ok"), I18n::Key("display_color_ok_tip"), "ok", &Settings::colorOk},
    };
    const sui::Disabled off(!s.showTree);
    const PluginUi::LabelColumn col({Tr(kRows[0].labelKey), Tr(kRows[1].labelKey), Tr(kRows[2].labelKey), Tr(kRows[3].labelKey),
                                     Tr(kRows[4].labelKey)});
    for (const Row& row : kRows) {
        const sui::Id id(row.id);
        col.RowNoWidth(Tr(row.labelKey));
        ImU32 color = s.*(row.member);
        if (PluginUi::Swatch("##swatch", color)) {
            s.*(row.member) = color;
            changed = true;
        }
        PluginUi::Tip(Tr(row.tipKey));
        ImGui::SameLine();
        {
            // The reset is there for every colour and lights up for the ones that were changed. Its
            // text gives way to an icon when the line has no room for it (a long translation).
            const sui::Disabled same(s.*(row.member) == defaults.*(row.member));
            const char* label = TrId("display_color_reset");
            const bool fits = PluginUi::ButtonWidth(label) <= ImGui::GetContentRegionAvail().x;
            if (fits ? PluginUi::SubtleButton(label) : PluginUi::SubtleButton(ICON_FA_ROTATE_LEFT "###display_color_reset")) {
                s.*(row.member) = defaults.*(row.member);
                changed = true;
            }
        }
        PluginUi::Tip(Tr("display_color_reset_tip"));
    }
    return changed;
}

bool GearCard(Settings& s) {
    bool changed = false;
    const sui::Card card("##bp_display_gear");
    PluginUi::Heading(ICON_FA_SHIELD_HALVED, Tr("display_gear_title"), Tr("display_gear_help"));
    changed |= Toggle(I18n::Key("display_gear_card"), "bpGearCard", s.showGearCard, I18n::Key("display_gear_card_tip"));
    changed |= Toggle(I18n::Key("display_slot_marks"), "bpSlotMarks", s.showSlotMarks, I18n::Key("display_slot_marks_tip"));
    return changed;
}

bool SkillsCard(App& app, Settings& s) {
    bool changed = false;
    const sui::Card card("##bp_display_skills");
    PluginUi::Heading(ICON_FA_WAND_MAGIC_SPARKLES, Tr("display_skills_title"), Tr("display_skills_help"));
    changed |= Toggle(I18n::Key("display_skill_marks"), "bpSkillMarks", s.showSkillMarks, I18n::Key("display_skill_marks_tip"));
    changed |= Toggle(I18n::Key("display_missing_skills"), "bpMissingSkills", s.showMissingSkills, I18n::Key("display_missing_skills_tip"));
    changed |= Toggle(I18n::Key("skills_show_granted"), "bpGranted", s.showGrantedSkills, I18n::Key("skills_show_granted_tip"));

    ImGui::Spacing();
    const PluginUi::LabelColumn col({Tr("display_window_mode"), Tr("display_window_hotkey")});
    {
        // Listed from "least" to "most", the default first; the stored values keep their meaning.
        static constexpr SkillsWindowMode kModes[] = {SkillsWindowMode::Never, SkillsWindowMode::WithSkillsPanel, SkillsWindowMode::Always};
        const char* const names[] = {Tr("display_window_never"), Tr("display_window_with_panel"), Tr("display_window_always")};
        int choice = 0;
        for (int i = 0; i < 3; ++i)
            if (kModes[i] == s.skillsWindowMode) choice = i;
        // Every answer shows whole: under the label when the line cannot hold both.
        sui::ComboRow(col, Tr("display_window_mode"), sui::ComboWidthFor(names, 3));
        if (PluginUi::Combo("##bpWindowMode", choice, names, 3)) {
            s.skillsWindowMode = kModes[std::clamp(choice, 0, 2)];
            changed = true;
        }
    }
    PluginUi::Tip(Tr("display_window_mode_tip"));
    {
        // The key button and its "Clear" need a fixed room: under the label when the line is too short.
        const float need = sui::Em() * 7.0f + ImGui::GetStyle().ItemSpacing.x +
                           PluginUi::ButtonWidth(PluginSDK::I18n::KitText("plugin_kit_clear", "Clear"));
        if (ImGui::GetContentRegionAvail().x - col.Width() >= need) {
            col.RowNoWidth(Tr("display_window_hotkey"));
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(Tr("display_window_hotkey"));
        }
        changed |= PluginUi::HotkeyCapture("##bpWindowHotkey", &s.skillsWindowHotkey, Tr("display_window_hotkey_tip"));
    }

    // Shows or hides the window right now, whatever the mode says (until the mode's answer changes).
    {
        const sui::Disabled none(app.Active().build == nullptr);
        if (PluginUi::SubtleButton(app.SkillsWindowShown() ? TrId("display_window_hide") : TrId("display_window_show")))
            app.ToggleSkillsWindow();
        PluginUi::Tip(Tr("display_window_toggle_tip"));
    }
    return changed;
}

bool GeneralCard(App& app, Settings& s, bool* reset) {
    bool changed = false;
    const sui::Card card("##bp_display_general");
    PluginUi::Heading(ICON_FA_SLIDERS, Tr("display_general_title"));
    const PluginUi::LabelColumn col({Tr("display_card_opacity")});
    changed |= PluginUi::SliderFloatRow(col, Tr("display_card_opacity"), "##bpCardOpacity", &s.cardOpacity, Settings::kCardOpacityMin,
                                        Settings::kCardOpacityMax, "%.2f", Tr("display_card_opacity_tip"));
    ImGui::Spacing();
    Settings defaults = s;
    ResetDisplaySettings(defaults);
    {
        const sui::Disabled nothing(defaults == s);
        if (PluginUi::SubtleButton(TrId("display_reset"))) ImGui::OpenPopup("##bpResetDisplay");
        PluginUi::Tip(Tr("display_reset_tip"));
    }
    if (PluginUi::ConfirmPopup("##bpResetDisplay", Tr("display_reset_confirm"), Tr("display_reset_yes"))) *reset = true;
    (void)app;
    return changed;
}

} // namespace

void DrawTabDisplay(App& app) {
    // Edited in place; ConfigChanged once at the end when anything moved.
    Settings& s = app.EditConfig();
    bool changed = false, reset = false;
    changed |= TreeCard(s);
    ImGui::Spacing();
    changed |= ColorsCard(s);
    ImGui::Spacing();
    changed |= GearCard(s);
    ImGui::Spacing();
    changed |= SkillsCard(app, s);
    ImGui::Spacing();
    changed |= GeneralCard(app, s, &reset);
    if (reset) app.ResetDisplay();
    else if (changed) app.ConfigChanged();
}

} // namespace bp
