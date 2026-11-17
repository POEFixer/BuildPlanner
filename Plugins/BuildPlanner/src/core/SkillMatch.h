#pragma once
// BuildPlanner - the character's skills and supports vs the build's skill set
// SkillMatch.h - the build's skill set against the character's skills and their socketed supports
// as one data source for the Skills panel marks, the "Build skills" window and the Skills
// tab. Pure, deterministic, no SDK, never throws.
//
// What a "group" shows: its main skill = the mainActive-th enabled non-support gem (the first
// non-support gem when that is disabled or out of range; a group without any active gem is left
// out), its supports = the group's enabled support gems, in PoB order. Disabled groups are left out;
// granted groups (GroupSource other than Gems) only when `includeGranted`.
//
// Matching a build gem to a character gem: by gem id (NormalizeGemId of BuildGem::gemId against
// CharGem::gemId: the same string in PoB and in the game); when the build gem has no id, by name
// (English, case-insensitive) against CharGem::name. Several groups with the same main skill: the
// first group takes the character's first such skill, the next group the next one.
//
// Sockets of a matched skill, each filled socket once, in this order:
//   1. InBuild    the socketed gem is one of the group's enabled gems OTHER than the main one: a
//                 support of the group, or an active gem the build keeps in a meta gem's socket (a
//                 curse inside Blasphemy is not "not in the build");
//   2. OtherTier  it is another tier of a support of the group that is not matched yet
//                 (SameGemOtherTier over GemData; without gem data: SplitGemTier of the two English
//                 names, when the character gem has one): "Multishot I" where the build has
//                 "Multishot II". Never across gems of one family ("Wildshards" is not a tier of
//                 "Multishot");
//   3. NotInBuild everything else.
// A support of the build is "to add" only when no socket matched it either way; one matched as
// OtherTier is "to upgrade".
//
// Threads: stateless; the App calls MatchSkills on the render thread when the selected skill set or
// the character's skills (SkillsState::skillsSerial) change.
#include <string>
#include <vector>

#include "GemData.h"
#include "SkillsReader.h"
#include "Types.h"

namespace bp {

enum class Have : uint8_t {
    Unknown = 0,   // the character's skills are not known (not in game)
    Yes,
    No
};

struct SupportAdvice {
    int gem = -1;                 // index into the group's BuildSkillGroup::gems
    std::string name;             // BuildGem::name
    Attr color = Attr::None;
    Have have = Have::Unknown;    // socketed into the matched character skill (this very gem)
    bool otherTier = false;       // have == No, but another tier of it is socketed ("Other tier")
    std::string haveName;         // otherTier: the name of the tier that is socketed (game language)
};

// One skill group of the build, ready to draw.
struct GroupAdvice {
    int group = -1;               // index into BuildSkillSet::groups
    int mainGem = -1;             // index into that group's gems (the main skill)
    std::string name;             // the main skill's name
    std::string label;            // the group's label ("" none)
    int level = 0, quality = 0;   // the main gem's
    Attr color = Attr::None;      // the main gem's colour (GemData; None unknown)
    bool granted = false;         // GroupSource != Gems
    Have have = Have::Unknown;    // the character has this skill
    int charSkill = -1;           // index into SkillsState::skills when have == Yes, else -1
    std::vector<SupportAdvice> supports;   // the group's enabled SUPPORT gems, PoB order
};

// What to mark on one support socket of one of the character's skills.
enum class SocketMark : uint8_t {
    None = 0,        // an empty socket, or a skill the build does not have
    InBuild,         // this gem is in the build's group (green tick)
    OtherTier,       // the build uses another tier of this support (amber arrow: "Other tier")
    NotInBuild       // this gem is not in the build's group (red cross)
};

// One skill of the character (parallel to SkillsState::skills).
struct CharSkillAdvice {
    int group = -1;                    // index into SkillAdvice::groups of the build group it matches, -1 none
    std::vector<SocketMark> sockets;   // one per socket of the skill (CharSkill::supports order)
    // Parallel to `sockets`: the index into that group's GroupAdvice::supports of the build support
    // the socket's gem stands for (InBuild: that gem; OtherTier: the tier the build wants); -1 for an
    // empty socket, NotInBuild, and InBuild through a non-support gem of the group.
    std::vector<int> socketWant;
    std::vector<int> add;              // indices into that group's supports: in the build, matched by NO
                                       // socket (neither InBuild nor OtherTier), PoB order
    std::vector<int> upgrade;          // indices into that group's supports: matched only as OtherTier
    std::vector<int> notInBuild;       // socket indices marked NotInBuild
    // The compact card beside a Skills panel block: "In the build" when all three lists are empty.
    bool Complete() const { return group >= 0 && add.empty() && upgrade.empty() && notInBuild.empty(); }
};

struct SkillAdvice {
    int skillSet = -1;                 // the skill set matched, -1 = the build has none
    bool charKnown = false;            // the character's skills were known
    std::vector<GroupAdvice> groups;   // the groups to show, PoB order
    std::vector<CharSkillAdvice> charSkills;   // parallel to SkillsState::skills (empty when !charKnown)
    std::vector<int> missing;          // indices into `groups`: skills the character does not have at all
};

// `skillSet` out of range (or -1) gives an empty advice. `skills` null or !skills->skillsKnown gives
// Have::Unknown everywhere and no charSkills. `gems` (may be null) supplies colours and tiers.
SkillAdvice MatchSkills(const BuildData& build, int skillSet, const SkillsState* skills, const GemData* gems,
                        bool includeGranted);

// Is `have` the gem `want` (see the matching rules above)? !have.present -> false.
bool SameGem(const BuildGem& want, const CharGem& have);

// Is `have` another TIER of the support `want` (rule 2 above)? false when it is the same gem, when
// either side has no tier, or when nothing tells (`gems` null and no English name to split).
bool OtherTierOf(const BuildGem& want, const CharGem& have, const GemData* gems);

} // namespace bp
