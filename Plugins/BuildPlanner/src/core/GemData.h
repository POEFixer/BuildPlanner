#pragma once
// BuildPlanner - the gem table (data/gems.json)
// GemData.h - the bundled gem table (data/gems.json, made by tools/make_gems.py from Path of Building's
// Gems.lua and Skills/*.lua): gem id -> English name, support or not, colour. Pure, no SDK.
//
// data/gems.json (written by tools/make_gems.py; the reader below is the contract):
//   { "bp_meta": { "source": "...", "commit": "...", "count": 967 },
//     "gems": [ { "id": "Metadata/Items/Gems/SkillGemFireball", "variant": "Fireball",
//                 "name": "Fireball", "support": false, "color": "int", "skill": "FireballPlayer",
//                 "family": "Fireball" }, ... ] }
//   "color": "str" | "dex" | "int" | "none". "variant", "skill" and "family" may be absent.
//   "actives": 0..kMaxGemActives (GemInfo::activeSkills), written only where it is not the default
//   (support ? 0 : 1); a row with another value or type is skipped.
//   "family" is Path of Building's `gemFamily`. Its `Tier` field is NOT written: that is the level
//   at which a gem unlocks (Rapid Attacks II has Tier 4), not the gem's own tier.
//
// Tiers of support gems (the "Other tier" of the skill hints): "Multishot I" and "Multishot II" are two
// tiers of one gem. The family alone does not say so: "Additional Projectiles" holds Multishot I / II AND
// Wildshards I / II, which are different gems (44 of the 134 support families with several gems mix
// gems like that). So a gem's tier is the roman numeral its English name ends with (GemInfo::tier,
// SplitGemTier) and two gems are tiers of one gem when the names without the numeral are equal
// (GemInfo::tierBase) and the families do not contradict: SameGemOtherTier.
// Eight supports name their FIRST tier without a numeral while the next one has it ("Living Lightning"
// and "Living Lightning II"; also Accelerated Growth, Ambrosia, Fan The Flames, Mark for Death, Mark of
// Siphoning, Shock Conduction, Thrill of the Kill in Gems.lua 601768bd). So Parse gives a gem without a
// numeral tier 1 (tierBase = its name) when the table also holds a gem named exactly "<that name> II";
// every other gem without a numeral keeps tier 0. SplitGemTier itself (the name-only fallback without
// the table) stays strict: "Living Lightning" alone has no tier.
//
// Ids: Path of Building and the game write both "Metadata/Items/Gems/..." and "Metadata/Items/Gem/...".
// Every lookup here goes through NormalizeGemId, so the two spellings are one gem.
//
// Threads: a GemData is immutable after Parse. The App loads it once on the job thread and publishes
// it as shared_ptr<const GemData>; any thread may read it.
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Types.h"

