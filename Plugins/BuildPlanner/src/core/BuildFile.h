#pragma once
// BuildPlanner - the game's own Build Planner .build file
// BuildFile.h - writes a stage of a build as a file of the game's own Build Planner:
// PoE2 reads *.build JSON files from Documents\My Games\Path of Exile 2\BuildPlanner and shows them
// natively. Schema: GGG's developer documentation (pathofexile.com/developer/docs/game, "Build
// Planner"); Path of Building (PoE2)'s writer: src/Modules/BuildExportPoE2.lua. No SDK, never throws.
//
// One file per build: "POEFixer - <build name>.build", for the stage in effect. The plugin only ever
// writes or removes files whose name starts with kBuildFilePrefix and ends with ".build" AND whose
// content carries our marker (IsOurBuildFile): any other file in that folder is never touched.
//
// Mapping (as Path of Building does it):
//   name        "<build name> - <stage title>" (the stage title left out when empty)
//   author      kBuildFileAuthor (the marker)
//   ascendancy  BuildSpec::ascendancyId (left out when "")
//   passives    one entry per allocated node the tree data knows and that is Allocatable:
//               { "id": Node::stringId, "weapon_set": 1 | 2 (left out for ordinary points),
//                 "additional_text": the author's note (left out when none) }; on an attribute node
//                 the build picks an attribute for, additional_text also names it ("<red>{+5 to
//                 Strength}", green Dexterity, blue Intelligence; after the note, on its own line)
//   skills      one entry per shown group (SkillMatch's rule for a group's main skill and supports,
//               granted groups left out): { "id": gemId, "additional_text": "Level N, Q% Quality",
//               "support_skills": [ gemId, ... ] }; gems without a gemId are left out
//   inventory_slots  one entry per GearSlot with a build item: { "inventory_id": GearSlotInfo::
//               inventoryId, "slot_x": cellX (left out when 0), "unique_name" for a unique,
//               "additional_text": a rarity-coloured header and the mod lines, braces removed }
//
// Threads: BuildFileJson and the name helpers are pure. WriteBuildFile / RemoveBuildFile do file
// I/O: the App calls them on the job thread with COPIES of the build.
#include <filesystem>
#include <string>
#include <string_view>

#include "Loadouts.h"   // ActiveSets
#include "TreeData.h"
#include "Types.h"

namespace bp {

inline constexpr std::string_view kBuildFilePrefix = "POEFixer - ";
inline constexpr std::string_view kBuildFileExt = ".build";
inline constexpr std::string_view kBuildFileAuthor = "POEFixer Build Planner";

// The JSON text of the .build file for `sets` of `build` (2-space indent, UTF-8). `tree` gives the
// passives' string ids (null or empty: "passives" is left out). `stageTitle` may be "".
// "" only when the build has nothing to write (sets.spec out of range and no skills or items).
std::string BuildFileJson(const StoredBuild& build, const ActiveSets& sets, const PassiveTree* tree,
                          std::string_view stageTitle);

// "POEFixer - <name>.build": the build name with characters Windows forbids in file names replaced
// by '_', control characters dropped, trailing dots / spaces trimmed, cut to 120 characters; "Build"
// when nothing is left. UTF-8.
std::string BuildFileName(std::string_view buildName);

// Documents\My Games\Path of Exile 2\BuildPlanner of the current user (SHGetKnownFolderPath; the
// folder may not exist yet). Empty path when Documents cannot be resolved.
std::filesystem::path BuildPlannerFolder();

// Does this file look like one of ours: the name has our prefix and extension, the file is at most
// 4 MiB of JSON, and its "author" equals kBuildFileAuthor.
bool IsOurBuildFile(const std::filesystem::path& file);

// What a write or a removal did (the Builds tab shows it).
struct BuildFileResult {
    bool ok = false;
    std::filesystem::path file;    // the file written or removed (empty when nothing was touched)
    std::string error;             // English, "" when ok
};

// Writes `json` as folder/fileName atomically (JsonFile.h), creating the folder. Refuses a fileName
// without our prefix / extension, and refuses to overwrite an existing file that is not ours.
// `previousFileName` (may be ""): when it differs from fileName and that file is ours, it is removed
// after the new one is written (the build was renamed).
BuildFileResult WriteBuildFile(const std::filesystem::path& folder, std::string_view fileName, std::string_view json,
                               std::string_view previousFileName);

// Removes folder/fileName when it is ours. ok also when the file does not exist.
BuildFileResult RemoveBuildFile(const std::filesystem::path& folder, std::string_view fileName);

} // namespace bp
