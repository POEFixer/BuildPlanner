#pragma once
// BuildPlanner - an IGame replaying a capture file (.mem)
// FixtureGame - an IGame that replays a capture file (.mem) instead of a live game: the game of the
// unit tests and of the demo mode (CLAUDE.md "Demo mode"). Pure: no SDK, no ImGui.
//
// A capture holds the memory regions the readers read (made by tools/make_fixture.py from a full
// snapshot of the client), the UiElementBase offsets of the captured build (so UiRead / UiChildren /
// UiRect answer like the host), the client size and the game status at the moment of the capture.
// The file format is this module's own; it is documented at the top of FixtureGame.cpp.
//
// Threads: the object returned by OpenFixtureGame is immutable and safe for the render and the watcher
// thread at the same time, like every IGame.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Game.h"

namespace bp {

// Opens a capture file. nullptr and *error (English, for the log) on failure.
std::unique_ptr<IGame> OpenFixtureGame(const std::filesystem::path& file, std::string* error);

// ---- deriving a modified game from a capture (tests) ----

// Bytes laid over the capture. A patch wins over the captured bytes it overlaps and makes its bytes
// readable also where the capture has none (a fake object at a fake address).
struct FixturePatch {
    uint64_t address = 0;
    std::string bytes;
};

// What EditFixtureGame changes. Every memory access of the derived game (Read, and the element reads
// behind UiRead / UiChildren / UiRect(s) / UiControlName / UiText / UiFind) goes through these.
struct FixtureEdits {
    std::vector<FixturePatch> patches;                   // later patches win over earlier ones
    std::vector<std::pair<uint64_t, uint64_t>> hidden;   // [from, to): an access touching it fails
    int clientW = 0, clientH = 0;                        // > 0: the game client has this size instead
    uint32_t failEvery = 0;       // > 0: every n-th memory access fails (counted from the edit)
    uint32_t garbageOneIn = 0;    // > 0: about one memory access in n returns pseudo-random bytes
    uint64_t seed = 1;            // the garbage generator's seed (deterministic per access number)
};

// A new game over the same capture with `edits` on top (the base is not changed). Edits stack: a game
// derived from a derived game keeps the earlier patches, hidden ranges and client size, and takes the
// fault settings (failEvery, garbageOneIn, seed) of the newest edit. nullptr when `base` was not made
// by OpenFixtureGame / EditFixtureGame.
std::unique_ptr<IGame> EditFixtureGame(const IGame& base, const FixtureEdits& edits);

// The header of a capture file: the JSON object documented at the top of FixtureGame.cpp, including
// its "expect" block (the values tests check). "" and *error on failure.
std::string ReadFixtureHeader(const std::filesystem::path& file, std::string* error);

} // namespace bp
