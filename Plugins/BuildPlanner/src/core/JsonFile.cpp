// BuildPlanner - capped JSON file read and atomic write
// JsonFile.cpp - see JsonFile.h. Win32 file I/O with wide paths only.
#include "JsonFile.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <climits>
#include <system_error>

namespace bp {
namespace {

std::string WinError(const char* what, DWORD code) {
    return std::string(what) + " failed (Windows error " + std::to_string(code) + ")";
}

class File {
public:
    explicit File(HANDLE h) : h_(h) {}
    ~File() { Close(); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    bool Ok() const { return h_ != INVALID_HANDLE_VALUE; }
    HANDLE Get() const { return h_; }
    // true when the handle closed without an error (a failed close can lose buffered data).
    bool Close() {
        if (h_ == INVALID_HANDLE_VALUE) return true;
        const bool ok = CloseHandle(h_) != FALSE;
        h_ = INVALID_HANDLE_VALUE;
        return ok;
    }

private:
    HANDLE h_;
};

void SetError(std::string* error, std::string text) {
    if (error) *error = std::move(text);
}

} // namespace

std::string PathUtf8(const std::filesystem::path& path) {
    try {
        const std::wstring& w = path.native();
        if (w.empty() || w.size() > static_cast<size_t>(INT_MAX)) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        if (n <= 0) return {};
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
        return out;
    } catch (...) {
        return {};
    }
}

FileRead ReadFileCapped(const std::filesystem::path& path, size_t maxBytes) {
    FileRead r;
    try {
        if (path.empty()) {
            r.status = FileStatus::Missing;
            return r;
        }
        File f(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!f.Ok()) {
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
                r.status = FileStatus::Missing;
                return r;
            }
            r.error = WinError("opening the file", e);
            return r;
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(f.Get(), &size)) {
            r.error = WinError("reading the file size", GetLastError());
            return r;
        }
        if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > maxBytes) {
            r.status = FileStatus::TooLarge;
            r.error = "the file is larger than " + std::to_string(maxBytes) + " bytes";
            return r;
        }
        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        size_t got = 0;
        while (got < bytes.size()) {
            const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - got, 1u << 20));
            DWORD n = 0;
            if (!ReadFile(f.Get(), bytes.data() + got, want, &n, nullptr)) {
                r.error = WinError("reading the file", GetLastError());
                return r;
            }
            if (n == 0) break;   // the file shrank while it was read
            got += n;
        }
        bytes.resize(got);
        if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
            static_cast<unsigned char>(bytes[2]) == 0xBF)
            bytes.erase(0, 3);
        r.bytes = std::move(bytes);
        r.status = FileStatus::Ok;
        return r;
    } catch (...) {
        r.status = FileStatus::IoError;
        r.bytes.clear();
        r.error = "out of memory while reading the file";
        return r;
    }
}

bool WriteFileAtomic(const std::filesystem::path& path, std::string_view bytes, std::string* error) {
    try {
        if (path.empty()) {
            SetError(error, "the file path is empty");
            return false;
        }
        std::error_code ec;
        const std::filesystem::path dir = path.parent_path();
        if (!dir.empty()) std::filesystem::create_directories(dir, ec);   // an existing folder is not an error
        std::filesystem::path tmp = path;
        tmp += L".tmp";
        {
            File f(CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!f.Ok()) {
                SetError(error, WinError("creating the temporary file", GetLastError()));
                return false;
            }
            size_t put = 0;
            while (put < bytes.size()) {
                const DWORD want = static_cast<DWORD>(std::min<size_t>(bytes.size() - put, 1u << 20));
                DWORD n = 0;
                if (!WriteFile(f.Get(), bytes.data() + put, want, &n, nullptr) || n == 0) {
                    const DWORD e = GetLastError();
                    f.Close();
                    DeleteFileW(tmp.c_str());
                    SetError(error, WinError("writing the temporary file", e));
                    return false;
                }
                put += n;
            }
            if (!FlushFileBuffers(f.Get())) {
                const DWORD e = GetLastError();
                f.Close();
                DeleteFileW(tmp.c_str());
                SetError(error, WinError("flushing the temporary file", e));
                return false;
            }
            if (!f.Close()) {
                const DWORD e = GetLastError();
                DeleteFileW(tmp.c_str());
                SetError(error, WinError("closing the temporary file", e));
                return false;
            }
        }
        // ReplaceFileW keeps the target's attributes and is atomic on NTFS; it needs an existing
        // target, so the first write moves the temporary file into place instead.
        if (ReplaceFileW(path.c_str(), tmp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) return true;
        const DWORD replaceError = GetLastError();
        if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
        const DWORD moveError = GetLastError();
        DeleteFileW(tmp.c_str());
        SetError(error, WinError("replacing the file", replaceError) + "; " + WinError("moving the temporary file", moveError));
        return false;
    } catch (...) {
        SetError(error, "out of memory while writing the file");
        return false;
    }
}

bool RemoveFile(const std::filesystem::path& path, std::string* error) {
    try {
        if (path.empty()) return true;
        if (DeleteFileW(path.c_str())) return true;
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
        SetError(error, WinError("deleting the file", e));
        return false;
    } catch (...) {
        SetError(error, "deleting the file failed");
        return false;
    }
}

} // namespace bp
