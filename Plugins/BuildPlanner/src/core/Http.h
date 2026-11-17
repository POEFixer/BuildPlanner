#pragma once
// BuildPlanner - one HTTPS GET (a port of POE1Fixer's Http.cpp)
// Http.h - one HTTPS GET with WinHTTP. Used for build links (PobImport.h) and for "Update tree data"
// (TreeData.h). No SDK. Sends a generic User-Agent ("POEFixer-BuildPlanner/1.0") and nothing about
// the user: no cookies, no Windows logon, no request body.
//
// Threads: HttpGet blocks; call it on the job thread only, never on the render or watcher thread.
// It has no shared state, and no thread of its own outlives the call.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace bp {

struct HttpResult {
    int status = 0;            // HTTP status, 0 = no answer
    std::string body;          // at most maxBytes
    std::string error;         // English, "" on success (status 200)
    bool Ok() const { return status == 200 && error.empty(); }
};

// https:// on port 443 only (anything else is refused before the network is touched). Follows at most
// 5 redirects, each checked against the same rules again. Refuses a body larger than maxBytes
// (reading stops there). timeoutMs bounds each wait for the response and for body data; resolving
// the name, connecting and sending wait at most min(timeoutMs, 15 s) each. Only status 200 is Ok.
// `cancel` (may be null) is polled every 50 ms while any network step is outstanding: once it is set
// the request is aborted and the error is exactly "cancelled", normally within ~50 ms. A flag that
// is already set means no request at all.
// `allowedHosts`: when not empty, the host of the URL AND of every redirect must equal one of them
// (ASCII case-insensitive, exact: no subdomain matching); otherwise the error names the host
// ("the host ... is not allowed") and nothing is sent to it.
HttpResult HttpGet(const std::string& url, size_t maxBytes, uint32_t timeoutMs, const std::atomic<bool>* cancel,
                   std::span<const std::string_view> allowedHosts = {});

// The download seam of the import and of the tree update: production passes a lambda over HttpGet
// bound to its allow-list, tests pass a fake that answers from memory.
using HttpGetFn = std::function<HttpResult(const std::string& url, size_t maxBytes, uint32_t timeoutMs,
                                           const std::atomic<bool>* cancel)>;

} // namespace bp
