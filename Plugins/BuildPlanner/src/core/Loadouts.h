#pragma once
// BuildPlanner - the stages of a build
// Loadouts.h - the stages of a build: which skill set and gear set belong to which passive tree
// (Path of Building's loadout rules: equal titles, or a {n} tag in the titles), level ranges in
// titles, and the
// automatic choice of the stage to follow. Pure, deterministic, no SDK, never throws.
//
// A "stage" is one tree of the build (index = BuildData::specs index) with its linked sets.
// Linking, as Path of Building does it: (a) a `{n}` or `{n,m}` tag in the titles (only [A-Za-z0-9,]
// inside the braces): sets whose tags share an id belong together; else (b) equal titles (compared
// after trimming); a build with exactly one skill set (or one item set) links that set to every stage.
// No link -> -1 (the caller falls back to Path of Building's active set).
//
// Threads: stateless; safe from any thread (the App calls it on the render thread).
#include <cstdint>
#include <string>
#include <vector>

#include "Types.h"

namespace bp {

struct PassiveTree;   // TreeData.h

struct Stage {
    int spec = -1;               // index into BuildData::specs (== this stage's index)
    int skillSet = -1;           // linked skill set, -1 none
    int itemSet = -1;            // linked item set, -1 none
    std::string title;           // the tree's title with any {..} tag removed and trimmed ("" untitled)
    bool hasLevels = false;      // the title names a level or a level range
    int levelLo = 0, levelHi = 0;   // valid when hasLevels (1..100, lo <= hi)
    // The passive points the tree spends on the main tree (weapon set points included), as Path of
    // Building's CountAllocNodes and the game count them: with the tree data (BuildStages' `tree`),
    // the nodes the data knows that cost a point (Node::CostsPoint: no start, no isFreeAllocate node,
    // no multiple-choice option) and are no ascendancy node. Without the tree data (the
    // old count, kept for callers that have none): every node outside `neverListed`.
    int points = 0;
    int ascPoints = 0;           // likewise the ascendancy points; 0 without the tree data
};

// One Stage per tree, in PoB order. Empty for a build without trees. `neverListed` (may be null): the
// ids that are in a PoB tree but are no allocated points (NeverListedIds, sorted ascending); null
// counts every node. `tree` (may be null): the tree data that `points` / `ascPoints` are counted with.
std::vector<Stage> BuildStages(const BuildData& build, const std::vector<uint16_t>* neverListed = nullptr,
                               const PassiveTree* tree = nullptr);

// The ids of the build's trees that the character's allocated list can never hold, for
// StageContext::neverListed and BuildStages: PassiveTree::StartNodeIds (Path of
// Building lists the class and ascendancy starts, the game never does) and every id the tree data
// does not know (PassiveTree::IndexOf) in a tree made for an OLDER version than the data
// (BuildSpec::treeVersion below PassiveTree::treeVersion): such a tree names passives the game no
// longer has. An unknown id of a tree of the same or a newer version (or of no stated version) is NOT
// listed: the data may be behind the game, the character can take that passive (CharReader publishes
// ids the data lacks), and the stage waits for it. Sorted ascending, unique. Empty when the tree data
// is empty (nothing can be judged unknown then). Never throws.
std::vector<uint16_t> NeverListedIds(const PassiveTree& tree, const BuildData& build);

// PoB titles of leveling builds carry level ranges: "Lvl 01-30 {1}", "Lvl 41-60", "Level 90",
// "lvl 95+", "Act 1-3" (no level -> false). Parses the FIRST level range or single level:
// "Lvl 41-60" -> [41, 60]; "Lvl 90" -> [90, 90]; "Lvl 95+" -> [95, 100]. Case-insensitive, prefixes
// "level" / "lvl" / "lv" at a word start, optional spaces / dots, dash forms '-', '~', en and em dash.
// Clamped to 1..100, swapped when reversed. (POE1 PassiveTreePlanner's rule, unchanged.)
bool ParseLevelRange(const std::string& title, int& lo, int& hi);

// The index to follow for a character level among titles with level ranges (POE1's rule, unchanged):
//   - the title whose range contains `level`; if several, the NARROWEST, the first on ties;
//   - none contains it: P = the LAST index whose range ends below the level; the answer is P + 1 (the
//     tree the author put next, which may be an untitled one), P itself only when it is the last;
//   - the level is below every range: the index with the lowest lo, the first on ties;
//   - -1 only when no title names a level.
int SpecForLevel(const std::vector<std::string>& titles, int level);

enum class StageHow : uint8_t {
    None = 0,          // the build has no tree
    Pinned,            // the user pinned it (StoredBuild::selectedSpec)
    ByLevel,           // titles carry level ranges and the character's level is known
    FirstUnfinished,   // the first tree in PoB order the character has not finished
    PobActive,         // Path of Building's active tree
    Last               // nothing else applied: the last tree
};

struct StageChoice {
    int stage = -1;              // index into the stages, -1 only with StageHow::None
    StageHow how = StageHow::None;
};

// What the automatic choice needs to know about the character.
struct StageContext {
    bool followLevel = true;     // the "Follow my level" setting (Settings::autoStageByLevel)
    int level = 0;               // the character's level, 0 = unknown
    // The character's allocated passive ids, sorted ascending; nullptr = unknown (not in game).
    const std::vector<uint16_t>* allocated = nullptr;
    // Ids that Path of Building lists in a tree but the game never lists as allocated (NeverListedIds:
    // the class and ascendancy starts and the ids the tree data does not know), sorted ascending;
    // nullptr = none. An id missing here that the game can never list keeps its tree unfinished.
    const std::vector<uint16_t>* neverListed = nullptr;
};

// The stage to follow ("Automatic"):
//   1. the user's pin when it names an existing tree -> Pinned;
//   2. followLevel, a known level (1..100) and titles with levels -> SpecForLevel -> ByLevel, but only
//      when the level falls inside or after a titled range (some title's lo <= level), OR at least two
//      titles name levels. A single titled stage the character has not reached yet decides nothing:
//      "01 | Act 1" .. "07 | Red Maps", "08 | Level 95" at level 30 falls through to step 3 (SpecForLevel's
//      "below every range: the earliest range" would put a level-30 character on the level-95 tree);
//   3. `allocated` known -> the first tree, in PoB order, that has a node (outside neverListed) the
//      character has not allocated -> FirstUnfinished; when every tree is finished: the last tree;
//   4. Path of Building's active tree when valid -> PobActive;
//   5. the last tree -> Last.
StageChoice ChooseStage(const StoredBuild& build, const std::vector<Stage>& stages, const StageContext& ctx);

// "done / current / later" of the stage table: before the chosen stage, the chosen one, after it.
enum class StageState : uint8_t { Done = 0, Current, Later };
inline StageState StateOfStage(int stage, int chosen) {
    return stage < chosen ? StageState::Done : stage == chosen ? StageState::Current : StageState::Later;
}

// The tree, skill set and gear set in effect for a stage.
struct ActiveSets {
    int spec = -1;               // == the stage
    int skillSet = -1;           // -1 = the build has no skill set
    int itemSet = -1;            // -1 = the build has no item set
    bool skillSetPinned = false; // the user's override (StoredBuild::selectedSkillSet) is in effect
    bool itemSetPinned = false;
};

// Skill set / item set for `stage`: the user's override when it names an existing set; else the
// stage's link; else Path of Building's active set when valid; else set 0 when the build has any;
// else -1. `stage` out of range gives spec -1 and the same set rules without a link.
ActiveSets ResolveSets(const StoredBuild& build, const std::vector<Stage>& stages, int stage);

} // namespace bp
