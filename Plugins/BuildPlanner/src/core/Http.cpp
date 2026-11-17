// BuildPlanner - one HTTPS GET (WinHTTP): POE1Fixer PassiveTreePlanner's Http.cpp,
// ported as is, plus the host allow-list.
// Http.cpp - see Http.h. WinHTTP in asynchronous mode, one GET per call; every handle is closed by
// RAII. Redirects are followed by hand (WinHTTP's own redirect policy is NEVER) so each hop is checked
// against the https:443 rule again. No cookies, no automatic Windows logon, no request body, no
// header beyond WinHTTP's defaults and a generic User-Agent.
//
// Why asynchronous: a synchronous WinHTTP call blocks for up to a whole timeout (DNS, TCP connect, the
// TLS handshake, the wait for the response or for body data), and nothing reliable ends it early.
// Measured 2026-11-13 against a local listener that accepts and never answers, with the call blocked
// in the TLS handshake: closing the SESSION or the CONNECT handle from another thread does not abort
// it (63 s after the close, at the timeout); closing the REQUEST handle does (at once, error 12017),
// but from another thread that is a race, since the handle can be closed between a check and the
// next call made with it. In asynchronous mode each operation is started, then HttpGet's thread waits
// for its completion while polling `cancel` every kCancelPollMs; on cancel that same thread closes the
// request handle, which is how WinHTTP documents cancelling an asynchronous request. So exactly one
// thread calls WinHTTP with each handle and closes it, exactly once, and never uses it afterwards.
// The status callback (on WinHTTP's threads) only stores a completion in a refcounted Completion and
// signals; ReadData writes into the Completion's own buffer, so a cancelled read can never write into
// memory HttpGet has released. Closing the request waits (bounded) for its HANDLE_CLOSING callback, so
// normally no callback for it is still running when HttpGet returns.
#include "Http.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <climits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>

#pragma comment(lib, "winhttp.lib")

namespace bp {
namespace {

constexpr wchar_t kUserAgent[] = L"POEFixer-BuildPlanner/1.0";
constexpr int kMaxRedirects = 5;
constexpr size_t kMaxUrlChars = 8192;
constexpr DWORD kMaxLocationBytes = 32 * 1024;
constexpr DWORD kReadChunk = 256 * 1024;
constexpr uint32_t kMaxSetupTimeoutMs = 15000;   // resolve / connect / send; receive keeps the caller's timeout
constexpr DWORD kCancelPollMs = 50;
constexpr DWORD kCloseWaitMs = 5000;   // for HANDLE_CLOSING after closing a request (normally immediate)

class Handle {
public:
    explicit Handle(HINTERNET h = nullptr) : h_(h) {}
    ~Handle() {
        if (h_) WinHttpCloseHandle(h_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HINTERNET get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HINTERNET h_;
};

bool Cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(std::memory_order_relaxed); }

// The completion state of one asynchronous request. Two references: HttpGet's (AsyncRequest) and the
// callback side's, which the HANDLE_CLOSING callback releases (WinHTTP's last callback for a handle).
// The callback writes `what` / `error` / `bytes` before SetEvent(done) and HttpGet reads them after
// waiting on it; one operation is outstanding at a time.
struct Completion {
    std::atomic<long> refs{2};
    HANDLE done = nullptr;     // auto-reset: the outstanding operation completed or failed
    HANDLE closed = nullptr;   // manual-reset: HANDLE_CLOSING arrived
    DWORD what = 0;            // the WINHTTP_CALLBACK_STATUS_* that completed it
    DWORD error = 0;           // REQUEST_ERROR's error
    DWORD bytes = 0;           // DATA_AVAILABLE's count, READ_COMPLETE's length
    std::unique_ptr<unsigned char[]> buf;   // WinHttpReadData's target: lives as long as WinHTTP may write it

