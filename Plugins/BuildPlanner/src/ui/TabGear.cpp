// BuildPlanner - the Gear tab
// BP_TESTS_EXCLUDE: ImGui + the plugin SDK; not built into tests/.
// TabGear.cpp - the Gear tab: the gear set in effect, slot by slot: the build's item,
// what the character wears and the verdict; a row opens to the item's mods. Jewels and the other
// slots the game has no equipment slot for follow in a second card.
//
// The list is a measured grid drawn by hand, not an ImGui table: an opened row must span the columns
// right of the slot name, and on a narrow page the "equipped" column moves into the opened row.
#include <string>
#include <unordered_set>
#include <vector>

#include "GearShown.h"
#include "SettingsCommon.h"
#include "Theme.h"
#include "Ui.h"

namespace bp {
namespace {

using I18n::Tr;
using I18n::TrF;
using I18n::TrId;

// Which rows are open: item indices of the build named by g_openOwner (a pin of the gear set or
// another build starts with every row closed).
std::unordered_set<int> g_open;
std::string g_openOwner;

struct GearRow {
    std::string label;                           // the slot's name
    int item = -1;                               // index into BuildData::items
    bool hasVerdict = false;                     // an equipment slot (the other rows have no verdict)
    SlotMatch match = SlotMatch::NoBuildItem;
    // Match, but only by the base type: a rare or magic item under another name than the build's
    // (shown::SameBaseOnly, the build item card's rule too). "Worn" beside another name would read as
    // a mistake, and the mods are the player's to compare.
    bool sameBase = false;
    std::string equipped;                        // what the character wears there ("" nothing / unknown)
};

const char* VerdictText(SlotMatch m, bool sameBase = false) {
    switch (m) {
    case SlotMatch::Match: return sameBase ? Tr("gear_verdict_base") : Tr("gear_verdict_match");
    case SlotMatch::Differs: return Tr("gear_verdict_differs");
    case SlotMatch::Empty: return Tr("gear_verdict_empty");
    case SlotMatch::Unknown: return Tr("gear_verdict_unknown");
    case SlotMatch::NoBuildItem: break;
    }
    return "";
}

const char* VerdictTip(SlotMatch m, bool sameBase = false) {
    switch (m) {
    case SlotMatch::Match: return sameBase ? Tr("gear_verdict_base_tip") : Tr("gear_verdict_match_tip");
    case SlotMatch::Differs: return Tr("gear_verdict_differs_tip");
    case SlotMatch::Empty: return Tr("gear_verdict_empty_tip");
    case SlotMatch::Unknown: return Tr("gear_verdict_unknown_tip");
    case SlotMatch::NoBuildItem: break;
    }
    return "";
}

ImU32 VerdictColor(SlotMatch m) {
    switch (m) {
    case SlotMatch::Match: return sui::GoodColor();
    case SlotMatch::Differs:
    case SlotMatch::Empty: return sui::WarnColor();
    default: return sui::DimColor();
    }
}

const char* RarityText(Rarity r) {
    switch (r) {
    case Rarity::Magic: return Tr("rarity_magic");
    case Rarity::Rare: return Tr("rarity_rare");
    case Rarity::Unique: return Tr("rarity_unique");
    case Rarity::Relic: return Tr("rarity_relic");
    case Rarity::Normal: break;
    }
    return Tr("rarity_normal");
}

// The opened part of a row: what kind of item it is, then its lines as the item shows them.
void ItemDetails(const BuildItem& item, const GearRow& row, bool showEquipped, bool showVerdict) {
    const ImU32 dim = sui::DimColor();
    // The lines sit close together, like on an item: a quarter of the usual spacing.
    const ImGuiStyle& style = ImGui::GetStyle();
    const sui::ItemSpacing tight(ImVec2(style.ItemSpacing.x, PluginUi::SnapPx(style.ItemSpacing.y * 0.25f)));
    const float ruleGap = PluginUi::SnapPx(sui::Em() * 0.45f);
    ImGui::PushTextWrapPos(0.0f);
    {
        // "Rare Grand Regalia", then the facts that are known, on one wrapped line.
        std::string facts = RarityText(item.rarity);
        if (!item.name.empty() && !item.base.empty()) facts += " " + item.base;
        const auto add = [&facts](const std::string& part) { facts += "  -  " + part; };
        if (item.quality > 0) add(TrF("gear_quality", {{"n", std::to_string(item.quality)}}));
        // A count, as the build item card and the game show it, never PoB's notation ("S S").
        if (const int sockets = shown::CountSockets(item.sockets); sockets > 0) add(TrF("gear_sockets", {{"n", std::to_string(sockets)}}));
        if (item.itemLevel > 0) add(TrF("gear_item_level", {{"n", std::to_string(item.itemLevel)}}));
        if (item.levelReq > 0) add(TrF("gear_level_req", {{"n", std::to_string(item.levelReq)}}));
        if (item.corrupted) add(Tr("gear_corrupted"));
        const sui::TextColor c(dim);
        ImGui::TextUnformatted(facts.c_str());
    }
    if (!item.runes.empty()) {
        std::string runes;
        for (const std::string& r : item.runes) runes += (runes.empty() ? "" : ", ") + r;
        const sui::TextColor c(sui::Legible(theme::kEnchant));
        ImGui::TextUnformatted(TrF("gear_runes", {{"runes", runes}}).c_str());
    }
    for (const ItemModLine& line : item.implicits) {
        const sui::TextColor c(sui::Legible(theme::ModLineColor(line.tags)));
        ImGui::TextUnformatted(line.text.c_str());
    }
    if (!item.implicits.empty() && !item.explicits.empty()) {
        // A short rule between the implicit and the explicit block, as on the item itself.
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float y = PluginUi::SnapPx(pos.y + ruleGap * 0.5f);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(pos.x, y), ImVec2(pos.x + sui::Em() * 8.0f, y), theme::WithAlpha(dim, 0.45f));
        ImGui::Dummy(ImVec2(1.0f, ruleGap));
    }
    for (const ItemModLine& line : item.explicits) {
        const sui::TextColor c(sui::Legible(theme::ModLineColor(line.tags)));
        ImGui::TextUnformatted(line.text.c_str());
    }
    if (item.implicits.empty() && item.explicits.empty()) {
        const sui::TextColor c(dim);
        ImGui::TextUnformatted(Tr("gear_no_mods"));
    }
    if (showEquipped && row.hasVerdict) {
        // The "You wear" column of the wide page, as a line of the narrow one.
        ImGui::Dummy(ImVec2(1.0f, ruleGap * 0.5f));
        if (showVerdict && row.match != SlotMatch::NoBuildItem) {
            const sui::TextColor c(VerdictColor(row.match));
            ImGui::TextUnformatted(VerdictTip(row.match, row.sameBase));
        }
        const sui::TextColor c(dim);
        if (!row.equipped.empty()) ImGui::TextUnformatted(TrF("gear_equipped_line", {{"name", row.equipped}}).c_str());
        else if (!showVerdict) ImGui::TextUnformatted(row.match == SlotMatch::Empty ? Tr("gear_equipped_nothing") : Tr("gear_equipped_unknown"));
    }
    ImGui::PopTextWrapPos();
}

// The grid: [arrow + slot] [build item] ([equipped]) ([verdict]).
void GearGrid(const char* id, const std::vector<GearRow>& rows, const StoredBuild& build, bool verdicts, bool openAll) {
    const sui::Id scope(id);
    const ImGuiStyle& style = ImGui::GetStyle();
    const float lineH = ImGui::GetTextLineHeight();
    const float padX = style.CellPadding.x, padY = style.CellPadding.y;
    const float gap = style.ItemSpacing.x * 2.0f;
    const float x0 = ImGui::GetCursorPosX();
    const float full = ImGui::GetContentRegionAvail().x;
    const float inner = full - padX * 2.0f;

    const float arrowW = ImGui::CalcTextSize(ICON_FA_CHEVRON_RIGHT).x + style.ItemSpacing.x;
    float labelW = ImGui::CalcTextSize(Tr("gear_col_slot")).x;
    for (const GearRow& r : rows) labelW = (std::max)(labelW, ImGui::CalcTextSize(r.label.c_str()).x);
    labelW = (std::min)(labelW, inner * 0.34f);   // a long translated slot name is cut, not the item
    float verdictW = 0.0f;
    if (verdicts) {
        verdictW = ImGui::CalcTextSize(Tr("gear_col_verdict")).x;
        for (const SlotMatch m : {SlotMatch::Match, SlotMatch::Differs, SlotMatch::Empty, SlotMatch::Unknown})
            verdictW = (std::max)(verdictW, sui::PillSize(VerdictText(m)).x);
        verdictW = (std::max)(verdictW, sui::PillSize(VerdictText(SlotMatch::Match, true)).x);
    }
    // The "equipped" column needs a wide page; on a narrow one that line moves into the opened row.
    const bool wide = verdicts && full >= sui::Em() * 44.0f;
    const float itemX = padX + arrowW + labelW + gap;
    // And where even the verdict pill would squeeze the item's name, the row shows a dot in the
    // verdict's colour and the opened row says it in words.
    const bool dots = verdicts && !wide && padX + inner - verdictW - gap - itemX < sui::Em() * 10.0f;
    if (dots) verdictW = lineH;
    const float verdictX = padX + inner - verdictW;
    const float middle = (verdicts ? verdictX - gap : padX + inner) - itemX;
    const float itemW = (std::max)(sui::Em() * 4.0f, wide ? middle * 0.56f - gap * 0.5f : middle);
    const float equippedX = itemX + itemW + gap;
    const float equippedW = wide ? (std::max)(sui::Em() * 3.0f, verdictX - gap - equippedX) : 0.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = lineH + padY * 2.0f;
    // A row's background, from the cursor (at the row's top-left) over the grid's full width.
    const auto rowFill = [&](ImU32 color) {
        const ImVec2 a = PluginUi::SnapPx(ImGui::GetCursorScreenPos());
        dl->AddRectFilled(a, ImVec2(PluginUi::SnapPx(a.x + full), PluginUi::SnapPx(a.y + rowH)), color);
    };

    // The header, like a table's.
    {
        const float y = ImGui::GetCursorPosY();
        rowFill(ImGui::GetColorU32(ImGuiCol_TableHeaderBg));
        const auto head = [&](float x, const char* text, float width) {
            ImGui::SetCursorPos(ImVec2(x0 + x, y + padY));
            sui::Clipped(text, 0, width);
        };
        head(padX + arrowW, Tr("gear_col_slot"), labelW);
        head(itemX, Tr("gear_col_item"), itemW);
        if (wide) head(equippedX, Tr("gear_col_equipped"), equippedW);
        if (verdicts && !dots) head(verdictX, Tr("gear_col_verdict"), verdictW);
        ImGui::SetCursorPos(ImVec2(x0, y + rowH));
    }

    int index = 0;
    for (const GearRow& row : rows) {
        if (row.item < 0 || row.item >= static_cast<int>(build.items.size())) continue;
        const BuildItem& item = build.items[static_cast<size_t>(row.item)];
        const sui::Id rowId(index);
        const bool open = openAll || g_open.count(row.item) != 0;
        const float y = ImGui::GetCursorPosY();
        if (index % 2 == 1) rowFill(ImGui::GetColorU32(ImGuiCol_TableRowBgAlt));

        // The whole line is the button that opens the row.
        if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(full, rowH))) {
            if (open) g_open.erase(row.item);
            else g_open.insert(row.item);
        }

