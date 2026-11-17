// BuildPlanner - the character's skills and supports vs the build's skill set
// SkillMatch.cpp - see SkillMatch.h.
//
// Decisions the header leaves open:
//  - BuildSkillGroup::mainActive indexes the group's non-support gems (Types.h). When that gem is
//    disabled or the index is out of range, the first ENABLED non-support gem is the main skill; a
//    group whose non-support gems are all disabled is left out (Path of Building skips it too).
//  - Gem ids are compared with "Metadata/Items/Gem/" read as "Metadata/Items/Gems/" (GemData.h's
//    NormalizeGemId rule, applied here directly) and otherwise byte for byte.
//  - InBuild: each build gem stands for one socket at most (a second socketed copy is NotInBuild).
//  - OtherTier through gem data needs BOTH gems in the table; else the English names decide: the
//    build gem's name (the table's when it knows the id) and the character gem's (the table's when it
//    knows the id, else its own name, which is English only on an English client: another language
//    simply finds no tier match). Tier bases are compared ASCII case-insensitively.
#include "SkillMatch.h"

#include <string_view>

namespace bp {
namespace {

constexpr std::string_view kGemSingular = "Metadata/Items/Gem/";
constexpr std::string_view kGemPlural = "Metadata/Items/Gems/";

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

// The id without its "Metadata/Items/Gem(s)/" folder, so both spellings compare equal.
std::string_view GemIdTail(std::string_view id) {
    if (id.substr(0, kGemPlural.size()) == kGemPlural) return id.substr(kGemPlural.size());
    if (id.substr(0, kGemSingular.size()) == kGemSingular) return id.substr(kGemSingular.size());
    return id;
}

bool SameGemId(std::string_view a, std::string_view b) {
    if (a.empty() || b.empty()) return false;
    const bool folderA = GemIdTail(a).size() != a.size(), folderB = GemIdTail(b).size() != b.size();
    return folderA == folderB && GemIdTail(a) == GemIdTail(b);
}

// The main skill of a group (see the top of this file), -1 none.
int MainGemOf(const BuildSkillGroup& g) {
    int ordinal = 0, firstEnabled = -1;
    for (size_t k = 0; k < g.gems.size(); ++k) {
        const BuildGem& gem = g.gems[k];
        if (gem.support) continue;
        if (ordinal == g.mainActive && gem.enabled) return static_cast<int>(k);
        if (gem.enabled && firstEnabled < 0) firstEnabled = static_cast<int>(k);
        ++ordinal;
    }
    return firstEnabled;
}

const GemInfo* InfoOfBuildGem(const GemData* gems, const BuildGem& g) {
    if (!gems || gems->Empty()) return nullptr;
    const GemInfo* info = g.gemId.empty() ? nullptr : gems->Find(g.gemId, g.variantId);
    if (!info && !g.name.empty()) info = gems->FindByName(g.name);
    return info;
}

} // namespace

bool SameGem(const BuildGem& want, const CharGem& have) {
    if (!have.present) return false;
    if (!want.gemId.empty()) return SameGemId(want.gemId, have.gemId);
    const std::string_view a = Trim(want.name), b = Trim(have.name);
    return !a.empty() && EqualsNoCase(a, b);
}

bool OtherTierOf(const BuildGem& want, const CharGem& have, const GemData* gems) {
    try {
        if (!have.present || SameGem(want, have)) return false;
        const GemInfo* w = InfoOfBuildGem(gems, want);
        const GemInfo* h = (gems && !gems->Empty() && !have.gemId.empty()) ? gems->Find(have.gemId) : nullptr;
        if (w && h) return w != h && SameGemOtherTier(*w, *h);
        std::string baseW, baseH;
        const int tierW = SplitGemTier(w ? std::string_view(w->name) : std::string_view(want.name), &baseW);
        const int tierH = SplitGemTier(h ? std::string_view(h->name) : std::string_view(have.name), &baseH);
        return tierW > 0 && tierH > 0 && tierW != tierH && !baseW.empty() && EqualsNoCase(baseW, baseH);
    } catch (...) {
        return false;
    }
}

SkillAdvice MatchSkills(const BuildData& build, int skillSet, const SkillsState* skills, const GemData* gems,
                        bool includeGranted) {
    SkillAdvice a;
    try {
        if (skillSet < 0 || static_cast<size_t>(skillSet) >= build.skillSets.size()) return a;
        a.skillSet = skillSet;
        const BuildSkillSet& set = build.skillSets[static_cast<size_t>(skillSet)];
        a.charKnown = skills && skills->skillsKnown;
        const Have unknownOrNo = a.charKnown ? Have::No : Have::Unknown;

        for (size_t gi = 0; gi < set.groups.size(); ++gi) {
            const BuildSkillGroup& g = set.groups[gi];
            if (!g.enabled) continue;
            const bool granted = g.source != GroupSource::Gems;
            if (granted && !includeGranted) continue;
            const int main = MainGemOf(g);
            if (main < 0) continue;
            const BuildGem& mg = g.gems[static_cast<size_t>(main)];
            GroupAdvice ga;
            ga.group = static_cast<int>(gi);
            ga.mainGem = main;
            ga.name = mg.name;
            ga.label = g.label;
            ga.level = mg.level;
            ga.quality = mg.quality;
            ga.color = GemColorOf(gems, mg.gemId, mg.name);
            ga.granted = granted;
            ga.have = unknownOrNo;
            for (size_t k = 0; k < g.gems.size(); ++k) {
                const BuildGem& gem = g.gems[k];
                if (static_cast<int>(k) == main || !gem.enabled || !gem.support) continue;
                SupportAdvice sa;
                sa.gem = static_cast<int>(k);
                sa.name = gem.name;
                sa.color = GemColorOf(gems, gem.gemId, gem.name);
                sa.have = unknownOrNo;
                ga.supports.push_back(std::move(sa));
            }
            a.groups.push_back(std::move(ga));
        }
        if (!a.charKnown) return a;

        // Groups -> the character's skills: the first unused skill with the group's main gem.
        const std::vector<CharSkill>& cs = skills->skills;
        a.charSkills.resize(cs.size());
        std::vector<uint8_t> used(cs.size(), 0);
        for (size_t gi = 0; gi < a.groups.size(); ++gi) {
            GroupAdvice& ga = a.groups[gi];
            const BuildGem& mg = set.groups[static_cast<size_t>(ga.group)].gems[static_cast<size_t>(ga.mainGem)];
            for (size_t ci = 0; ci < cs.size(); ++ci) {
                if (used[ci] || !SameGem(mg, cs[ci].gem)) continue;
                used[ci] = 1;
                ga.have = Have::Yes;
                ga.charSkill = static_cast<int>(ci);
                a.charSkills[ci].group = static_cast<int>(gi);
                break;
            }
            if (ga.have != Have::Yes) a.missing.push_back(static_cast<int>(gi));
        }

        // Sockets of every matched skill.
        for (size_t ci = 0; ci < cs.size(); ++ci) {
            CharSkillAdvice& adv = a.charSkills[ci];
            const CharSkill& sk = cs[ci];
            adv.sockets.assign(sk.supports.size(), SocketMark::None);
            adv.socketWant.assign(sk.supports.size(), -1);
            if (adv.group < 0) continue;
            GroupAdvice& ga = a.groups[static_cast<size_t>(adv.group)];
            const BuildSkillGroup& g = set.groups[static_cast<size_t>(ga.group)];
            auto supportIndexOf = [&ga](int gem) {
                for (size_t si = 0; si < ga.supports.size(); ++si)
                    if (ga.supports[si].gem == gem) return static_cast<int>(si);
                return -1;
            };
            std::vector<uint8_t> gemTaken(g.gems.size(), 0);
            // 1. InBuild
            for (size_t k = 0; k < sk.supports.size(); ++k) {
                if (!sk.supports[k].present) continue;
                for (size_t j = 0; j < g.gems.size(); ++j) {
                    if (static_cast<int>(j) == ga.mainGem || !g.gems[j].enabled || gemTaken[j]) continue;
                    if (!SameGem(g.gems[j], sk.supports[k])) continue;
                    gemTaken[j] = 1;
                    adv.sockets[k] = SocketMark::InBuild;
                    const int si = supportIndexOf(static_cast<int>(j));
                    adv.socketWant[k] = si;
                    if (si >= 0) ga.supports[static_cast<size_t>(si)].have = Have::Yes;
                    break;
                }
            }
            // 2. OtherTier, against the supports not matched yet
            for (size_t k = 0; k < sk.supports.size(); ++k) {
                if (!sk.supports[k].present || adv.sockets[k] != SocketMark::None) continue;
                for (size_t si = 0; si < ga.supports.size(); ++si) {
                    SupportAdvice& sup = ga.supports[si];
                    if (sup.have == Have::Yes || sup.otherTier) continue;
                    if (!OtherTierOf(g.gems[static_cast<size_t>(sup.gem)], sk.supports[k], gems)) continue;
                    sup.otherTier = true;
                    sup.haveName = sk.supports[k].name;
                    adv.sockets[k] = SocketMark::OtherTier;
                    adv.socketWant[k] = static_cast<int>(si);
                    break;
                }
            }
            // 3. NotInBuild
            for (size_t k = 0; k < sk.supports.size(); ++k)
                if (sk.supports[k].present && adv.sockets[k] == SocketMark::None) {
                    adv.sockets[k] = SocketMark::NotInBuild;
                    adv.notInBuild.push_back(static_cast<int>(k));
                }
            for (size_t si = 0; si < ga.supports.size(); ++si) {
                const SupportAdvice& sup = ga.supports[si];
                if (sup.have == Have::Yes) continue;
                (sup.otherTier ? adv.upgrade : adv.add).push_back(static_cast<int>(si));
            }
        }
    } catch (...) {
        a = SkillAdvice{};
    }
    return a;
}

} // namespace bp