    ~Completion() {
        if (done) CloseHandle(done);
        if (closed) CloseHandle(closed);
    }
    void Release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }
};

void CALLBACK OnStatus(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD infoLength) {
    auto* c = reinterpret_cast<Completion*>(context);
    if (!c) return;
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
        SetEvent(c->closed);
        c->Release();
        return;
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
        c->bytes = 0;
        break;
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
        c->bytes = (info && infoLength >= sizeof(DWORD)) ? *static_cast<const DWORD*>(info) : 0;
        break;
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
        c->bytes = infoLength;
        break;
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
        const auto* result = static_cast<const WINHTTP_ASYNC_RESULT*>(info);
        c->error = (result && result->dwError != 0) ? result->dwError : static_cast<DWORD>(ERROR_WINHTTP_INTERNAL_ERROR);
        break;
    }
    default:
        return;   // nothing waits on the other notifications
    }
    c->what = status;
    SetEvent(c->done);
}

// One request handle in asynchronous mode. Only HttpGet's thread calls WinHTTP with it and closes it.
class AsyncRequest {
public:
    AsyncRequest() = default;
    ~AsyncRequest() { Close(); }
    AsyncRequest(const AsyncRequest&) = delete;
    AsyncRequest& operator=(const AsyncRequest&) = delete;

    // Opens a GET for `object` and registers the status callback. 0, or the error.
    DWORD Open(HINTERNET connect, const wchar_t* object) {
        h_ = WinHttpOpenRequest(connect, L"GET", object, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!h_) return LastError();
        c_ = new (std::nothrow) Completion;
        if (!c_) return ERROR_NOT_ENOUGH_MEMORY;
        c_->done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        c_->closed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!c_->done || !c_->closed) return LastError();
        c_->buf.reset(new (std::nothrow) unsigned char[kReadChunk]);
        if (!c_->buf) return ERROR_NOT_ENOUGH_MEMORY;
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(c_);
        if (!WinHttpSetOption(h_, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context))) return LastError();
        if (WinHttpSetStatusCallback(h_, &OnStatus, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) ==
            WINHTTP_INVALID_STATUS_CALLBACK)
            return LastError();
        callback_ = true;
        return 0;
    }

    HINTERNET get() const { return h_; }
    unsigned char* buffer() const { return c_->buf.get(); }
    DWORD bytes() const { return c_->bytes; }

    // Finishes an operation just started: `started` is the start call's result (FALSE = it failed at
    // once, GetLastError has why). Waits for the completion `expected`, polling `cancel`. 0, or the
    // error; on cancel the request is closed (aborting the operation) and the error is
    // ERROR_WINHTTP_OPERATION_CANCELLED.
    DWORD Finish(BOOL started, DWORD expected, const std::atomic<bool>* cancel) {
        if (!started) return LastError();
        if (!c_) return ERROR_WINHTTP_INTERNAL_ERROR;   // closed already: nothing to wait for
        for (;;) {
            const DWORD w = WaitForSingleObject(c_->done, cancel ? kCancelPollMs : INFINITE);
            if (w == WAIT_OBJECT_0) break;
            if (w != WAIT_TIMEOUT) {
                Close();
                return ERROR_WINHTTP_INTERNAL_ERROR;
            }
            if (Cancelled(cancel)) {
                Close();
                return ERROR_WINHTTP_OPERATION_CANCELLED;
            }
        }
        if (c_->what == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) return c_->error;
        return c_->what == expected ? 0 : static_cast<DWORD>(ERROR_WINHTTP_INTERNAL_ERROR);
    }

    // Closes the handle (aborting an outstanding operation) and waits, bounded, for its HANDLE_CLOSING
    // callback. Idempotent. The Completion stays alive until the callback side lets go of it, so a late
    // callback never touches freed memory.
    void Close() noexcept {
        if (h_) {
            const bool closedOk = WinHttpCloseHandle(h_) != FALSE;
            h_ = nullptr;
            if (callback_ && closedOk) WaitForSingleObject(c_->closed, kCloseWaitMs);
        }
        if (c_) {
            if (!callback_) c_->Release();   // no callback will ever release the callback side's reference
            c_->Release();
            c_ = nullptr;
        }
    }

