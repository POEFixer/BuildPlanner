// BuildPlanner - the Diagnostics card
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// Diagnostics.cpp - the Diagnostics card that ends every settings tab: what each reader
// sees and its problem text, the tree data in use against the game, the offsets in use and where
// they came from, the last problems, "Update tree data", "Use bundled data", "Retry now" and "Copy
// diagnostics". Also the texts of the problem codes and the slot names that the overlays and the tabs
// share (Ui.h).
#include <string>

#include "SettingsCommon.h"
#include "Ui.h"

namespace bp {

const char* TreeProblemText(TreeProblem p) {
    switch (p) {
    case TreeProblem::None: return "";
    case TreeProblem::NotInGame: return I18n::Tr("tree_problem_not_in_game");
    case TreeProblem::NoTreeData: return I18n::Tr("tree_problem_no_data");
    case TreeProblem::NoUiRoot: return I18n::Tr("tree_problem_no_ui");
    case TreeProblem::NoPanel: return I18n::Tr("tree_problem_no_panel");
    case TreeProblem::NoCanvas: return I18n::Tr("tree_problem_no_canvas");
    case TreeProblem::NoNodes: return I18n::Tr("tree_problem_no_nodes");
    case TreeProblem::DataMismatch: return I18n::Tr("tree_problem_mismatch");
    case TreeProblem::ReadFailed: return I18n::Tr("tree_problem_read");
    }
    return "";
}

const char* CharProblemText(CharProblem p) {
    switch (p) {
    case CharProblem::None: return "";
    case CharProblem::NotInGame: return I18n::Tr("char_problem_not_in_game");
    case CharProblem::NoTreeData: return I18n::Tr("char_problem_no_data");
    case CharProblem::NoUiRoot: return I18n::Tr("char_problem_no_ui");
    case CharProblem::NoPlayerData: return I18n::Tr("char_problem_no_player");
    case CharProblem::NoPassiveObject: return I18n::Tr("char_problem_no_object");
    case CharProblem::BadList: return I18n::Tr("char_problem_bad_list");
    case CharProblem::UnknownIds: return I18n::Tr("char_problem_unknown_ids");
    case CharProblem::BadScalars: return I18n::Tr("char_problem_bad_values");
    case CharProblem::LevelMismatch: return I18n::Tr("char_problem_level");
    case CharProblem::ReadFailed: return I18n::Tr("char_problem_read");
    }
    return "";
}

const char* GearProblemText(GearProblem p) {
    switch (p) {
    case GearProblem::None: return "";
    case GearProblem::NotInGame: return I18n::Tr("gear_problem_not_in_game");
    case GearProblem::NoPanel: return I18n::Tr("gear_problem_no_panel");
    case GearProblem::NoSlots: return I18n::Tr("gear_problem_no_slots");
    case GearProblem::ReadFailed: return I18n::Tr("gear_problem_read");
    }
    return "";
}

const char* SkillsProblemText(SkillsProblem p) {
    switch (p) {
    case SkillsProblem::None: return "";
    case SkillsProblem::NotInGame: return I18n::Tr("skills_problem_not_in_game");
    case SkillsProblem::NoPlayerData: return I18n::Tr("skills_problem_no_player");
    case SkillsProblem::NoGems: return I18n::Tr("skills_problem_no_gems");
    case SkillsProblem::NoPanel: return I18n::Tr("skills_problem_no_panel");
    case SkillsProblem::ReadFailed: return I18n::Tr("skills_problem_read");
    }
    return "";
}

const char* GearSlotText(GearSlot slot) {
    switch (slot) {
    case GearSlot::Weapon1: return I18n::Tr("slot_weapon1");
    case GearSlot::Offhand1: return I18n::Tr("slot_offhand1");
    case GearSlot::Weapon2: return I18n::Tr("slot_weapon2");
    case GearSlot::Offhand2: return I18n::Tr("slot_offhand2");
    case GearSlot::Helm: return I18n::Tr("slot_helm");
    case GearSlot::BodyArmour: return I18n::Tr("slot_body");
    case GearSlot::Gloves: return I18n::Tr("slot_gloves");
    case GearSlot::Boots: return I18n::Tr("slot_boots");
    case GearSlot::Belt: return I18n::Tr("slot_belt");
    case GearSlot::Amulet: return I18n::Tr("slot_amulet");
    case GearSlot::Ring1: return I18n::Tr("slot_ring1");
    case GearSlot::Ring2: return I18n::Tr("slot_ring2");
    case GearSlot::Ring3: return I18n::Tr("slot_ring3");
    case GearSlot::Flask1: return I18n::Tr("slot_flask1");
    case GearSlot::Flask2: return I18n::Tr("slot_flask2");
    case GearSlot::Charm1: return I18n::Tr("slot_charm1");
    case GearSlot::Charm2: return I18n::Tr("slot_charm2");
    case GearSlot::Charm3: return I18n::Tr("slot_charm3");
    case GearSlot::Count: break;
    }
    return "";
}

std::string ImportProblemText(ImportProblem problem, const std::string& detail) {
    switch (problem) {
    case ImportProblem::None: return {};
    case ImportProblem::Empty: return I18n::Tr("import_problem_empty");
    case ImportProblem::Unrecognized: return I18n::Tr("import_problem_unrecognized");
    case ImportProblem::MaxrollGuide: return I18n::Tr("import_problem_maxroll_guide");
    case ImportProblem::Download: return I18n::TrF("import_problem_download", {{"detail", detail}});
    case ImportProblem::NotFound: return I18n::Tr("import_problem_not_found");
    case ImportProblem::Decode: return I18n::TrF("import_problem_decode", {{"detail", detail}});
    case ImportProblem::Parse: return I18n::TrF("import_problem_parse", {{"detail", detail}});
    case ImportProblem::NoSpecs: return I18n::Tr("import_problem_no_tree");
    case ImportProblem::Cancelled: return I18n::Tr("import_cancelled");
    }
    return {};
}

namespace {

using I18n::Tr;
using I18n::TrF;
using I18n::TrId;

std::string Num(long long n) { return std::to_string(n); }

// One reader's row: its name, a state pill and one wrapped sentence (what it sees, or the problem
// text: the cause and what to do).
enum class ReaderTone { Ok, Waiting, Problem };

void ReaderRow(const char* name, ReaderTone tone, const std::string& detail) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(name);
    ImGui::TableSetColumnIndex(1);
    switch (tone) {
    case ReaderTone::Ok: sui::Pill(Tr("diag_state_ok"), sui::GoodColor()); break;
    case ReaderTone::Waiting: sui::Pill(Tr("diag_state_waiting"), sui::DimColor()); break;
    case ReaderTone::Problem: sui::Pill(Tr("diag_state_problem"), sui::WarnColor()); break;
    }
    ImGui::TableSetColumnIndex(2);
    sui::Wrapped(detail, tone == ReaderTone::Problem ? sui::WarnColor() : sui::DimColor());
}

void ReadersTable(const GameView& v) {
    const float nameW = sui::MaxTextWidth({Tr("diag_col_part"), Tr("diag_part_tree"), Tr("diag_part_character"), Tr("diag_part_gear"),
                                           Tr("diag_part_skills")});
    const float stateW = (std::max)(ImGui::CalcTextSize(Tr("diag_col_state")).x,
                                    (std::max)(sui::PillSize(Tr("diag_state_ok")).x,
                                               (std::max)(sui::PillSize(Tr("diag_state_waiting")).x, sui::PillSize(Tr("diag_state_problem")).x)));
    const sui::Table table("##bpReaders", 3, sui::kTableFlags);
    if (!table) return;
    ImGui::TableSetupColumn(TrId("diag_col_part"), ImGuiTableColumnFlags_WidthFixed, nameW);
    ImGui::TableSetupColumn(TrId("diag_col_state"), ImGuiTableColumnFlags_WidthFixed, stateW);
    ImGui::TableSetupColumn(TrId("diag_col_sees"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();
    // A reader that only waits for a character is no problem, whatever the game state says.
    const auto tone = [](bool notInGame) { return notInGame ? ReaderTone::Waiting : ReaderTone::Problem; };

    // The passive tree panel. One whole sentence per state, never a shared "open" / "closed" word put
    // into it: in French, Spanish or Portuguese the word agrees with the panel's noun.
    const TreeTable& t = *v.tree;
    if (t.found) {
        const std::string nodes = Num(static_cast<long long>(t.nodes.size()));
        const std::string coverage = Num(static_cast<long long>(t.coverage * 100.0f + 0.5f));
        ReaderRow(Tr("diag_part_tree"), ReaderTone::Ok,
                  v.treeOpen ? TrF("diag_tree_found_open", {{"nodes", nodes}, {"coverage", coverage}})
                             : TrF("diag_tree_found_closed", {{"nodes", nodes}, {"coverage", coverage}}));
    } else {
        ReaderRow(Tr("diag_part_tree"), tone(t.problem == TreeProblem::NotInGame), TreeProblemText(t.problem));
    }

    // The character's passives.
    const CharState& c = *v.character;
    if (c.valid) {
        ReaderRow(Tr("diag_part_character"), ReaderTone::Ok,
                  TrF("diag_character_found", {{"level", Num(c.level)},
                                               {"passives", Num(static_cast<long long>(c.allocated.size()))},
                                               {"free", Num(c.freePoints)}}));
    } else {
        ReaderRow(Tr("diag_part_character"), tone(c.problem == CharProblem::NotInGame), CharProblemText(c.problem));
    }

    // The equipment slots.
    const GearState& g = *v.gear;
    if (g.found) {
        ReaderRow(Tr("diag_part_gear"), ReaderTone::Ok,
                  g.itemsKnown ? TrF("diag_gear_found", {{"slots", Num(g.boundSlots)}})
                               : TrF("diag_gear_found_no_items", {{"slots", Num(g.boundSlots)}}));
    } else {
        ReaderRow(Tr("diag_part_gear"), tone(g.problem == GearProblem::NotInGame), GearProblemText(g.problem));
    }

    // The character's skills and the Skills window.
    const SkillsState& k = *v.skills;
    if (k.skillsKnown) {
        // The gems are known; a Skills window that does not validate only loses the socket marks.
        const std::string skills = Num(static_cast<long long>(k.skills.size()));
        std::string text = !k.panelFound ? TrF("diag_skills_found_missing", {{"skills", skills}})
                           : k.panelOpen ? TrF("diag_skills_found_open", {{"skills", skills}})
                                         : TrF("diag_skills_found_closed", {{"skills", skills}});
        if (k.problem == SkillsProblem::NoPanel) text += std::string(" ") + SkillsProblemText(k.problem);
        ReaderRow(Tr("diag_part_skills"), k.problem == SkillsProblem::NoPanel ? ReaderTone::Problem : ReaderTone::Ok, text);
    } else {
        ReaderRow(Tr("diag_part_skills"), tone(k.problem == SkillsProblem::NotInGame), SkillsProblemText(k.problem));
    }
}

// The tree data in use, what the game says about it, and the two buttons that change it.
void TreeDataBlock(App& app, const GameView& v) {
    const TreeInfo& info = app.TreeStatus();
    sui::Caption(Tr("diag_tree_data"));
    if (info.loaded) {
        const std::string version = info.treeVersion.empty() ? std::string("?") : info.treeVersion;
        std::string line = info.from == "downloaded" ? TrF("diag_tree_downloaded", {{"version", version}, {"nodes", Num(info.nodes)}})
                                                     : TrF("diag_tree_bundled", {{"version", version}, {"nodes", Num(info.nodes)}});
        const std::string date = sui::DateText(info.fileTime);
        if (!date.empty()) line += " " + TrF("diag_tree_date", {{"date", date}});
        sui::Wrapped(line, ImGui::GetColorU32(ImGuiCol_Text));
    } else if (info.loading) {
        PluginUi::Hint(Tr("diag_tree_loading"));
    } else {
        sui::WarnLine(Tr("diag_tree_missing"));
    }
    // Does the data fit the game? The tree reader measured it; the stage's own version is the other clue.
    const TreeTable& t = *v.tree;
    if (t.found) {
        sui::IconLine(ICON_FA_CIRCLE_CHECK, TrF("diag_tree_fits", {{"coverage", Num(static_cast<long long>(t.coverage * 100.0f + 0.5f))}}),
                      sui::GoodColor());
    } else if (t.problem == TreeProblem::DataMismatch) {
        sui::WarnLine(TreeProblemText(t.problem));
    }
    if (app.Plan().treeOlder) sui::WarnLine(Tr("passives_tree_older"));

    const bool busy = app.JobRunning();
    const bool downloaded = info.from == "downloaded";
    const char* updateLabel = TrId("diag_update_tree");
    const char* bundledLabel = TrId("diag_use_bundled");
    bool update = false, bundled = false;
    const float right = theme::LineRight();
    {
        // A demo never downloads (App::UpdateTreeData refuses it too).
        const sui::Disabled disabled(busy || app.DemoMode());
        if (ImGui::Button(updateLabel)) update = true;
        PluginUi::Tip(Tr("diag_update_tree_tip"));
    }
    sui::SameLineIfFits(PluginUi::ButtonWidth(bundledLabel), right);
    {
        // Nothing to go back from while the bundled data is in use.
        const sui::Disabled disabled(busy || !downloaded);
        if (PluginUi::SubtleButton(bundledLabel)) bundled = true;
        PluginUi::Tip(Tr("diag_use_bundled_tip"));
    }
    if (app.RunningJob() == JobKind::UpdateTree) {
        PluginUi::StatusPill(Tr("diag_update_busy_pill"), sui::AccentColor());
        ImGui::SameLine();
        if (PluginUi::SubtleButton(TrId("common_cancel"))) app.CancelJob();
    } else {
        sui::NoteLine(app.Note(JobKind::UpdateTree));
        sui::NoteLine(app.Note(JobKind::LoadData));
    }
    // After the last read of `info`: both calls replace it.
    if (update) app.UpdateTreeData();
    else if (bundled) app.UseBundledData();
}

// The private offsets: where the values came from, what the files complained about, and the table
// itself behind a disclosure (it is long, and names and values are identifiers, not prose).
void OffsetsBlock(const App& app, bool openAll) {
    const OffsetsInfo& info = app.OffsetsInUse();
    const std::span<const OffsetField> fields = OffsetFields();
    sui::Caption(Tr("diag_offsets"));
    int fromConfig = 0;
    for (const OffsetSource src : info.source) fromConfig += src == OffsetSource::ConfigFile ? 1 : 0;
    if (fromConfig > 0) sui::IconLine(ICON_FA_CIRCLE_INFO, TrF("diag_offsets_config", {{"n", Num(fromConfig)}}), sui::WarnColor());
    else PluginUi::Hint(info.dataFileRead ? Tr("diag_offsets_shipped") : Tr("diag_offsets_builtin"));
    for (const std::string& w : info.warnings) sui::WarnLine(w);   // English, as logged

    if (openAll) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    if (!PluginUi::Disclosure(Tr("diag_offsets_show"), "bpOffsets")) return;
    const float valueW = sui::MaxTextWidth({Tr("diag_col_value"), "0x00000000"});
    const sui::Table table("##bpOffsets", 3, sui::kTableFlags);
    if (!table) return;
    // On a narrow page the name column gives way (cut names keep their tooltip), never the value.
    ImGui::TableSetupColumn(TrId("diag_col_offset"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn(TrId("diag_col_value"), ImGuiTableColumnFlags_WidthFixed, valueW);
    ImGui::TableSetupColumn(TrId("diag_col_source"), ImGuiTableColumnFlags_WidthStretch, 2.0f);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < fields.size(); ++i) {
        const OffsetSource src = i < info.source.size() ? info.source[i] : OffsetSource::BuiltIn;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        sui::Clipped(fields[i].name);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(OffsetHex(info.values.*(fields[i].member)).c_str());
        ImGui::TableSetColumnIndex(2);
        switch (src) {
        case OffsetSource::ConfigFile: sui::Clipped(Tr("diag_source_config"), sui::WarnColor()); break;
        case OffsetSource::DataFile: sui::Clipped(Tr("diag_source_data"), sui::DimColor()); break;
        case OffsetSource::BuiltIn: sui::Clipped(Tr("diag_source_builtin"), sui::DimColor()); break;
        }
    }
}

} // namespace

void DrawDiagnostics(App& app) {
    const bool openAll = app.DemoMode() && app.DemoOpenAll();
    if (openAll) ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    const sui::Diagnostics card(Tr("diag_tip"), "bp_diag");
    if (!card) return;
    const std::shared_ptr<const GameView> view = app.View();
    const GameView& v = *view;

    // The game as a whole, and the two actions on the card itself.
    const char* stateText = !app.HostCompatible() ? Tr("diag_game_incompatible")
                            : app.DemoMode()      ? Tr("diag_game_demo")
                            : v.status.inGame     ? Tr("diag_in_game")
                            : v.status.attached   ? Tr("diag_not_in_game")
                                                  : Tr("diag_not_attached");
    sui::IconLine(v.status.inGame ? ICON_FA_CIRCLE_CHECK : ICON_FA_CIRCLE_INFO, stateText,
                  v.status.inGame ? sui::GoodColor() : sui::DimColor());
    const char* retryLabel = TrId("diag_retry");
    const char* copyLabel = TrId("diag_copy");
    const float right = theme::LineRight();
    if (ImGui::Button(retryLabel)) app.KickReaders();
    PluginUi::Tip(Tr("diag_retry_tip"));
    sui::SameLineIfFits(PluginUi::ButtonWidth(copyLabel), right);
    if (PluginUi::SubtleButton(copyLabel)) ImGui::SetClipboardText(app.DiagnosticsText().c_str());
    PluginUi::Tip(Tr("diag_copy_tip"));

    ImGui::Spacing();
    ReadersTable(v);

    ImGui::Spacing();
    TreeDataBlock(app, v);

    ImGui::Spacing();
    OffsetsBlock(app, openAll);

    // The last warnings and errors: log lines, English, as written to the host log.
    const std::vector<std::string>& problems = app.RecentProblems();
    if (!problems.empty()) {
        ImGui::Spacing();
        sui::Caption(Tr("diag_problems"));
        const size_t first = problems.size() > 8 ? problems.size() - 8 : 0;
        for (size_t i = first; i < problems.size(); ++i) sui::Wrapped(problems[i], sui::DimColor());
    }
}

} // namespace bp