        ImGui::SetCursorPos(ImVec2(x0 + padX, y + padY));
        {
            const sui::TextColor c(sui::DimColor());
            ImGui::TextUnformatted(open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT);
        }
        ImGui::SetCursorPos(ImVec2(x0 + padX + arrowW, y + padY));
        sui::Clipped(row.label, 0, labelW);
        ImGui::SetCursorPos(ImVec2(x0 + itemX, y + padY));
        sui::Clipped(item.DisplayName(), sui::Legible(theme::RarityColor(item.rarity)), itemW);
        if (wide) {
            ImGui::SetCursorPos(ImVec2(x0 + equippedX, y + padY));
            if (row.equipped.empty()) ImGui::TextDisabled("-");
            else sui::Clipped(row.equipped, sui::DimColor(), equippedW);
        }
        if (verdicts && row.hasVerdict && row.match != SlotMatch::NoBuildItem) {
            ImGui::SetCursorPos(ImVec2(x0 + verdictX, y + padY));
            if (dots) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(lineH, lineH));
                dl->AddCircleFilled(ImVec2(PluginUi::SnapPx(at.x + lineH * 0.5f), PluginUi::SnapPx(at.y + lineH * 0.5f)), lineH * 0.26f,
                                    VerdictColor(row.match), 12);
            } else {
                sui::Pill(VerdictText(row.match, row.sameBase), VerdictColor(row.match));
            }
            PluginUi::Tip(VerdictTip(row.match, row.sameBase));
        }
        ImGui::SetCursorPos(ImVec2(x0, y + rowH));

        if (open) {
            // The details hang under the item column and wrap at the grid's right edge; on a narrow
            // page they start under the slot name instead, so a mod line keeps a readable width.
            ImGui::SetCursorPos(ImVec2(x0 + (wide || !verdicts ? itemX : padX + arrowW), y + rowH + padY));
            {
                const sui::Group group;
                ItemDetails(item, row, !wide, dots);
            }
            ImGui::SetCursorPos(ImVec2(x0, ImGui::GetCursorPosY() + padY));
        }
        ++index;
    }
    // Leave the cursor where an item of the grid's full width would have: the card measures by it.
    ImGui::SetCursorPosX(x0);
    ImGui::Dummy(ImVec2(full, 0.0f));
}