private:
    static DWORD LastError() {
        const DWORD e = GetLastError();
        return e != 0 ? e : static_cast<DWORD>(ERROR_WINHTTP_INTERNAL_ERROR);
    }

    HINTERNET h_ = nullptr;
    Completion* c_ = nullptr;
    bool callback_ = false;
};

wchar_t LowerW(wchar_t c) { return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c; }

bool StartsWithIW(std::wstring_view s, std::wstring_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (LowerW(s[i]) != LowerW(prefix[i])) return false;
    return true;
}

std::wstring Widen(const std::string& s) {
    if (s.empty() || s.size() > static_cast<size_t>(INT_MAX)) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), w.data(), n) != n) return {};
    return w;
}

bool HasControlOrSpace(std::wstring_view s) {
    for (wchar_t c : s)
        if (c <= 0x20 || c == 0x7F) return true;
    return false;
}

std::wstring StripFragment(std::wstring s) {
    const size_t hash = s.find(L'#');
    if (hash != std::wstring::npos) s.resize(hash);
    return s;
}

std::string ErrorText(const char* what, DWORD code) {
    switch (code) {
    case ERROR_WINHTTP_TIMEOUT: return std::string(what) + ": the server did not answer in time";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "the server name could not be resolved (no internet connection?)";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "could not connect to the server";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection was reset";
    case ERROR_WINHTTP_SECURE_FAILURE: return "the secure connection failed (certificate or TLS error)";
    case ERROR_WINHTTP_INVALID_URL:
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return "the URL is not valid";
    case ERROR_WINHTTP_OPERATION_CANCELLED: return "cancelled";
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return "the server sent an invalid response";
    default: return std::string(what) + " failed (WinHTTP error " + std::to_string(code) + ")";
    }
}

struct Target {
    std::wstring host;
    std::wstring object;   // path + query, "/" at least
};

// Accepts https://host[:443]/path?query only (no other scheme or port, no user info).
bool CrackHttps(const std::wstring& url, Target& out, std::string& error) {
    if (url.empty() || url.size() > kMaxUrlChars) {
        error = "the URL is empty or too long";
        return false;
    }
    if (HasControlOrSpace(url)) {
        error = "the URL contains spaces or control characters";
        return false;
    }
    if (!StartsWithIW(url, L"https://")) {
        error = "only https:// URLs are allowed";
        return false;
    }
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = static_cast<DWORD>(-1);
    uc.dwHostNameLength = static_cast<DWORD>(-1);
    uc.dwUserNameLength = static_cast<DWORD>(-1);
    uc.dwPasswordLength = static_cast<DWORD>(-1);
    uc.dwUrlPathLength = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) {
        error = "the URL is not valid";
        return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTPS) {
        error = "only https:// URLs are allowed";
        return false;
    }
    if (uc.nPort != INTERNET_DEFAULT_HTTPS_PORT) {
        error = "only the standard https port (443) is allowed";
        return false;
    }
    if (!uc.lpszHostName || uc.dwHostNameLength == 0) {
        error = "the URL has no host";
        return false;
    }
    if (uc.dwUserNameLength > 0 || uc.dwPasswordLength > 0) {
        error = "URLs with a user name or password are not allowed";
        return false;
    }
    out.host.assign(uc.lpszHostName, uc.dwHostNameLength);
    out.object.clear();
    if (uc.lpszUrlPath && uc.dwUrlPathLength > 0) out.object.assign(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (out.object.empty() || out.object[0] != L'/') out.object.insert(out.object.begin(), L'/');
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength > 0) out.object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    return true;
}

