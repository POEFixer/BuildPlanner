// BuildPlanner - the Builds tab
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// TabBuilds.cpp - the Builds tab: the import row, the saved builds, the stages with
// their skill set and gear set, and the card of the game's own Build Planner.
//
// Every action that changes the store or the selection is collected while a card draws and applied
// after its last read: App's accessors (Active(), Store()) are only valid until the next mutating call.
#include <string>
#include <vector>

#include "SettingsCommon.h"
#include "Theme.h"
#include "Ui.h"

namespace bp {
namespace {

using I18n::Tr;
using I18n::TrF;
using I18n::TrId;

// UI-only state of this tab (DrawSettings context; never an ImGui id or a pointer into the App).
std::string g_input;              // the import field: a link being typed, never a whole code
size_t g_inputLen = 0;            // its length on the previous frame (a jump = a paste)
std::string g_renameId;           // the build being renamed, "" = none
std::string g_renameText;
bool g_renameFocus = false;       // put the keyboard into the rename field on its first frame
bool g_renameRefused = false;     // the last name was refused (empty or too long)

// More than this in the field is no link any more: it goes to the importer and out of the box.
constexpr size_t kFieldKeepBytes = 2048;
// A field that grew by more than this in one frame was pasted into, not typed.
constexpr size_t kPasteJump = 16;

void SendToImporter(App& app, std::string text) {
    if (!app.StartImport(std::move(text))) return;
    // The field may be active (Enter, Ctrl+V): an active field keeps its own copy of the text.
    ImGui::ClearActiveID();
    g_input.clear();
    g_inputLen = 0;
}

// ---- import ---------------------------------------------------------------------------------

void ImportCard(App& app) {
    const sui::Card card("##bp_import");
    PluginUi::Heading(ICON_FA_FILE_IMPORT, Tr("import_title"), Tr("import_help"));

    const bool busy = app.JobRunning();
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* pasteLabel = TrId("import_paste");
    const char* importLabel = TrId("import_button");
    const float buttonsW = PluginUi::ButtonWidth(pasteLabel) + PluginUi::ButtonWidth(importLabel) + style.ItemSpacing.x * 2.0f;
    const float avail = ImGui::GetContentRegionAvail().x;
    // The field shares the line with its buttons while its hint (what to do here) still shows whole;
    // else the buttons go below it.
    const float hintW = ImGui::CalcTextSize(Tr("import_hint")).x + style.FramePadding.x * 2.0f + sui::Em() * 0.5f;
    const bool oneLine = avail - buttonsW >= (std::max)(sui::Em() * 12.0f, hintW);
    // A hint too long even for the whole line (a long translation on a narrow page) cannot wrap
    // inside the field: it becomes a line above it.
    const bool hintInField = hintW <= avail;
    if (!hintInField) PluginUi::Hint(Tr("import_hint"));

    bool start = false;
    ImGui::SetNextItemWidth(oneLine ? avail - buttonsW : -FLT_MIN);
    if (PluginUi::InputString("##bpImportText", hintInField ? Tr("import_hint") : "", g_input, ImGuiInputTextFlags_EnterReturnsTrue))
        start = !busy;
    const bool edited = ImGui::IsItemEdited();
    PluginUi::Tip(Tr("import_field_tip"));
    if (edited && g_input.size() > g_inputLen + kPasteJump) {
        // Pasted into the field: a link or a code starts the import by itself, and anything too long
        // to be a link leaves the box at once (a 200 KB code must never sit in a text field).
        const bool importable = StartsImport(ClassifyInput(g_input).kind);
        if (!busy && (importable || g_input.size() > kFieldKeepBytes)) {
            start = true;
        } else if (g_input.size() > kFieldKeepBytes) {
            ImGui::ClearActiveID();
            g_input.clear();
        }
    }
    g_inputLen = g_input.size();

    if (oneLine) ImGui::SameLine();
    std::string pasted;
    bool havePasted = false;
    {
        const sui::Disabled disabled(busy);
        if (ImGui::Button(pasteLabel)) {
            // The clipboard goes straight to the importer: whatever it holds, the result line says
            // what it was and what to do.
            const char* clip = ImGui::GetClipboardText();
            pasted = clip ? clip : "";
            havePasted = true;
        }
        PluginUi::Tip(Tr("import_paste_tip"));
        ImGui::SameLine();
        {
            // The secondary way in (a link typed into the field): nothing to import while it is empty.
            const sui::Disabled empty(g_input.empty());
            if (PluginUi::SubtleButton(importLabel)) start = true;
        }
        PluginUi::Tip(Tr("import_button_tip"));
    }

    if (app.RunningJob() == JobKind::Import) {
        PluginUi::StatusPill(Tr("import_busy_pill"), sui::AccentColor());
        ImGui::SameLine();
        if (PluginUi::SubtleButton(TrId("common_cancel"))) app.CancelJob();
        PluginUi::Tip(Tr("import_cancel_tip"));
    } else if (busy && app.Note(JobKind::Import).text.empty()) {
        PluginUi::Hint(Tr("common_busy"));
    } else {
        sui::NoteLine(app.Note(JobKind::Import));
    }

    if (havePasted) SendToImporter(app, std::move(pasted));
    else if (start) SendToImporter(app, g_input);
}

// ---- saved builds ---------------------------------------------------------------------------

// A radio-like marker in front of a build's name: a ring, filled for the selected build.
void SelectMarker(bool selected) {
    const float h = ImGui::GetTextLineHeight();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float baseline = ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset;   // before the Dummy zeroes it
    ImGui::Dummy(ImVec2(h, h));
    if (!ImGui::IsItemVisible()) return;
    const ImVec2 c(PluginUi::SnapPx(pos.x + h * 0.5f), PluginUi::SnapPx(pos.y + baseline + h * 0.5f));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 color = selected ? sui::AccentColor() : sui::DimColor();
    dl->AddCircle(c, h * 0.32f, color, 16, 1.5f);
    if (selected) dl->AddCircleFilled(c, h * 0.17f, color, 12);
}

std::string ClassText(const StoredBuild& b) {
    if (b.className.empty()) return b.ascendName;
    return b.ascendName.empty() ? b.className : b.className + " / " + b.ascendName;
}

// builds.json was there at the start but could not be used: the list is empty (or starts anew) and
// the user must know why, and that the file is not lost (BuildStore.h StoreFileVerdict).
void StoreFileNotice(const BuildStore& store) {
    switch (store.PendingFileVerdict()) {
    case StoreFileVerdict::Unusable: sui::WarnLine(Tr("builds_file_unusable")); break;
    case StoreFileVerdict::Newer: sui::WarnLine(Tr("builds_file_newer")); break;
    case StoreFileVerdict::Accepted: break;
    }
}

void SavedBuildsCard(App& app) {
    const sui::Card card("##bp_saved");
    const BuildStore& store = app.Store();
    if (store.builds.empty()) {
        // First run: what to do, step by step, instead of an empty list.
        PluginUi::Heading(ICON_FA_CIRCLE_PLAY, Tr("start_title"));
        StoreFileNotice(store);
        const float numberW = sui::StepNumberWidth();
        sui::Step(1, Tr("start_step_copy"), numberW);
        sui::Step(2, Tr("start_step_paste"), numberW);
        sui::Step(3, Tr("start_step_game"), numberW);
        PluginUi::Hint(Tr("start_more"));
        return;
    }
    PluginUi::Heading(ICON_FA_LIST, Tr("builds_title"), Tr("builds_help"));
    StoreFileNotice(store);

    const ImGuiStyle& style = ImGui::GetStyle();
    const std::string selected = app.Config().selectedBuild;
    const float levelW = sui::MaxTextWidth({Tr("builds_col_level"), "100"});
    const float dateW = sui::MaxTextWidth({Tr("builds_col_imported"), "2026-12-30"});
    const float actionsW = sui::IconButtonWidth(ICON_FA_PEN) + sui::IconButtonWidth(ICON_FA_TRASH) + style.ItemSpacing.x;
    // On a narrow page the name keeps its room: the import date gives way first, then the class
    // (both are repeated under the table for the build that is followed).
    const float tableW = ImGui::GetContentRegionAvail().x;
    const bool showDate = tableW >= sui::Em() * 38.0f;
    const bool showClass = tableW >= sui::Em() * 28.0f;
    const float rowH = ImGui::GetFrameHeight();

    std::string doSelect, doDelete;
    bool commitRename = false, cancelRename = false;
    if (const sui::Table table{"##bpBuilds", 3 + (showClass ? 1 : 0) + (showDate ? 1 : 0), sui::kTableFlags}) {
        ImGui::TableSetupColumn(TrId("builds_col_name"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
        if (showClass) ImGui::TableSetupColumn(TrId("builds_col_class"), ImGuiTableColumnFlags_WidthStretch, 2.2f);
        ImGui::TableSetupColumn(TrId("builds_col_level"), ImGuiTableColumnFlags_WidthFixed, levelW);
        if (showDate) ImGui::TableSetupColumn(TrId("builds_col_imported"), ImGuiTableColumnFlags_WidthFixed, dateW);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, actionsW);
        ImGui::TableHeadersRow();

        for (const StoredBuild& b : store.builds) {
            const sui::Id id(b.id.c_str());
            const bool isSelected = b.id == selected;
            const bool renaming = b.id == g_renameId;
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            if (renaming) {
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (g_renameFocus) {
                    ImGui::SetKeyboardFocusHere();
                    g_renameFocus = false;
                }
                if (PluginUi::InputString("##rename", Tr("builds_rename_hint"), g_renameText,
                                          ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
                    commitRename = true;
                else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && (ImGui::IsItemFocused() || ImGui::IsItemDeactivated()))
                    cancelRename = true;   // Esc ends the field's edit in the same frame
            } else {
                if (ImGui::Selectable("##row", isSelected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                      ImVec2(0.0f, rowH)))
                    doSelect = b.id;
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::AlignTextToFramePadding();
                SelectMarker(isSelected);
                ImGui::SameLine();
                sui::Clipped(b.name, isSelected ? sui::AccentColor() : 0);
            }

            int column = 1;
            if (showClass) {
                ImGui::TableSetColumnIndex(column++);
                ImGui::AlignTextToFramePadding();
                sui::Clipped(ClassText(b));
            }

            ImGui::TableSetColumnIndex(column++);
            ImGui::AlignTextToFramePadding();
            if (b.level > 0) ImGui::Text("%d", b.level);
            else ImGui::TextDisabled("-");

            if (showDate) {
                ImGui::TableSetColumnIndex(column++);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(sui::DateText(b.importedAt).c_str());
            }

            ImGui::TableSetColumnIndex(column);
            if (renaming) {
                if (sui::IconButton(ICON_FA_CHECK, "renameOk", Tr("builds_rename_ok_tip"))) commitRename = true;
                ImGui::SameLine();
                if (sui::IconButton(ICON_FA_XMARK, "renameCancel", Tr("builds_rename_cancel_tip"))) cancelRename = true;
            } else {
                if (sui::IconButton(ICON_FA_PEN, "rename", Tr("builds_rename_tip"))) {
                    g_renameId = b.id;
                    g_renameText = b.name;
                    g_renameFocus = true;
                    g_renameRefused = false;
                }
                ImGui::SameLine();
                if (sui::IconButton(ICON_FA_TRASH, "delete", Tr("builds_delete_tip"))) ImGui::OpenPopup("##bpConfirmDelete");
                if (PluginUi::ConfirmPopup("##bpConfirmDelete", TrF("builds_delete_confirm", {{"name", b.name}}).c_str(),
                                           Tr("builds_delete_yes")))
                    doDelete = b.id;
            }
        }
    }
    if (g_renameRefused) sui::WarnLine(Tr("builds_rename_refused"));

    // The selected build's origin, under the table (also where the import date goes on a narrow page).
    if (const StoredBuild* b = store.Find(selected)) {
        const std::string facts = TrF("builds_facts", {{"date", sui::DateText(b->importedAt)},
                                                       {"trees", std::to_string(b->specs.size())},
                                                       {"skills", std::to_string(b->skillSets.size())},
                                                       {"gear", std::to_string(b->itemSets.size())}});
        if (!showClass && !ClassText(*b).empty()) PluginUi::Hint(ClassText(*b).c_str());
        PluginUi::Hint(facts.c_str());
        sui::Clipped(TrF("builds_source", {{"source", b->source}}), sui::DimColor());
        // A build for another class is the first thing to know about it, here where it is chosen.
        if (app.Plan().classDiffers) sui::WarnLine(Tr("passives_class_differs"));
    } else {
        PluginUi::Hint(Tr("builds_none_selected"));
    }

    // The actions, after the last read of the store.
    if (!doDelete.empty()) {
        if (doDelete == g_renameId) g_renameId.clear();
        app.DeleteBuild(doDelete);
    } else if (commitRename) {
        if (app.RenameBuild(g_renameId, g_renameText)) {
            g_renameId.clear();
            g_renameRefused = false;
        } else {
            g_renameRefused = true;
            g_renameFocus = true;
        }
    } else if (cancelRename) {
        g_renameId.clear();
        g_renameRefused = false;
    } else if (!doSelect.empty()) {
        app.SelectBuild(doSelect);
    }
}

// ---- stages ---------------------------------------------------------------------------------

const char* StageHowText(StageHow how) {
    switch (how) {
    case StageHow::Pinned: return Tr("stages_how_pinned");
    case StageHow::ByLevel: return Tr("stages_how_level");
    case StageHow::FirstUnfinished: return Tr("stages_how_unfinished");
    case StageHow::PobActive: return Tr("stages_how_pob");
    case StageHow::Last: return Tr("stages_how_last");
    case StageHow::None: break;
    }
    return "";
}

// The set the automatic rule gives a stage (Loadouts.h ResolveSets without the user's override):
// the stage's link, else Path of Building's active set, else the first one.
int AutomaticSet(int link, int pobActive, int count) {
    if (count <= 0) return -1;
    if (link >= 0 && link < count) return link;
    if (pobActive >= 0 && pobActive < count) return pobActive;
    return 0;
}

// A "which set" combo: entry 0 follows the stage, the others pin a set.
struct SetChoice {
    std::vector<std::string> texts;   // [0] "Automatic: <title>", then every set; or the one "no set" text
    std::vector<const char*> items;
    int value = 0;                    // the entry shown
    bool any = false;                 // the build has sets of this kind
};

template <class SetList>
SetChoice MakeSetChoice(const SetList& sets, int pinned, int automatic, const char* nameKey, const char* noneKey) {
    SetChoice c;
    const int count = static_cast<int>(sets.size());
    c.any = count > 0;
    if (!c.any) {
        c.texts.push_back(Tr(noneKey));
    } else {
        c.texts.reserve(static_cast<size_t>(count) + 1);
        const std::string autoTitle = automatic >= 0 ? sui::TitleOr(sets[static_cast<size_t>(automatic)].title, nameKey, automatic) : std::string();
        c.texts.push_back(TrF("stages_set_auto", {{"title", autoTitle}}));
        for (int i = 0; i < count; ++i) c.texts.push_back(sui::TitleOr(sets[static_cast<size_t>(i)].title, nameKey, i));
        c.value = (pinned >= 0 && pinned < count) ? pinned + 1 : 0;
    }
    c.items.reserve(c.texts.size());
    for (const std::string& t : c.texts) c.items.push_back(t.c_str());
    return c;
}

// Draws the combo (the caller placed its label and width). Returns the new pin (-1 = automatic) in
// *pin and true when the user chose something.
bool SetCombo(const char* id, const SetChoice& c, int* pin) {
    int value = c.value;
    const sui::Disabled none(!c.any);
    if (!PluginUi::Combo(id, value, c.items.data(), static_cast<int>(c.items.size())) || !c.any) return false;
    *pin = value - 1;
    return true;
}

void StagesCard(App& app) {
    const ActiveView& a = app.Active();
    const StoredBuild* build = a.build;
    if (!build) return;
    const sui::Card card("##bp_stages");
    PluginUi::Heading(ICON_FA_LAYER_GROUP, Tr("stages_title"), Tr("stages_help"));

    bool follow = app.Config().autoStageByLevel;
    const bool followChanged = PluginUi::Check(Tr("stages_follow_level"), "bpFollowLevel", &follow, Tr("stages_follow_level_tip"));

    const bool pinned = a.choice.how == StageHow::Pinned;
    int pinStage = -2;   // -2 = no action, -1 = automatic, >= 0 = that stage
    if (const Stage* stage = a.StageInfo()) {
        const std::string following = TrF("stages_following", {{"stage", sui::TitleOr(stage->title, I18n::Key("common_tree_n"), stage->spec)},
                                                               {"how", StageHowText(a.choice.how)}});
        sui::InfoLine(following);
    }

    const float numW = sui::MaxTextWidth({"#", "00"});
    // Points as the player spends them, the ascendancy's in brackets ("113 (+8)", Stage::ascPoints).
    const float pointsW = sui::MaxTextWidth({Tr("stages_col_points"), "000 (+00)"});
    const float stateW = (std::max)(ImGui::CalcTextSize(Tr("stages_col_state")).x,
                                    (std::max)(sui::PillSize(Tr("stages_state_done")).x,
                                               (std::max)(sui::PillSize(Tr("stages_state_current")).x, sui::PillSize(Tr("stages_state_later")).x)));
    // The stage's name keeps a readable width: the points column gives way when the page (or a long
    // translation of the headers) leaves too little for it.
    const float cellPad = ImGui::GetStyle().CellPadding.x * 2.0f;
    const bool showPoints = ImGui::GetContentRegionAvail().x - (numW + pointsW + stateW + cellPad * 4.0f) >= sui::Em() * 8.0f;
    if (const sui::Table table{"##bpStages", showPoints ? 4 : 3, sui::kTableFlags}) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, numW);
        ImGui::TableSetupColumn(TrId("stages_col_stage"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        if (showPoints) ImGui::TableSetupColumn(TrId("stages_col_points"), ImGuiTableColumnFlags_WidthFixed, pointsW);
        ImGui::TableSetupColumn(TrId("stages_col_state"), ImGuiTableColumnFlags_WidthFixed, stateW);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < a.stages.size(); ++i) {
            const Stage& s = a.stages[i];
            const sui::Id id(static_cast<int>(i));
            const bool current = static_cast<int>(i) == a.choice.stage;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable("##stage", current, ImGuiSelectableFlags_SpanAllColumns)) pinStage = static_cast<int>(i);
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::Text("%d", static_cast<int>(i) + 1);

            ImGui::TableSetColumnIndex(1);
            if (build->selectedSpec == static_cast<int>(i)) {
                const sui::TextColor c(sui::AccentColor());
                ImGui::TextUnformatted(ICON_FA_THUMBTACK);
                ImGui::SameLine();
            }
            sui::Clipped(sui::TitleOr(s.title, I18n::Key("common_tree_n"), s.spec), current ? sui::AccentColor() : 0);

            if (showPoints) {
                ImGui::TableSetColumnIndex(2);
                if (s.ascPoints > 0) ImGui::Text("%d (+%d)", s.points, s.ascPoints);
                else ImGui::Text("%d", s.points);
            }

            ImGui::TableSetColumnIndex(showPoints ? 3 : 2);
            switch (StateOfStage(static_cast<int>(i), a.choice.stage)) {
            case StageState::Done: sui::Pill(Tr("stages_state_done"), sui::DimColor()); break;
            case StageState::Current: sui::Pill(Tr("stages_state_current"), sui::GoodColor()); break;
            case StageState::Later: sui::Pill(Tr("stages_state_later"), sui::DimColor()); break;
            }
        }
    }
    PluginUi::Hint(Tr("stages_click_hint"));
    {
        // The way back from a clicked stage; there is nothing to undo while the plugin chooses.
        const sui::Disabled disabled(!pinned);
        if (PluginUi::SubtleButton(TrId("stages_automatic"))) pinStage = -1;
        PluginUi::Tip(Tr("stages_automatic_tip"));
    }

    // Which skill set and gear set go with the stage.
    ImGui::Spacing();
    const Stage* stage = a.StageInfo();
    const PluginUi::LabelColumn col({Tr("stages_skill_set"), Tr("stages_gear_set")});
    int pinSkill = -2, pinGear = -2;
    const SetChoice skillChoice =
        MakeSetChoice(build->skillSets, build->selectedSkillSet,
                      AutomaticSet(stage ? stage->skillSet : -1, build->activeSkillSet, static_cast<int>(build->skillSets.size())),
                      I18n::Key("common_skill_set_n"), I18n::Key("stages_no_skill_sets"));
    const SetChoice gearChoice =
        MakeSetChoice(build->itemSets, build->selectedItemSet,
                      AutomaticSet(stage ? stage->itemSet : -1, build->activeItemSet, static_cast<int>(build->itemSets.size())),
                      I18n::Key("common_gear_set_n"), I18n::Key("stages_no_gear_sets"));
    // Both combos are as wide as the longest title of either needs: a set's name is never cut while
    // the page has the room, and the two line up.
    const float comboW = (std::max)(sui::ComboWidthFor(skillChoice.items.data(), static_cast<int>(skillChoice.items.size())),
                                    sui::ComboWidthFor(gearChoice.items.data(), static_cast<int>(gearChoice.items.size())));
    int pin = -1;
    sui::ComboRow(col, Tr("stages_skill_set"), comboW);
    if (SetCombo("##bpSkillSet", skillChoice, &pin)) pinSkill = pin;
    PluginUi::Tip(Tr("stages_skill_set_tip"));
    sui::ComboRow(col, Tr("stages_gear_set"), comboW);
    if (SetCombo("##bpGearSet", gearChoice, &pin)) pinGear = pin;
    PluginUi::Tip(Tr("stages_gear_set_tip"));

    // The actions, after the last read of `a` and `build`.
    if (followChanged) {
        app.EditConfig().autoStageByLevel = follow;
        app.ConfigChanged();
    }
    if (pinStage != -2) app.PinStage(pinStage);
    else if (pinSkill != -2) app.PinSkillSet(pinSkill);
    else if (pinGear != -2) app.PinItemSet(pinGear);
}

// ---- the game's own Build Planner -----------------------------------------------------------

void BuildFileCard(App& app) {
    if (!app.Active().build) return;
    const sui::Card card("##bp_buildfile");
    PluginUi::Heading(ICON_FA_FILE_EXPORT, Tr("buildfile_title"), Tr("buildfile_help"));
    PluginUi::Hint(Tr("buildfile_intro"));

    const bool busy = app.JobRunning();
    const std::string fileName = app.Config().buildFileName;
    const char* writeLabel = TrId("buildfile_write");
    const char* removeLabel = TrId("buildfile_remove");
    const char* folderLabel = TrId("buildfile_folder");
    bool doWrite = false, doRemove = false;
    const float right = theme::LineRight();
    {
        const sui::Disabled disabled(busy);
        if (ImGui::Button(writeLabel)) doWrite = true;
        PluginUi::Tip(Tr("buildfile_write_tip"));
    }
    sui::SameLineIfFits(PluginUi::ButtonWidth(removeLabel), right);
    {
        const sui::Disabled disabled(busy || fileName.empty());
        if (PluginUi::SubtleButton(removeLabel)) doRemove = true;
        PluginUi::Tip(Tr("buildfile_remove_tip"));
    }
    sui::SameLineIfFits(PluginUi::ButtonWidth(folderLabel), right);
    if (PluginUi::SubtleButton(folderLabel)) app.OpenBuildFolder();
    PluginUi::Tip(Tr("buildfile_folder_tip"));

    bool keep = app.Config().buildFileKeepUpdated;
    if (PluginUi::Check(Tr("buildfile_keep"), "bpKeepUpdated", &keep, Tr("buildfile_keep_tip"))) {
        app.EditConfig().buildFileKeepUpdated = keep;
        app.ConfigChanged();
    }

    if (fileName.empty()) PluginUi::Hint(Tr("buildfile_none"));
    else sui::Clipped(TrF("buildfile_file", {{"file", fileName}}), sui::DimColor());

    const JobKind running = app.RunningJob();
    if (running == JobKind::WriteBuildFile || running == JobKind::RemoveBuildFile)
        PluginUi::StatusPill(Tr("buildfile_busy_pill"), sui::AccentColor());
    else sui::NoteLine(app.Note(JobKind::WriteBuildFile));

    if (doWrite) app.WriteBuildFile();
    else if (doRemove) app.RemoveBuildFile();
}

} // namespace

void DrawTabBuilds(App& app) {
    ImportCard(app);
    ImGui::Spacing();
    SavedBuildsCard(app);
    if (app.Active().build) {
        ImGui::Spacing();
        StagesCard(app);
        ImGui::Spacing();
        BuildFileCard(app);
    }
}

} // namespace bp
