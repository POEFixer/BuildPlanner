// BuildPlanner - the Skills tab
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// TabSkills.cpp - the Skills tab: the skill set in effect, group by group: the
// main skill with its level, the group's label, the support gems as chips, and what the character
// already has of it when that is known.
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

// "Level 20, quality 10%" of a gem; "" when the build names neither.
std::string LevelText(int level, int quality) {
    if (level > 0 && quality > 0) return TrF("skills_level_quality", {{"level", std::to_string(level)}, {"quality", std::to_string(quality)}});
    if (level > 0) return TrF("skills_level", {{"level", std::to_string(level)}});
    if (quality > 0) return TrF("skills_quality", {{"quality", std::to_string(quality)}});
    return {};
}

// A support gem as a chip: an icon for what the character has of it, then the name.
void SupportChip(const SupportAdvice& support, const BuildGem* gem, float lineRight, bool first) {
    const char* icon = "";
    const char* state = "";
    switch (support.have) {
    case Have::Yes:
        icon = ICON_FA_CHECK " ";
        state = Tr("skills_support_have");
        break;
    case Have::No:
        icon = support.otherTier ? ICON_FA_ARROW_UP " " : ICON_FA_PLUS " ";
        state = support.otherTier ? "" : Tr("skills_support_add");
        break;
    case Have::Unknown: break;
    }
    const std::string text = std::string(icon) + support.name;
    if (!first && sui::ChipFits(text.c_str(), lineRight)) ImGui::SameLine();
    sui::Chip(text.c_str(), sui::Legible(theme::GemColor(support.color)));

    // The tip: the gem, its level in the build, and what to do about it.
    std::string tip = support.name;
    if (gem) {
        const std::string level = LevelText(gem->level, gem->quality);
        if (!level.empty()) tip += "\n" + level;
    }
    if (support.have == Have::No && support.otherTier)
        tip += "\n" + TrF("skills_support_upgrade", {{"have", support.haveName.empty() ? std::string(Tr("skills_other_tier")) : support.haveName}});
    else if (state[0]) tip += std::string("\n") + state;
    PluginUi::Tip(tip.c_str());
}

void GroupBlock(const GroupAdvice& g, const BuildSkillGroup* group, bool charKnown) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float right = theme::LineRight();

    // The verdict pill sits at the right edge; the name gets what is left of it.
    const char* pill = "";
    ImU32 pillColor = 0;
    if (g.have == Have::Yes) {
        pill = Tr("skills_have");
        pillColor = sui::GoodColor();
    } else if (g.have == Have::No && charKnown) {
        pill = Tr("skills_missing");
        pillColor = sui::WarnColor();
    }
    const float pillW = pill[0] ? sui::PillSize(pill).x + style.ItemSpacing.x : 0.0f;

    theme::ColorDot(sui::Legible(theme::GemColor(g.color)));
    ImGui::SameLine();
    const float nameX = ImGui::GetCursorPosX();
    const float nameRoom = right - ImGui::GetCursorScreenPos().x - pillW;
    const std::string name = sui::GemName(g.name);
    const float nameW = ImGui::CalcTextSize(name.c_str()).x;
    sui::Clipped(name, 0, (std::max)(sui::Em() * 3.0f, nameRoom));

    // Level, the author's label and "granted" follow the name while the line has room.
    const std::string level = LevelText(g.level, g.quality);
    float used = (std::min)(nameW, nameRoom);
    const auto tail = [&](const std::string& text, ImU32 color) {
        if (text.empty()) return;
        const float w = ImGui::CalcTextSize(text.c_str()).x + style.ItemSpacing.x;
        if (used + w > nameRoom) return;
        ImGui::SameLine();
        const sui::TextColor c(color);
        ImGui::TextUnformatted(text.c_str());
        used += w;
    };
    tail(level, sui::DimColor());
    if (!g.label.empty()) tail("- " + g.label, sui::DimColor());
    if (g.granted) tail(std::string("- ") + Tr("skills_granted"), sui::DimColor());

    if (pill[0]) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (std::max)(0.0f, right - ImGui::GetCursorScreenPos().x - sui::PillSize(pill).x));
        sui::Pill(pill, pillColor);
    }

    // The supports, under the name.
    ImGui::SetCursorPosX(nameX);
    if (g.supports.empty()) {
        const sui::TextColor c(sui::DimColor());
        ImGui::TextUnformatted(Tr("skills_no_supports"));
        return;
    }
    const sui::Group chips;
    const float chipRight = theme::LineRight();
    for (size_t i = 0; i < g.supports.size(); ++i) {
        const SupportAdvice& s = g.supports[i];
        const BuildGem* gem = (group && s.gem >= 0 && s.gem < static_cast<int>(group->gems.size())) ? &group->gems[static_cast<size_t>(s.gem)] : nullptr;
        SupportChip(s, gem, chipRight, i == 0);
    }
}

} // namespace

void DrawTabSkills(App& app) {
    const ActiveView& a = app.Active();
    const StoredBuild* build = a.build;
    if (!build) {
        sui::NoBuildCard(ICON_FA_WAND_MAGIC_SPARKLES, Tr("skills_title"));
        return;
    }
    const SkillAdvice& advice = app.Skills();
    const BuildSkillSet* set = a.SkillSet();
    const std::shared_ptr<const GameView> view = app.View();

    const sui::Card card("##bp_skills");
    PluginUi::Heading(ICON_FA_WAND_MAGIC_SPARKLES, Tr("skills_title"), Tr("skills_help"));
    bool granted = app.Config().showGrantedSkills;
    bool grantedChanged = false;
    if (!set) {
        PluginUi::Hint(Tr("skills_no_set"));
    } else {
        sui::Clipped(TrF("skills_set_line", {{"set", sui::TitleOr(set->title, I18n::Key("common_skill_set_n"), a.sets.skillSet)}}),
                     sui::AccentColor());

        // What the comparison stands on.
        const SkillsState& skills = *view->skills;
        if (advice.charKnown) {
            const int all = static_cast<int>(advice.groups.size());
            const int have = all - static_cast<int>(advice.missing.size());
            sui::IconLine(have == all ? ICON_FA_CIRCLE_CHECK : ICON_FA_CIRCLE_INFO,
                          TrF("skills_summary", {{"have", std::to_string(have)}, {"all", std::to_string(all)}}),
                          have == all && all > 0 ? sui::GoodColor() : sui::DimColor());
        } else if (!view->status.inGame) {
            sui::InfoLine(Tr("skills_not_in_game"));
        } else if (skills.problem != SkillsProblem::None && skills.problem != SkillsProblem::NotInGame) {
            sui::WarnLine(SkillsProblemText(skills.problem));
        }
        grantedChanged = PluginUi::Check(Tr("skills_show_granted"), "bpShowGranted", &granted, Tr("skills_show_granted_tip"));

        if (advice.groups.empty()) {
            PluginUi::Hint(Tr("skills_set_empty"));
        } else {
            for (size_t i = 0; i < advice.groups.size(); ++i) {
                const GroupAdvice& g = advice.groups[i];
                const sui::Id id(static_cast<int>(i));
                ImGui::Separator();
                const BuildSkillGroup* group =
                    (g.group >= 0 && g.group < static_cast<int>(set->groups.size())) ? &set->groups[static_cast<size_t>(g.group)] : nullptr;
                GroupBlock(g, group, advice.charKnown);
            }
        }
    }
    // After the last read of the advice: the setting rebuilds it.
    if (grantedChanged) {
        app.EditConfig().showGrantedSkills = granted;
        app.ConfigChanged();
    }
}

} // namespace bp