// The Location header of a 3xx, or "" when there is none (or it is unreasonably long).
std::wstring QueryLocation(HINTERNET request) {
    DWORD bytes = 0;
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &bytes,
                            WINHTTP_NO_HEADER_INDEX) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0 || bytes > kMaxLocationBytes)
        return {};
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    DWORD size = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    buf.resize(std::min<size_t>(buf.size(), size / sizeof(wchar_t)));
    while (!buf.empty() && (buf.back() == L' ' || buf.back() == L'\t' || buf.back() == L'\0')) buf.pop_back();
    size_t lead = 0;
    while (lead < buf.size() && (buf[lead] == L' ' || buf[lead] == L'\t')) ++lead;
    return buf.substr(lead);
}

// Resolves a Location against the current https target (RFC 3986 reference forms, without dot
// segment removal: the server sees the path as given).
std::wstring Resolve(const Target& base, std::wstring loc) {
    loc = StripFragment(std::move(loc));
    if (StartsWithIW(loc, L"https://") || StartsWithIW(loc, L"http://")) return loc;   // http is refused by CrackHttps
    if (loc.size() >= 2 && loc[0] == L'/' && loc[1] == L'/') return L"https:" + loc;
    const std::wstring origin = L"https://" + base.host;
    if (!loc.empty() && loc[0] == L'/') return origin + loc;
    std::wstring path = base.object;
    if (const size_t q = path.find(L'?'); q != std::wstring::npos) path.resize(q);
    if (loc.empty()) return origin + base.object;
    if (loc[0] == L'?') return origin + path + loc;
    const size_t slash = path.rfind(L'/');
    return origin + (slash == std::wstring::npos ? std::wstring(L"/") : path.substr(0, slash + 1)) + loc;
}

void SetDwordOption(HINTERNET h, DWORD option, DWORD value) {
    WinHttpSetOption(h, option, &value, sizeof(value));   // best effort
}

// The allow-list: an empty list allows every host; otherwise an exact, ASCII case-insensitive match.
bool HostAllowed(const std::wstring& host, std::span<const std::string_view> allowed) {
    if (allowed.empty()) return true;
    for (const std::string_view a : allowed) {
        if (a.size() != host.size()) continue;
        bool same = true;
        for (size_t i = 0; i < a.size() && same; ++i)
            same = LowerW(host[i]) == LowerW(static_cast<wchar_t>(static_cast<unsigned char>(a[i])));
        if (same) return true;
    }
    return false;
}

std::string NarrowAscii(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (const wchar_t c : s) out.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
    return out;
}

} // namespace

