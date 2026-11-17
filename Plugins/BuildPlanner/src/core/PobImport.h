#pragma once
// BuildPlanner - Path of Building import
// PobImport.h - turns what the user pastes into a build: a Path of Building (PoE2)
// export code, raw PoB XML, or a link to a build site. Pure except ImportFromText, which may download
// through the HttpGetFn it is given. No SDK. Nothing here throws.
//
// Formats (real builds in tests/fixtures/builds):
//   PoB code  = base64(zlib(XML)) with '-' '_' for '+' '/', padding optional, whitespace ignored.
//               The decoder also accepts raw deflate; the output is capped at 16 MiB.
//   PoB XML   = root <PathOfBuilding2> (<PathOfBuilding> accepted, except a Path of Exile 1 build: that
//               root with every tree on a version whose major number is 2 or more, "3_25", is refused
//               with a Parse error that says so): <Build>, <Tree>/<Spec> with the
//               PoE2 children WeaponSet1/2, Overrides/AttributeOverride, Sockets, Notes, <Skills>/
//               <SkillSet>/<Skill>/<Gem>, <Items>/<Item>/<ItemSet>/<Slot>; legacy layouts (a <Skill>
//               directly under <Skills>, a <Slot> directly under <Items>). The literal "nil" is an
//               absent value. A Spec without `nodes` falls back to its <URL> tree link.
//   Links     = the table at ClassifyInput.
//
// Class and ascendancy of a <Spec>. BuildSpec::classId is the tree data's integerId and
// BuildSpec::ascendancyId its internalId; PobImport has no tree data, so it carries the maps itself:
//   - `classInternalId` / `ascendancyInternalId` win when present (and are not "nil").
//   - else legacy `classId`: for treeVersion 0_1..0_3 through Path of Building's legacyClassIdMap
//     (0_1 {0=2,1=6,2=9,3=1,4=7,5=10}; 0_2 and 0_3 {0=2,1=8,2=6,3=9,4=1,5=7,6=10}); from 0_4 on it IS
//     the integerId. ClassIdFromLegacy.
//   - else legacy `ascendClassId` (1-based index into the class's ascendancy list of THAT tree
//     version, 0 = none): the lists differ per version (Monk1 and Huntress2 exist only from 0_5), and
//     only the 0_5 tree ships with the plugin, so PobImport.cpp embeds the internalId lists of
//     0_1..0_5 per class (taken from Path of Building's src/TreeData/<version>/tree.json "classes").
//     AscendancyIdFromLegacy. An unknown version uses the newest list.
//   - a missing treeVersion is "0_1" (Path of Building's default).
//
// Skill groups: a <Skill removed="true"> (a granted group the author hid) is dropped. `enabled` is
// read from the legacy `active` attribute when `enabled` is absent. A <Gem> without `gemId` gets it
// from GemData (by `skillId`, then by `nameSpec`) when `gems` is given, so the stored build matches
// the game by id and the .build file keeps the gem.
// The main skill: Path of Building's `mainActiveSkill` (1-based, absent or below 1 = 1) indexes the
// group's list of ACTIVE SKILLS, not of gems (Modules/CalcSetup.lua, the group's displaySkillList):
// every ENABLED gem adds, in gem order, one entry per active skill it grants (GemInfo::activeSkills:
// Galvanic Shards adds two, Apocalypse four, most supports none, a support with a triggered skill such
// as Living Lightning one; a gem the table does not know adds one unless it is a support); a disabled
// gem adds none. PobImport stores the gem that owns that entry as BuildSkillGroup::mainActive, its
// index among ALL the group's non-support gems (Types.h). A value past the list names its last entry
// (Path of Building clamps it so). When the entry belongs to a support gem, or the list is empty, the
// first enabled non-support gem is stored (0 when there is none).
//
// Threads: every function is stateless and safe from any thread. ImportFromText blocks while it
// downloads: the App calls it on the job thread only.
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Http.h"
#include "Types.h"

