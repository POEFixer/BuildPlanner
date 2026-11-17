#pragma once
// BuildPlanner - builds.json, the imported builds
// BuildStore.h - the imported builds (builds.json version 1): decoded trees, skills and
// items, never the Path of Building code. Pure, no SDK. The data model itself is in Types.h.
//
// builds.json:
//   { "version": 1, "builds": [ { "id", "name", "source", "importedAt", "className", "ascendName",
//       "level", "activeSpec", "activeSkillSet", "activeItemSet", "selectedSpec", "selectedSkillSet",
//       "selectedItemSet", "specs": [...], "skillSets": [...], "itemSets": [...], "items": [...] } ] }
//   Keys inside the lists are the field names of Types.h. The file is untrusted (hand edits): loading
//   applies every cap of Types.h, drops values of the wrong type or out of range, repairs indices that
//   point outside their list to -1, and gives a missing, malformed or duplicate id a new one. What was
//   dropped is named in `warnings` (English, at most 20 lines).
//
// The live state is the in-memory store (a hard rule): builds.json is a write-behind copy,
// read once at start. Nothing ever reloads it to pick up a change.
//
// A builds.json the load could not use is never overwritten (StoreFileVerdict): LoadBuildStore leaves
// the store empty and remembers it; the next SaveBuildStore first moves that file aside, to
// "builds.json.corrupt" or "builds.json.newer" next to it (a file of that name from an earlier time
// becomes "<name>.old" first, replacing an older ".old"; best effort, the save goes on without it),
// and writes only when the file itself was moved. A load that
// read the file, even with entries repaired or dropped, saves normally. A file that cannot be opened
// or read is tried again for half a second (kLoadRetries x kLoadRetryMs) before it counts as unusable:
// another program may hold it for a moment.
//
// Threads: a BuildStore belongs to the render thread (the App). Jobs get copies of a StoredBuild.
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "Types.h"

namespace bp {

// The builds.json version this plugin writes and reads; a file with a higher "version" is not read.
inline constexpr int kStoreFileVersion = 1;

// LoadBuildStore's patience with a builds.json it cannot open or read (see above).
inline constexpr int kLoadRetries = 5;
inline constexpr unsigned long kLoadRetryMs = 100;

// What LoadBuildStore made of the builds.json it found.
enum class StoreFileVerdict : uint8_t {
    Accepted = 0,   // no file, or the file was read (entries may have been repaired or dropped)
    Unusable,       // unreadable, over 16 MiB, not JSON, not a JSON object, "builds" not a list, or not
                    // read completely: the next save moves it to "builds.json.corrupt" first
    Newer           // its "version" is above kStoreFileVersion (a newer plugin wrote it): not read; the
                    // next save moves it to "builds.json.newer" first
};

class BuildStore {
public:
    BuildStore();

    std::vector<StoredBuild> builds;   // in import order (at most kMaxBuilds)

    // Replaces the contents from the text of builds.json. Never throws. Returns false (store empty)
    // when the text is not a JSON object or its "version" is above kStoreFileVersion; an object
    // without "builds" gives an empty store and true. A version below 1 is read as version 1.
    bool FromJsonText(std::string_view json, std::vector<std::string>* warnings);
    // builds.json as written: 2-space indent, invalid UTF-8 replaced. "" only when out of memory.
    std::string ToJsonText() const;

    StoredBuild* Find(std::string_view id);
    const StoredBuild* Find(std::string_view id) const;
    int IndexOf(std::string_view id) const;   // -1 unknown
    // The first stored build whose content equals what Add would store of `build` (BuildData's
    // operator== after Add's caps): the same class, level, trees, skill sets, item sets and items,
    // whatever its name, source, import time and pins. nullptr when there is none, or when the
    // comparison could not be made (out of memory). For an import of a build the list already has:
    // the caller follows that one instead of adding a second row.
    const StoredBuild* FindSame(const BuildData& build) const noexcept;

    // Adds an imported build with a new id and returns that id; "" when the store is full
    // (kMaxBuilds) or the build has no tree. The caps apply (named in `warnings`). `name` "" =
    // DefaultName(). selected* start at -1 (automatic).
    std::string Add(const BuildData& build, const std::string& source, int64_t importedAt,
                    std::vector<std::string>* warnings, const std::string& name = std::string());
    // Add, then the file guard: when builds.json would be larger than `maxFileBytes` with the new
    // build, it is taken out again, the store is as before, *tooLarge is set and "" returned.
    std::string AddWithinLimit(const BuildData& build, const std::string& source, int64_t importedAt,
                               std::vector<std::string>* warnings, bool* tooLarge,
                               size_t maxFileBytes = kMaxStoreFileBytes);
    // The size of ToJsonText(); SIZE_MAX when it cannot be computed.
    size_t FileBytes() const noexcept;

    bool Remove(std::string_view id);
    // Trims spaces; an empty or over-long (kMaxNameBytes) name is refused (false).
    bool Rename(std::string_view id, std::string_view name);

    std::string NewId();                         // a fresh id no build has
    void Seed(uint64_t seed) { m_rng = seed; }   // tests: deterministic ids

    // "<Class> <Ascendancy> L<level>" (missing parts left out); the source link when the class is
    // unknown (cut to 80 characters); "Build" when both are empty.
    static std::string DefaultName(const BuildData& build, const std::string& source);
    static bool ValidId(std::string_view id);    // "b" + 8 lower-case hex digits

    // The verdict of the last LoadBuildStore into this store that a SaveBuildStore still has to act
    // on: Accepted again once a save moved the file aside (or found it gone). FromJsonText alone
    // never changes it.
    StoreFileVerdict PendingFileVerdict() const { return m_fileVerdict; }

private:
    friend void LoadBuildStore(const std::filesystem::path&, BuildStore&, std::vector<std::string>*);
    friend bool SaveBuildStore(const std::filesystem::path&, const BuildStore&, std::string*);
    bool Parse(std::string_view json, std::vector<std::string>* warnings, StoreFileVerdict& verdict);

    uint64_t m_rng = 0;
    mutable StoreFileVerdict m_fileVerdict = StoreFileVerdict::Accepted;   // cleared by the const save
};

// Loads <configDir>/builds.json (at most 16 MiB). A missing file gives an empty store and no warning.
// A file the load cannot use (StoreFileVerdict) leaves the store empty, is named in `warnings` with
// what happens to it, and is remembered for SaveBuildStore. Never throws.
void LoadBuildStore(const std::filesystem::path& file, BuildStore& store, std::vector<std::string>* warnings);
// Writes it atomically (JsonFile.h). When the last load did not use the existing file, that file is
// first moved aside ("<file>.corrupt" / "<file>.newer"; an older one becomes "<that>.old"); when it cannot be moved,
// nothing is written, the file stays as it is and false is returned with an `error` that names the
// file and what to do (the next save tries again). false with `error` (English, may be null) on any
// failure.
bool SaveBuildStore(const std::filesystem::path& file, const BuildStore& store, std::string* error);

// The first `maxChars` code points of UTF-8 text (never splits a character), "..." appended when cut.
std::string Utf8Cut(std::string_view text, size_t maxChars);
// At most `maxBytes` bytes of UTF-8 text, cut on a character boundary; invalid sequences removed.
std::string Utf8Clamp(std::string_view text, size_t maxBytes);

} // namespace bp
