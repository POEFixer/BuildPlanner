// BuildPlanner - the Passives tab
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// TabPassives.cpp - the Passives tab: the plan as text (what the tree overlay draws): what is
// compared, the counts, the next objectives in order, what to refund or adjust, and what the build
// chooses on attribute passives and with the weapon set points.
#include <algorithm>
#include <map>
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

constexpr size_t kMaxObjectiveRows = 25;
constexpr size_t kMaxRefundChips = 60;
constexpr size_t kMaxAdjustRows = 40;

std::string Num(long long n) { return std::to_string(n); }

const char* AttrText(Attr a) {
    switch (a) {
    case Attr::Str: return Tr("attr_strength");
    case Attr::Dex: return Tr("attr_dexterity");
    case Attr::Int: return Tr("attr_intelligence");
    case Attr::None: break;
    }
    return Tr("attr_none");
}

const char* SetText(WeaponSet s) {
    switch (s) {
    case WeaponSet::One: return Tr("wset_one");
    case WeaponSet::Two: return Tr("wset_two");
    case WeaponSet::None: break;
    }
    return Tr("wset_none");
}

// What kind of passive an objective is, for the dim tag after its name.
const char* KindText(const Node& n) {
    if (n.Is(kNodeKeystone)) return Tr("passives_kind_keystone");
    if (n.Is(kNodeJewel)) return Tr("passives_kind_jewel");
    if (n.Is(kNodeNotable)) return Tr("passives_kind_notable");
    return "";
}

bool SameTextNoCase(const std::string& a, const char* b) {
    const auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; };
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return i == a.size() && !b[i];
}

// ---- the plan card --------------------------------------------------------------------------

void CountPill(const char* key, int n, ImU32 color, float lineRight, bool first) {
    const std::string text = TrF(key, {{"n", Num(n)}});
    if (!first) sui::SameLineIfFits(sui::PillSize(text.c_str()).x, lineRight);
    sui::Pill(text.c_str(), n > 0 ? color : sui::DimColor());
}

void PlanCard(App& app, const ActiveView& a, const PlanView& plan) {
    const sui::Card card("##bp_plan");
    PluginUi::Heading(ICON_FA_DIAGRAM_PROJECT, Tr("passives_title"), Tr("passives_help"));

    const Stage* stage = a.StageInfo();
    const std::string stageTitle = stage ? sui::TitleOr(stage->title, I18n::Key("common_tree_n"), stage->spec) : std::string();
    sui::Clipped(TrF("passives_following", {{"build", a.build->name}, {"stage", stageTitle}}), sui::AccentColor());

    // Who the plan compares with.
    const std::shared_ptr<const GameView> view = app.View();
    const CharState& c = *view->character;
    if (c.valid) {
        const std::string who = c.className.empty() ? std::string(Tr("passives_unknown_class"))
                                                    : (plan.ascendancyName.empty() || c.ascendancy.empty()
                                                           ? c.className : c.className + " / " + plan.ascendancyName);
        PluginUi::Hint(TrF("passives_character", {{"who", who}, {"level", Num(c.level)}}).c_str());
    } else {
        // Without the character every build passive reads as "to take": say so, and why.
        sui::InfoLine(TrF("passives_no_character", {{"why", CharProblemText(c.problem)}}));
    }

    if (!plan.valid) {
        sui::WarnLine(app.TreeStatus().loaded ? Tr("passives_no_plan") : Tr("passives_no_tree_data"));
        return;
    }
    const Plan& p = plan.plan;

    ImGui::Spacing();
    const float right = theme::LineRight();
    CountPill(I18n::Key("passives_take"), p.missing + p.ascMissing, sui::GoodColor(), right, true);
    CountPill(I18n::Key("passives_refund"), p.wrong + p.ascWrong, sui::BadColor(), right, false);
    CountPill(I18n::Key("passives_adjust"), p.adjust, sui::WarnColor(), right, false);
    CountPill(I18n::Key("passives_done"), p.ok + p.ascOk, sui::DimColor(), right, false);

    if (c.valid) {
        PluginUi::Hint(TrF("passives_points", {{"free", Num(c.freePoints)},
                                               {"set1", Num(c.freeSet1)},
                                               {"set2", Num(c.freeSet2)},
                                               {"asc", Num(c.freeAscPoints)}})
                           .c_str());
    }

    // What can make the plan misleading: each line names the cause and what to do.
    if (plan.classDiffers) sui::WarnLine(Tr("passives_class_differs"));
    if (plan.treeOlder) sui::WarnLine(Tr("passives_tree_older"));
    if (!p.unknownTarget.empty()) sui::WarnLine(TrF("passives_unknown_ids", {{"n", Num(static_cast<long long>(p.unknownTarget.size()))}}));
    // Only against a known character: without one nothing is "what you have", and passives a jewel
    // grants away from the tree would read as a fault of the build.
    if (p.unreachable > 0 && plan.charKnown) sui::WarnLine(TrF("passives_unreachable", {{"n", Num(p.unreachable)}}));

    // "Earlier trees first" only does something from the second stage on.
    ImGui::Spacing();
    bool tiers = app.Config().useTiers;
    bool changed = false;
    {
        const sui::Disabled disabled(a.sets.spec <= 0);
        changed = PluginUi::Check(Tr("passives_tiers"), "bpUseTiers", &tiers, Tr("passives_tiers_tip"));
    }
    if (changed) {
        app.EditConfig().useTiers = tiers;
        app.ConfigChanged();   // `a` and `plan` are stale from here on: nothing below reads them
    }
}