HttpResult HttpGet(const std::string& url, size_t maxBytes, uint32_t timeoutMs, const std::atomic<bool>* cancel,
                   std::span<const std::string_view> allowedHosts) {
    HttpResult r;
    auto fail = [&r](std::string e) {
        r.body.clear();
        r.error = std::move(e);
        return r;
    };
    // A failed WinHTTP operation: "cancelled" when it was aborted for the flag, else its own text.
    auto failCall = [&](const char* what, DWORD code) {
        if (code == ERROR_WINHTTP_OPERATION_CANCELLED || Cancelled(cancel)) return fail("cancelled");
        return fail(ErrorText(what, code));
    };
    try {
        if (url.size() > kMaxUrlChars) return fail("the URL is too long");
        std::wstring current = Widen(url);
        if (current.empty()) return fail(url.empty() ? "the URL is empty" : "the URL is not valid UTF-8");
        current = StripFragment(std::move(current));
        Target target;
        std::string err;
        if (!CrackHttps(current, target, err)) return fail(err);
        if (!HostAllowed(target.host, allowedHosts)) return fail("the host " + NarrowAscii(target.host) + " is not allowed");
        if (Cancelled(cancel)) return fail("cancelled");

        HINTERNET hs = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                   WINHTTP_FLAG_ASYNC);
        if (!hs)
            hs = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
        const Handle session(hs);
        if (!session) return fail(ErrorText("opening WinHTTP", GetLastError()));
        const int setupMs = static_cast<int>(std::clamp<uint32_t>(timeoutMs, 1u, kMaxSetupTimeoutMs));
        const int receiveMs = static_cast<int>(std::clamp<uint32_t>(timeoutMs, 1u, static_cast<uint32_t>(INT_MAX)));
        if (!WinHttpSetTimeouts(session.get(), setupMs, setupMs, setupMs, receiveMs))
            return fail(ErrorText("setting the timeouts", GetLastError()));
        SetDwordOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY, WINHTTP_OPTION_REDIRECT_POLICY_NEVER);

        for (int hop = 0;; ++hop) {
            if (hop > 0 && !CrackHttps(current, target, err)) return fail("a redirect was refused: " + err);
            if (hop > 0 && !HostAllowed(target.host, allowedHosts))
                return fail("a redirect was refused: the host " + NarrowAscii(target.host) + " is not allowed");
            if (Cancelled(cancel)) return fail("cancelled");
            const Handle connect(WinHttpConnect(session.get(), target.host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
            if (!connect) return fail(ErrorText("connecting", GetLastError()));
            AsyncRequest request;   // declared after `connect`: closed (and its callback done) before it
            if (const DWORD e = request.Open(connect.get(), target.object.c_str()); e != 0) return fail(ErrorText("opening the request", e));
            SetDwordOption(request.get(), WINHTTP_OPTION_DISABLE_FEATURE, WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS);
            SetDwordOption(request.get(), WINHTTP_OPTION_AUTOLOGON_POLICY, WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH);
#ifdef WINHTTP_OPTION_DECOMPRESSION
            SetDwordOption(request.get(), WINHTTP_OPTION_DECOMPRESSION, WINHTTP_DECOMPRESSION_FLAG_ALL);   // Windows 8.1+; ignored if refused
#endif
            DWORD e = request.Finish(WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0),
                                     WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, cancel);
            if (e != 0) return failCall("sending the request", e);
            e = request.Finish(WinHttpReceiveResponse(request.get(), nullptr), WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, cancel);
            if (e != 0) return failCall("receiving the response", e);
            DWORD status = 0, len = sizeof(status);
            if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                     &status, &len, WINHTTP_NO_HEADER_INDEX))
                return failCall("reading the status", GetLastError());
            r.status = static_cast<int>(std::min<DWORD>(status, 999));
            if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                if (hop >= kMaxRedirects) return fail("too many redirects");
                const std::wstring loc = QueryLocation(request.get());
                if (loc.empty()) return fail("a redirect without a Location header");
                current = Resolve(target, loc);
                continue;
            }
            if (status != 200) return fail("HTTP status " + std::to_string(status));

            DWORD contentLength = 0;
            len = sizeof(contentLength);
            if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &contentLength, &len, WINHTTP_NO_HEADER_INDEX))
                r.body.reserve(std::min<size_t>(contentLength, maxBytes));   // a hint only: the cap below is what counts
            for (;;) {
                if (Cancelled(cancel)) return fail("cancelled");
                e = request.Finish(WinHttpQueryDataAvailable(request.get(), nullptr), WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE, cancel);
                if (e != 0) return failCall("reading the response", e);
                const DWORD avail = request.bytes();
                if (avail == 0) break;
                if (avail > maxBytes - r.body.size()) return fail("response too large");
                const DWORD want = std::min(avail, kReadChunk);
                e = request.Finish(WinHttpReadData(request.get(), request.buffer(), want, nullptr), WINHTTP_CALLBACK_STATUS_READ_COMPLETE,
                                   cancel);
                if (e != 0) return failCall("reading the response", e);
                const DWORD got = std::min(request.bytes(), want);
                if (got == 0) break;
                r.body.append(reinterpret_cast<const char*>(request.buffer()), got);
            }
            r.error.clear();
            return r;
        }
    } catch (const std::bad_alloc&) {
        return fail("out of memory");
    } catch (...) {
        return fail("the download failed");
    }
}

} // namespace bp