std::string SetTitle(const StoredBuild& build, int itemSet) {
    if (itemSet < 0 || itemSet >= static_cast<int>(build.itemSets.size())) return {};
    return sui::TitleOr(build.itemSets[static_cast<size_t>(itemSet)].title, I18n::Key("common_gear_set_n"), itemSet);
}

} // namespace

void DrawTabGear(App& app) {
    const ActiveView& a = app.Active();
    const StoredBuild* build = a.build;
    if (!build) {
        sui::NoBuildCard(ICON_FA_SHIELD_HALVED, Tr("gear_title"));
        return;
    }
    const GearAdvice& advice = app.Gear();
    const std::shared_ptr<const GameView> view = app.View();
    const bool openAll = app.DemoMode() && app.DemoOpenAll();

    // Open rows belong to one gear set of one build.
    const std::string owner = build->id + "/" + std::to_string(a.sets.itemSet);
    if (owner != g_openOwner) {
        g_openOwner = owner;
        g_open.clear();
    }

    std::vector<GearRow> worn, other;
    for (const GearSlotInfo& info : kGearSlots) {
        const SlotAdvice& slot = advice.Slot(info.slot);
        if (slot.itemIndex < 0) continue;
        GearRow row;
        row.label = GearSlotText(info.slot);
        row.item = slot.itemIndex;
        row.hasVerdict = true;
        row.match = slot.match;
        row.equipped = slot.equipped;
        // The rule the build item card over the game uses too (GearShown.h).
        if (slot.itemIndex < static_cast<int>(build->items.size()))
            row.sameBase = shown::SameBaseOnly(build->items[static_cast<size_t>(slot.itemIndex)], slot.match, slot.equipped);
        worn.push_back(std::move(row));
    }
    // Every socket of the tree is named "Jewel Socket": tree jewels are numbered once, in the stage's
    // tree order (its sockets that hold a jewel, sorted by node id), whether the item set names one
    // ("Jewel <node id>") or only the tree does; an id the tree does not list follows after them.
    std::vector<uint16_t> jewelIds;
    if (const BuildSpec* spec = a.Spec())
        for (const SpecSocket& s : spec->sockets)
            if (s.itemIndex >= 0) jewelIds.push_back(s.nodeId);
    const auto jewelNumber = [&jewelIds](int nodeId) {
        for (size_t i = 0; i < jewelIds.size(); ++i)
            if (jewelIds[i] == nodeId) return static_cast<int>(i) + 1;
        jewelIds.push_back(static_cast<uint16_t>(nodeId));
        return static_cast<int>(jewelIds.size());
    };
    if (const BuildItemSet* set = a.ItemSet()) {
        // Slots Path of Building names but the game has no equipment slot for (jewels, limbs): in the
        // program language like every other slot name, PoB's own identifier only when it is none of
        // PoB's patterns (shown::ParsePobSlotName).
        for (const BuildItemSlot& s : set->slots) {
            if (s.itemIndex < 0 || GearSlotFromPobName(s.name) != kNoGearSlot) continue;
            GearRow row;
            const shown::PobSlotName pob = shown::ParsePobSlotName(s.name);
            const std::string n = std::to_string(pob.number);
            switch (pob.kind) {
            case shown::PobSlotName::Kind::TreeJewel: row.label = TrF("gear_tree_jewel", {{"n", std::to_string(jewelNumber(pob.number))}}); break;
            case shown::PobSlotName::Kind::ItemJewel: row.label = TrF("gear_item_jewel", {{"slot", GearSlotText(pob.slot)}, {"n", n}}); break;
            case shown::PobSlotName::Kind::Arm: row.label = TrF("gear_slot_arm", {{"n", n}}); break;
            case shown::PobSlotName::Kind::Leg: row.label = TrF("gear_slot_leg", {{"n", n}}); break;
            case shown::PobSlotName::Kind::Raw: row.label = s.name; break;
            }
            row.item = s.itemIndex;
            other.push_back(std::move(row));
        }
    }
    if (const BuildSpec* spec = a.Spec()) {
        // Jewels socketed in the stage's tree that the item set did not list already.
        for (const SpecSocket& s : spec->sockets) {
            if (s.itemIndex < 0) continue;
            bool listed = false;
            for (const GearRow& r : other) listed = listed || r.item == s.itemIndex;
            if (listed) continue;
            GearRow row;
            row.label = TrF("gear_tree_jewel", {{"n", std::to_string(jewelNumber(s.nodeId))}});
            row.item = s.itemIndex;
            other.push_back(std::move(row));
        }
    }

    {
        const sui::Card card("##bp_gear");
        PluginUi::Heading(ICON_FA_SHIELD_HALVED, Tr("gear_title"), Tr("gear_help"));
        if (a.sets.itemSet < 0) {
            PluginUi::Hint(Tr("gear_no_set"));
        } else {
            sui::Clipped(TrF("gear_set_line", {{"set", SetTitle(*build, a.sets.itemSet)}}), sui::AccentColor());
            // What the comparison stands on.
            const GearState& gear = *view->gear;
            if (gear.itemsKnown && advice.withItem > 0) {
                const bool all = advice.matching == advice.withItem;
                sui::IconLine(all ? ICON_FA_CIRCLE_CHECK : ICON_FA_CIRCLE_INFO,
                              TrF("gear_summary", {{"worn", std::to_string(advice.matching)}, {"all", std::to_string(advice.withItem)}}),
                              all ? sui::GoodColor() : sui::DimColor());
            } else if (!view->status.inGame) {
                sui::InfoLine(Tr("gear_not_in_game"));
            } else if (gear.problem != GearProblem::None && gear.problem != GearProblem::NotInGame) {
                sui::WarnLine(GearProblemText(gear.problem));
            } else if (!gear.itemsKnown) {
                sui::InfoLine(Tr("gear_open_inventory"));
            }
            if (worn.empty()) {
                PluginUi::Hint(Tr("gear_set_empty"));
            } else {
                ImGui::Spacing();
                GearGrid("worn", worn, *build, true, openAll);
                PluginUi::Hint(Tr("gear_row_hint"));
            }
        }
    }
    if (!other.empty()) {
        ImGui::Spacing();
        const sui::Card card("##bp_gear_other");
        PluginUi::Heading(ICON_FA_GEM, Tr("gear_other_title"), Tr("gear_other_help"));
        GearGrid("other", other, *build, false, openAll);
    }
}

} // namespace bp
