#pragma once
// BuildPlanner - capped JSON file read and atomic write
// JsonFile.h - the plugin's file I/O for its JSON files: a size-capped read and an atomic write
// (temporary file + ReplaceFileW). Unicode paths only (std::filesystem::path, W APIs). No SDK.
// The JSON conversion itself lives with each module (Settings, BuildStore, Offsets): this header
// carries no JSON type on purpose, so including it is cheap.
//
// Threads: free functions without shared state; safe from any thread. Two threads must not write
// the same path at the same time (the App writes settings.json and builds.json on the render thread
// only, the tree download and the .build file on the job thread only).
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace bp {

enum class FileStatus : uint8_t {
    Ok = 0,
    Missing,      // the file (or its folder) does not exist
    TooLarge,     // larger than the cap: nothing was read
    IoError       // open / read / write / replace failed (`error` has the Windows error)
};

struct FileRead {
    FileStatus status = FileStatus::IoError;
    std::string bytes;        // the whole file (a UTF-8 BOM is removed), "" unless status == Ok
    std::string error;        // English, "" when Ok or Missing

    bool Ok() const { return status == FileStatus::Ok; }
};

// Reads a whole file of at most maxBytes. Never throws.
FileRead ReadFileCapped(const std::filesystem::path& path, size_t maxBytes);

// Writes `bytes` as the whole content of `path`, atomically: the data goes to "<path>.tmp" in the same
// folder, is flushed, and then replaces the target (ReplaceFileW; a first write moves it into
// place), so a crash or a full disk leaves the previous file intact. Creates the parent folder.
// Returns false and sets `error` (English, may be null) on failure. Never throws.
bool WriteFileAtomic(const std::filesystem::path& path, std::string_view bytes, std::string* error);

// Deletes a file. true when it is gone afterwards (also when it never existed). Never throws.
bool RemoveFile(const std::filesystem::path& path, std::string* error);

// The path as UTF-8 (for log lines and diagnostics). Never throws.
std::string PathUtf8(const std::filesystem::path& path);

} // namespace bp