// ---- next objectives ------------------------------------------------------------------------

struct ObjectiveCols {
    float number = 0, points = 0;
    bool fromTree = false;
};

void ObjectiveTable(const char* id, const std::vector<ObjectiveRow>& rows, const PassiveTree& tree, const PlanView& plan,
                    const ActiveView& a, const ObjectiveCols& cols) {
    const sui::Table table(id, cols.fromTree ? 4 : 3, sui::kTableFlags);
    if (!table) return;
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, cols.number);
    ImGui::TableSetupColumn(TrId("passives_col_objective"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
    ImGui::TableSetupColumn(TrId("passives_col_points"), ImGuiTableColumnFlags_WidthFixed, cols.points);
    if (cols.fromTree) ImGui::TableSetupColumn(TrId("passives_col_from"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableHeadersRow();

    const size_t shown = (std::min)(rows.size(), kMaxObjectiveRows);
    for (size_t i = 0; i < shown; ++i) {
        const ObjectiveRow& row = rows[i];
        const ImU32 dim = sui::DimColor();
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (row.firstNumber > 0) ImGui::Text("%d", row.firstNumber);
        else ImGui::TextDisabled("-");

        ImGui::TableSetColumnIndex(1);
        if (row.node < tree.nodes.size()) {
            const Node& n = tree.nodes[row.node];
            const std::string& name = n.NameFor(plan.className, plan.ascendancyName);
            // "Jewel Socket  jewel socket" says nothing twice: no tag that only repeats the name.
            const char* kind = SameTextNoCase(name, KindText(n)) ? "" : KindText(n);
            // The kind tag and "not reachable yet" follow the name on the same line while they fit.
            const std::string tag = !row.reachable ? std::string(Tr("passives_not_reachable")) : std::string(kind);
            const float tagW = tag.empty() ? 0.0f : ImGui::CalcTextSize(tag.c_str()).x + ImGui::GetStyle().ItemSpacing.x;
            const float avail = ImGui::GetContentRegionAvail().x;
            const bool tagFits = !tag.empty() && ImGui::CalcTextSize(name.c_str()).x + tagW <= avail;
            sui::Clipped(name, row.reachable ? 0 : dim, tagFits ? avail - tagW : 0.0f);
            if (tagFits) {
                ImGui::SameLine();
                const sui::TextColor c(row.reachable ? dim : sui::WarnColor());
                ImGui::TextUnformatted(tag.c_str());
            }
        } else {
            sui::Clipped(Tr("passives_remaining"), dim);
        }

        ImGui::TableSetColumnIndex(2);
        if (row.freeSteps > 0) {
            ImGui::Text("%d +%d", row.cost, row.freeSteps);
            PluginUi::Tip(Tr("passives_free_steps_tip"));
        } else {
            ImGui::Text("%d", row.cost);
        }

        if (cols.fromTree) {
            ImGui::TableSetColumnIndex(3);
            // tier = the 1-based index of the earlier tree of the build that already had this objective.
            if (row.tier >= 1 && row.tier <= static_cast<int>(a.stages.size())) {
                const Stage& s = a.stages[static_cast<size_t>(row.tier - 1)];
                sui::Clipped(sui::TitleOr(s.title, I18n::Key("common_tree_n"), s.spec), dim);
            } else {
                ImGui::TextDisabled("-");
            }
        }
    }
    if (rows.size() > shown) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(1);
        sui::Clipped(TrF("common_and_more", {{"n", Num(static_cast<long long>(rows.size() - shown))}}), sui::DimColor());
    }
}

void ObjectivesCard(const ActiveView& a, const PlanView& plan, const PassiveTree& tree) {
    const sui::Card card("##bp_objectives");
    PluginUi::Heading(ICON_FA_LIST_OL, Tr("passives_next_title"), Tr("passives_next_help"));
    const Plan& p = plan.plan;
    const PlanNumbers& numbers = plan.numbers;

    const NextObjective next = PickNext(numbers.objectives, numbers.ascObjectives, p.missing, p.ascMissing);
    if (next.kind == NextKind::Done) {
        sui::IconLine(ICON_FA_CIRCLE_CHECK, Tr("passives_all_done"), sui::GoodColor());
        return;
    }
    if (next.kind == NextKind::None) sui::WarnLine(Tr("passives_none_reachable"));

    ObjectiveCols cols;
    cols.number = sui::MaxTextWidth({"#", "000"});
    cols.points = sui::MaxTextWidth({Tr("passives_col_points"), "00 +0"});
    // The "from tree" column only when the tiers put something into it.
    const auto anyTier = [](const std::vector<ObjectiveRow>& rows) {
        return std::any_of(rows.begin(), rows.end(), [](const ObjectiveRow& r) { return r.tier > 0; });
    };
    cols.fromTree = anyTier(numbers.objectives) || anyTier(numbers.ascObjectives);

    if (!numbers.objectives.empty()) ObjectiveTable("##bpObjectives", numbers.objectives, tree, plan, a, cols);
    if (!numbers.ascObjectives.empty()) {
        ImGui::Spacing();
        sui::Caption(plan.ascendancyName.empty() ? Tr("passives_ascendancy")
                                                 : TrF("passives_ascendancy_named", {{"name", plan.ascendancyName}}).c_str());
        ObjectiveTable("##bpAscObjectives", numbers.ascObjectives, tree, plan, a, cols);
    }
}

// ---- refund and adjust ----------------------------------------------------------------------

// Chips on as many lines as they need.
void ChipFlow(const std::vector<std::string>& texts, ImU32 color) {
    const float right = theme::LineRight();
    for (size_t i = 0; i < texts.size(); ++i) {
        if (i > 0 && sui::ChipFits(texts[i].c_str(), right)) ImGui::SameLine();
        sui::Chip(texts[i].c_str(), color);
    }
}

void ChangesCard(const PlanView& plan, const PassiveTree& tree) {
    const Plan& p = plan.plan;
    if (p.wrong + p.ascWrong + p.adjust == 0) return;
    ImGui::Spacing();
    const sui::Card card("##bp_changes");
    PluginUi::Heading(ICON_FA_ROTATE_LEFT, Tr("passives_changes_title"), Tr("passives_changes_help"));

    // Refunds: one chip per name with a count (small passives share names), notables first.
    if (p.wrong + p.ascWrong > 0) {
        std::map<std::string, std::pair<int, bool>> byName;   // name -> (count, is an objective)
        for (size_t i = 0; i < p.state.size() && i < tree.nodes.size(); ++i) {
            if (p.state[i] != NodeState::Wrong) continue;
            const Node& n = tree.nodes[i];
            auto& entry = byName[n.NameFor(plan.className, plan.ascendancyName)];
            ++entry.first;
            entry.second = entry.second || n.IsObjective();
        }
        std::vector<std::string> chips;
        for (const bool objectives : {true, false})
            for (const auto& [name, entry] : byName) {
                if (entry.second != objectives) continue;
                if (chips.size() >= kMaxRefundChips) break;
                chips.push_back(entry.first > 1 ? name + " x" + Num(entry.first) : name);
            }
        sui::Caption(TrF("passives_refund_list", {{"n", Num(p.wrong + p.ascWrong)}}).c_str());
        ChipFlow(chips, sui::BadColor());
        if (byName.size() > chips.size())
            PluginUi::Hint(TrF("common_and_more", {{"n", Num(static_cast<long long>(byName.size() - chips.size()))}}).c_str());
    }

    // Adjustments: the passive and what to change on it.
    if (p.adjust > 0) {
        if (p.wrong + p.ascWrong > 0) ImGui::Spacing();
        sui::Caption(TrF("passives_adjust_list", {{"n", Num(p.adjust)}}).c_str());
        if (const sui::Table table{"##bpAdjust", 2, sui::kTableFlags}) {
            ImGui::TableSetupColumn(TrId("passives_col_passive"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn(TrId("passives_col_change"), ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::TableHeadersRow();
            size_t rows = 0;
            for (size_t i = 0; i < p.state.size() && i < tree.nodes.size() && rows < kMaxAdjustRows; ++i) {
                if (p.state[i] != NodeState::Adjust) continue;
                ++rows;
                const Node& n = tree.nodes[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                sui::Clipped(n.NameFor(plan.className, plan.ascendancyName));
                ImGui::TableSetColumnIndex(1);
                const uint8_t bits = i < p.adjustBits.size() ? p.adjustBits[i] : 0;
                std::string change;
                if ((bits & kAdjustAttribute) && i < p.wantAttr.size() && i < p.haveAttr.size())
                    change = TrF("passives_change_attr", {{"want", AttrText(p.wantAttr[i])}, {"have", AttrText(p.haveAttr[i])}});
                if ((bits & kAdjustWeaponSet) && i < p.wantSet.size() && i < p.haveSet.size()) {
                    if (!change.empty()) change += "; ";
                    change += TrF("passives_change_set", {{"want", SetText(p.wantSet[i])}, {"have", SetText(p.haveSet[i])}});
                }
                sui::Wrapped(change, sui::WarnColor());
            }
        }
        if (static_cast<size_t>(p.adjust) > kMaxAdjustRows)
            PluginUi::Hint(TrF("common_and_more", {{"n", Num(p.adjust - static_cast<int>(kMaxAdjustRows))}}).c_str());
    }
}

// ---- attribute choices and weapon sets ------------------------------------------------------

void ChoicesCard(const BuildSpec& spec, const PassiveTree& tree) {
    const size_t attrs = spec.attrStr.size() + spec.attrDex.size() + spec.attrInt.size();
    // Attribute passives of the build it gives no choice for.
    size_t unassigned = 0;
    for (const uint16_t id : spec.nodes) {
        const Node* n = tree.Find(id);
        if (!n || !n->Is(kNodeAttribute)) continue;
        const auto in = [id](const std::vector<uint16_t>& list) { return std::binary_search(list.begin(), list.end(), id); };
        if (!in(spec.attrStr) && !in(spec.attrDex) && !in(spec.attrInt)) ++unassigned;
    }
    const size_t sets = spec.weaponSet1.size() + spec.weaponSet2.size();
    if (attrs + unassigned + sets == 0) return;

    ImGui::Spacing();
    const sui::Card card("##bp_choices");
    PluginUi::Heading(ICON_FA_SLIDERS, Tr("passives_choices_title"), Tr("passives_choices_help"));

    if (attrs + unassigned > 0) {
        sui::Caption(Tr("passives_attr_caption"));
        const float right = theme::LineRight();
        bool first = true;
        const auto chip = [&](Attr attr, size_t n) {
            if (n == 0) return;
            const std::string text = std::string(AttrText(attr)) + " x" + Num(static_cast<long long>(n));
            if (!first && sui::ChipFits(text.c_str(), right)) ImGui::SameLine();
            sui::Chip(text.c_str(), sui::Legible(theme::AttrColor(attr)));
            first = false;
        };
        chip(Attr::Str, spec.attrStr.size());
        chip(Attr::Dex, spec.attrDex.size());
        chip(Attr::Int, spec.attrInt.size());
        if (unassigned > 0) PluginUi::Hint(TrF("passives_attr_unassigned", {{"n", Num(static_cast<long long>(unassigned))}}).c_str());
    }
    if (sets > 0) {
        if (attrs + unassigned > 0) ImGui::Spacing();
        sui::Caption(Tr("passives_wset_caption"));
        PluginUi::Hint(TrF("passives_wset_counts", {{"one", Num(static_cast<long long>(spec.weaponSet1.size()))},
                                                    {"two", Num(static_cast<long long>(spec.weaponSet2.size()))}})
                           .c_str());
    }
}

} // namespace

void DrawTabPassives(App& app) {
    if (!app.Active().build) {
        sui::NoBuildCard(ICON_FA_DIAGRAM_PROJECT, Tr("passives_title"));
        return;
    }
    PlanCard(app, app.Active(), app.Plan());
    // Taken again after the plan card: its checkbox changes a setting the plan is built from.
    const ActiveView& a = app.Active();
    const PlanView& plan = app.Plan();
    const std::shared_ptr<const PassiveTree> tree = app.Tree();
    if (!a.build || !plan.valid || !tree) return;
    ImGui::Spacing();
    ObjectivesCard(a, plan, *tree);
    ChangesCard(plan, *tree);
    if (const BuildSpec* spec = a.Spec()) ChoicesCard(*spec, *tree);
}

} // namespace bp