namespace bp {

class GemData;   // GemData.h

enum class InputKind : uint8_t {
    Empty = 0,
    PobCode,             // a bare export code
    PobXml,              // pasted XML (starts with '<')
    PobbinLink,          // pobb.in/<id>                         -> GET https://pobb.in/<id>/raw
                         // pobb.in/u/<user>/<id>                -> GET https://pobb.in/u/<user>/<id>/raw
    PoeNinjaLink,        // poe.ninja/poe2/pob/<id>, poe2.ninja/pob/<id>, .../pob/raw/<id>
                         //                                      -> GET https://poe.ninja/poe2/pob/raw/<id>
    MaxrollPobLink,      // maxroll.gg/poe2/pob/<id>             -> GET https://maxroll.gg/poe2/api/pob/<id>
    Poe2dbLink,          // poe2db.tw/pob/<id>                   -> GET https://poe2db.tw/pob/<id>/raw
    PastebinLink,        // pastebin.com/<id>, pastebin.com/raw/<id> -> GET https://pastebin.com/raw/<id>
    PoeNinjaCharacter,   // poe.ninja/poe2/builds/<league>/character/<account>/<name>: two JSON requests
                         // (index-state for the snapshot name, then the character; pathOfBuildingExport)
    MaxrollGuide,        // maxroll.gg/poe2/build-guides/... or /poe2/planner/...: NOT importable; the
                         // UI tells the user to paste the guide's Path of Building link instead
    Unknown
};

struct Classified {
    InputKind kind = InputKind::Unknown;
    // The URL to GET for the five *Link kinds that hold a code ("" otherwise). Always https.
    std::string fetchUrl;
    // PoeNinjaCharacter only: the path parts, percent-decoded, as pasted.
    std::string league, account, character;
};

// Classifies trimmed input (a UTF-8 BOM is ignored). A known link in angle brackets ("<https://pobb.in/x>",
// as Discord writes links) is that link. Order: '<' first -> PobXml; a link when the text is at most
// 16 KiB, has no whitespace or control characters and names a known host (http://, https:// or no
// scheme; "www." optional; query and fragment dropped; ids and user names [A-Za-z0-9_-]{1,64}); at
// least 64 base64url characters -> PobCode; else Unknown. Path keywords are case-sensitive.
Classified ClassifyInput(std::string_view text);

// true for the kinds an import can start from by itself: a code, XML, a *Link kind or a poe.ninja
// character page. The Builds tab starts the import on a paste when this is true.
bool StartsImport(InputKind kind);

// The hosts the import may contact (lower case). The App binds HttpGet to this list.
inline constexpr std::string_view kImportHosts[] = {"pobb.in", "poe.ninja", "maxroll.gg", "poe2db.tw", "pastebin.com"};

inline constexpr size_t kImportMaxBytes = 8u << 20;    // one download
inline constexpr uint32_t kImportTimeoutMs = 15000;
inline constexpr size_t kMaxXmlBytes = 16u << 20;      // a decoded code (the zip-bomb guard)

// base64 with either alphabet, whitespace ignored, padding optional. false on a character outside the
// alphabet or an impossible length.
bool Base64UrlDecode(std::string_view text, std::vector<uint8_t>& out);
std::string Base64UrlEncode(const uint8_t* data, size_t size);   // '-' '_', no padding (tests, fixtures)

// code -> XML. false with `error` (English, may be null): "the code is incomplete (it looks cut off)",
// "the code expands to more than 16 MiB (refused)", "the code is not valid base64", ...
bool DecodePobCode(std::string_view code, std::string& xml, std::string* error);

// XML -> build. `gems` (may be null) resolves BuildGem::support; without it the id heuristic of
// IsSupportGem (GemData.h) decides. Applies every cap of Types.h (what was dropped is named in
// `warnings`, English, may be null). false with `error` when the XML is malformed or the root is
// neither PathOfBuilding2 nor PathOfBuilding. A build without any tree still parses (ImportFromText
// refuses it as NoSpecs).
bool ParsePobXml(std::string_view xml, BuildData& out, std::string* error, const GemData* gems = nullptr,
                 std::vector<std::string>* warnings = nullptr);

// "^7" / "^xRRGGBB" colour codes removed.
std::string StripPobColors(std::string_view text);

// Legacy <Spec classId> -> the tree data's integerId (see the header comment); -1 when the map of
// that version has no such entry. `treeVersion` "" = "0_1".
int ClassIdFromLegacy(std::string_view treeVersion, int legacyClassId);
// Legacy <Spec ascendClassId> (1-based, 0 = none) of a class (integerId) -> the ascendancy's
// internalId ("Sorceress3") by that tree version's own list; "" for 0, an unknown class or an index
// past the list.
std::string AscendancyIdFromLegacy(std::string_view treeVersion, int classIntegerId, int ascendClassId);

enum class ImportProblem : uint8_t {
    None = 0,
    Empty,           // nothing pasted
    Unrecognized,    // neither a code, XML nor a known link
    MaxrollGuide,    // a maxroll guide / planner link: ask for the Path of Building link
    Download,        // the download failed (`detail` has the cause, `httpStatus` the status when there was one)
    NotFound,        // the site answered 404: the paste is gone or the link is wrong
    Decode,          // not a complete Path of Building code
    Parse,           // the XML could not be read
    NoSpecs,         // the build has no passive tree
    Cancelled        // the user cancelled
};

struct ImportResult {
    bool ok = false;
    InputKind kind = InputKind::Unknown;
    ImportProblem problem = ImportProblem::None;
    BuildData build;                      // valid when ok
    std::string source;                   // the link the user pasted (trimmed, <= kMaxSourceBytes),
                                          // or "PoB code" / "PoB XML"
    std::string detail;                   // English cause for the log and the red line's "(...)"; "" when ok
    int httpStatus = 0;                   // the last HTTP status seen, 0 = none
    std::vector<std::string> warnings;    // English; caps applied, ids dropped (ok may still be true)
};

// The whole import: classify -> download when it is a link -> decode -> parse. `http` performs the
// downloads (never called for a code or XML; at most twice for a poe.ninja character); it must honour
// `cancel`. A set `cancel` gives Cancelled. `gems` may be null.
ImportResult ImportFromText(std::string_view text, const std::atomic<bool>* cancel, const HttpGetFn& http,
                            const GemData* gems = nullptr);

} // namespace bp