namespace bp {

struct GemInfo {
    std::string id;          // normalized (NormalizeGemId)
    std::string variantId;   // "" none
    std::string name;        // English display name
    std::string skillId;     // granted effect id, "" unknown
    bool support = false;
    Attr color = Attr::None;
    std::string family;      // Path of Building's gemFamily ("Additional Projectiles"), "" unknown
    int tier = 0;            // the roman numeral the name ends with: "Multishot II" -> 2; 1 for a name
                             // without a numeral whose "<name> II" is in the table; 0 = none
    std::string tierBase;    // the name without that numeral ("Multishot"); == name when it has none
    // The id exactly as the file (and the game's BaseItemTypes) spells it, "Gem/" or "Gems/": what
    // PobImport writes into BuildGem::gemId when it fills an absent one. `id` is the normalized form.
    std::string gameId;
    // How many entries one enabled copy of the gem adds to Path of Building's list of a group's active
    // skills (the list `mainActiveSkill` indexes): the gem's granted effects that are no support and
    // not hidden from the side bar. 1 for most skill gems, 2..4 for a gem that grants several skills
    // (Galvanic Shards and its ammo skill, Apocalypse), 0 for most supports, 1 for a support that
    // grants a triggered skill (Living Lightning). File key "actives"; absent = support ? 0 : 1.
    int activeSkills = 1;
};

// Splits an English gem name at a trailing roman numeral I..X that follows a space:
// "Rapid Attacks III" -> 3 and base "Rapid Attacks"; "Fireball" -> 0 and base "Fireball". `base`
// (optional) gets the name without the numeral, trimmed. A name that is only a numeral has no tier.
int SplitGemTier(std::string_view name, std::string* base = nullptr);

// Are `a` and `b` two TIERS of one gem (never the same gem)? Both have a tier, the tiers differ, the
// tier bases are equal (byte for byte) and the families are equal when both are known.
inline bool SameGemOtherTier(const GemInfo& a, const GemInfo& b) {
    return a.tier > 0 && b.tier > 0 && a.tier != b.tier && !a.tierBase.empty() && a.tierBase == b.tierBase &&
           (a.family.empty() || b.family.empty() || a.family == b.family);
}

// "Metadata/Items/Gem/X" -> "Metadata/Items/Gems/X"; everything else unchanged. Case is kept (ids are
// compared byte for byte after this).
std::string NormalizeGemId(std::string_view gemId);

class GemData {
public:
    // Replaces the contents. false with `error` (English, may be null) when the text is not the format
    // above or holds no gem; the table is then empty. Fills GemInfo::tier / tierBase with SplitGemTier
    // and the "<name> II" rule above. Never throws.
    bool Parse(std::string_view json, std::string* error);

    bool Empty() const { return m_gems.empty(); }
    size_t Size() const { return m_gems.size(); }
    const std::string& Source() const { return m_source; }   // bp_meta.source, "" unknown
    const std::string& Commit() const { return m_commit; }

    // By id (either spelling). With a variantId the row of that variant when there is one, else the
    // id's first row. nullptr when unknown.
    const GemInfo* Find(std::string_view gemId, std::string_view variantId = {}) const;
    // By English name, ASCII case-insensitive ("Fireball"). nullptr when unknown or ambiguous.
    const GemInfo* FindByName(std::string_view name) const;
    // By granted effect id ("FireballPlayer"). nullptr when unknown.
    const GemInfo* FindBySkillId(std::string_view skillId) const;

private:
    std::vector<GemInfo> m_gems;
    std::unordered_map<std::string, std::vector<uint32_t>> m_byId;   // normalized id -> rows
    std::unordered_map<std::string, uint32_t> m_byName;              // lower-case name -> row (kNoIndex = ambiguous)
    std::unordered_map<std::string, uint32_t> m_bySkill;
    std::string m_source, m_commit;
};

// Reads data/gems.json (at most 8 MiB) and parses it. false with `error` otherwise. Never throws.
bool LoadGemFile(const std::filesystem::path& path, GemData& out, std::string* error);

// Limits of GemData::Parse: rows past kMaxGemRows are ignored; a row whose id, variant, name, skill or
// family is longer than kMaxGemFieldBytes (or whose id does not start with "Metadata/") is skipped.
inline constexpr size_t kMaxGemFileBytes = 8u << 20;
inline constexpr size_t kMaxGemRows = 20000;
inline constexpr size_t kMaxGemFieldBytes = 256;
inline constexpr int kMaxGemActives = 16;   // GemInfo::activeSkills (Path of Building's most is 4)

// Is this gem a support gem? `data` (may be null or empty) decides when it knows the id (+ variant);
// otherwise a heuristic: the id contains "Support", or skillId starts with "Support".
bool IsSupportGem(const GemData* data, std::string_view gemId, std::string_view variantId, std::string_view skillId);

// The colour to draw a gem with: the table's when it knows the gem (by id, then by English name),
// else Attr::None.
Attr GemColorOf(const GemData* data, std::string_view gemId, std::string_view name);

} // namespace bp
