#include "daemon_client.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fcitx-utils/log.h>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {
// Google Input Tools public endpoint. The daemon used to proxy this same URL
// through a local Flask server; we now call it directly from the plugin.
constexpr char kGoogleInputToolsUrl[] = "https://inputtools.google.com/request";
constexpr size_t kMaxResponseBytes = 1U << 20;

// libcurl requires a one-time global initialization before curl_easy_init()
// can be used. Because get_candidates() runs on detached worker threads inside
// the fcitx5 process, guard the init with std::call_once so it happens exactly
// once even under concurrent keystrokes.
std::once_flag g_curl_once;

void ensure_curl_global_init() {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    if (size != 0 && nmemb > static_cast<size_t>(-1) / size) return 0;
    const size_t bytes = size * nmemb;
    auto *response = static_cast<std::string*>(userp);
    if (response->size() > kMaxResponseBytes ||
        bytes > kMaxResponseBytes - response->size()) {
        return 0;
    }
    response->append(static_cast<char*>(contents), bytes);
    return bytes;
}

// --- Reusable libcurl easy-handle pool ---
//
// The IME fires one Google request per keystroke. If each request created a
// brand-new CURL handle (as the original code did via curl_easy_init +
// curl_easy_cleanup), every lookup would pay for a fresh TCP connect + TLS
// handshake (~50-100ms) against inputtools.google.com — by far the dominant
// component of perceived typing latency, and the main reason the IME felt
// noticeably laggier than typing directly on Google's own web page (where the
// browser keeps a single persistent connection warm across requests).
//
// To avoid that, we keep a small pool of already-initialized CURL handles and
// reuse them across requests. libcurl's per-handle connection cache then keeps
// the underlying TCP+TLS connection to Google alive (HTTP/1.1 keep-alive) so
// subsequent requests reuse the existing connection instead of reconnecting.
//
// Pooling (rather than a single shared handle behind a mutex) is intentional:
// fast typing can have more than one request in flight at a time (a new lookup
// is fired before the previous one has completed), and a single handle would
// serialize them, making the newest query wait behind a now-stale older one.
// A pool lets concurrent requests run in parallel while still reusing warmed
// connections once they are returned to the pool.
//
// Thread-safety: the pool vector and its idle count are guarded by a mutex.
// Each worker thread checks out one handle for the duration of curl_easy_perform
// and returns it to the idle pool when done (RAII via PooledHandle below).
//
// Concurrent handles are capped as well as idle handles, so a slow endpoint
// cannot cause this IME to open an unbounded number of connections.
class CurlHandlePool {
public:
    // Upper bound on idle handles we keep around between bursts. Plenty for
    // realistic typing concurrency, and bounded so idle handles don't leak.
    static constexpr size_t kMaxIdle = 8;
    static constexpr size_t kMaxTotal = 8;

    // Configure all of these on a freshly created handle so that EVERY
    // request reusing it inherits them automatically after a curl_easy_reset
    // (curl_easy_reset resets all options but leaves the connection cache
    // intact, which is what we rely on for keep-alive). These defaults are
    // static per request, so configuring them once in the constructor is fine.
    static void applyStaticOptions(CURL* handle) {
        // Explicitly allow connection reuse (the defaults already favor reuse,
        // but setting these documents the intent and guards against a future
        // libcurl build that flips a default). This is what keeps the TCP+TLS
        // connection to Google warm across requests.
        curl_easy_setopt(handle, CURLOPT_FRESH_CONNECT, 0L);
        const auto *version = curl_version_info(CURLVERSION_NOW);
        const long httpVersion =
            version && (version->features & CURL_VERSION_HTTP2)
                ? CURL_HTTP_VERSION_2TLS
                : CURL_HTTP_VERSION_1_1;
        curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, httpVersion);
        curl_easy_setopt(handle, CURLOPT_FORBID_REUSE, 0L);
        // With synchronous DNS resolvers, libcurl's timeout cannot interrupt
        // a blocking getaddrinfo() while NOSIGNAL is enabled.
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(handle, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
        // TCP keepalive probes keep the idle connection alive so Google's load
        // balancer doesn't close it between keystrokes during a pause.
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(handle, CURLOPT_USERAGENT,
                         "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                         "(KHTML, like Gecko) Chrome/120.0 Safari/537.36");
    }

    CURL* acquire() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!idle_.empty()) {
            CURL* handle = idle_.back();
            idle_.pop_back();
            return handle;
        }
        if (total_ >= kMaxTotal) return nullptr;
        // All pooled handles are busy: create a new one so this request can run
        // in parallel rather than waiting. curl_easy_init may return null on a
        // truly catastrophic failure (out of memory), which the caller handles
        // by treating it as an empty result.
        CURL* handle = curl_easy_init();
        if (handle) {
            applyStaticOptions(handle);
            ++total_;
        }
        return handle;
    }

    void release(CURL* handle) {
        if (!handle) return;
        // Reset all options to their defaults for the next request, but keep
        // the connection cache (which curl_easy_reset does NOT touch). This is
        // what lets the next request reuse the existing TCP+TLS connection.
        curl_easy_reset(handle);
        // Re-apply the static options that the reset just cleared (these are
        // the per-handle settings we always want to keep set across requests,
        // since they don't change per keystroke).
        applyStaticOptions(handle);

        std::lock_guard<std::mutex> lk(mutex_);
        if (idle_.size() >= kMaxIdle) {
            // Too many idle handles: clean this one up to avoid leaking.
            curl_easy_cleanup(handle);
            --total_;
        } else {
            idle_.push_back(handle);
        }
    }

    ~CurlHandlePool() {
        std::lock_guard<std::mutex> lk(mutex_);
        for (CURL* handle : idle_) {
            if (handle) curl_easy_cleanup(handle);
        }
        idle_.clear();
    }

private:
    std::vector<CURL*> idle_;
    size_t total_ = 0;
    std::mutex mutex_;
};

// Singleton pool. Intentionally never destroyed: a function-local static
// would otherwise be torn down at process/addon-unload time, but detached
// worker threads may still be mid-request holding a pooled handle or about to
// return one to the pool at that moment — destroying the pool (and cleaning up
// its CURL handles) out from under them would be a use-after-free. Making it
// process-lifetime (heap-allocated, never freed) trades a bounded leak of at
// most kMaxIdle idle CURL handles for shutdown safety. This mirrors why we also
// skip curl_global_cleanup() for the same detached-worker reason.
CurlHandlePool& handlePool() {
    static auto *pool = new CurlHandlePool;
    return *pool;
}

// RAII wrapper that always returns a borrowed handle back to the pool, even if
// curl_easy_perform throws or the caller returns early (e.g. on a parse
// error). This is what guarantees we never leak a handle out of the pool.
struct PooledHandle {
    CURL* handle;
    explicit PooledHandle() : handle(handlePool().acquire()) {}
    ~PooledHandle() {
        if (handle) handlePool().release(handle);
    }
    PooledHandle(const PooledHandle&) = delete;
    PooledHandle& operator=(const PooledHandle&) = delete;
};

// Build the request URL with proper URL-encoding for the query parameters.
// Note: Google's endpoint expects `text=` (the old daemon accepted `q=` and
// translated it to `text` internally); we pass `text` directly here.
// We also emit the small set of context params the official Google Input
// Tools web client sends (cp/cs/ie/oe/app) for closest parity with the site.
std::optional<std::string> build_url(CURL* curl, const std::string& text,
                                    const std::string& itc, int num) {
    char* esc_text = curl_easy_escape(curl, text.c_str(), 0);
    char* esc_itc = curl_easy_escape(curl, itc.c_str(), 0);
    if (!esc_text || !esc_itc) {
        if (esc_text) curl_free(esc_text);
        if (esc_itc) curl_free(esc_itc);
        return std::nullopt;
    }
    std::string url = std::string(kGoogleInputToolsUrl) + "?text=" +
                      esc_text +
                      "&itc=" + esc_itc +
                      "&num=" + std::to_string(num) +
                      "&cp=0&cs=1&ie=utf-8&oe=utf-8&app=demopage";
    curl_free(esc_text);
    curl_free(esc_itc);
    return url;
}
}  // namespace

std::vector<GoogleCandidate>
parse_candidates_response(const std::string& response, const std::string& itc) {
    std::vector<GoogleCandidate> out;

    // Google Input Tools returns an echoed preedit and parallel optional
    // matched_length / annotation arrays alongside its candidate strings.
    try {
        json data = json::parse(response);
        if (!data.is_array() || data.size() < 2) return out;
        if (!data[0].is_string() || data[0].get<std::string>() != "SUCCESS") {
            FCITX_WARN() << "get_candidates: Google status not SUCCESS"
                         << " (itc=" << itc << ")";
            return out;
        }
        const auto& results = data[1];
        if (!results.is_array() || results.empty()) return out;
        const auto& first = results[0];
        if (!first.is_array() || first.size() < 2) return out;

        int defaultLen = 1;
        if (first[0].is_string()) {
            const auto length = first[0].get<std::string>().size();
            defaultLen = static_cast<int>(
                std::min(length, static_cast<size_t>(std::numeric_limits<int>::max())));
            if (defaultLen < 1) defaultLen = 1;
        }

        std::vector<int> matchedLengths;
        if (first.size() >= 4 && first[3].is_object() &&
            first[3].contains("matched_length") &&
            first[3]["matched_length"].is_array()) {
            for (const auto& match : first[3]["matched_length"]) {
                int value = defaultLen;
                if (match.is_number_unsigned()) {
                    const auto raw = match.get<uint64_t>();
                    value = raw > static_cast<uint64_t>(defaultLen)
                                ? defaultLen
                                : std::max(1, static_cast<int>(raw));
                } else if (match.is_number_integer()) {
                    const auto raw = match.get<int64_t>();
                    value = static_cast<int>(std::clamp<int64_t>(
                        raw, 1, static_cast<int64_t>(defaultLen)));
                }
                matchedLengths.push_back(value);
            }
        }

        std::vector<std::string> annotations;
        if (first.size() >= 4 && first[3].is_object() &&
            first[3].contains("annotation") &&
            first[3]["annotation"].is_array()) {
            for (const auto& annotation : first[3]["annotation"]) {
                annotations.push_back(annotation.is_string()
                                          ? annotation.get<std::string>()
                                          : std::string{});
            }
        }

        const auto& candidates = first[1];
        if (!candidates.is_array()) return out;
        for (size_t i = 0; i < candidates.size(); ++i) {
            if (!candidates[i].is_string()) continue;
            GoogleCandidate candidate;
            candidate.text = candidates[i].get<std::string>();
            if (candidate.text.empty()) continue;
            candidate.matchedLength =
                i < matchedLengths.size() ? matchedLengths[i] : defaultLen;
            candidate.annotation =
                i < annotations.size() ? annotations[i] : "";
            out.push_back(std::move(candidate));
        }
    } catch (const std::exception& e) {
        FCITX_WARN() << "get_candidates: JSON parse error: " << e.what();
    }

    if (out.empty()) {
        FCITX_DEBUG() << "get_candidates: SUCCESS response had no candidates"
                      << " (itc=" << itc << ")";
    }
    return out;
}

std::vector<GoogleCandidate> get_candidates(const std::string& text,
                                            const std::string& itc, int num) {
    std::vector<GoogleCandidate> out;
    if (text.empty()) return out;

    ensure_curl_global_init();

    PooledHandle pooled;
    CURL* curl = pooled.handle;
    if (!curl) {
        FCITX_WARN() << "get_candidates: request concurrency limit reached or CURL initialization failed";
        return out;
    }

    auto url = build_url(curl, text, itc, num);
    if (!url) {
        FCITX_WARN() << "get_candidates: failed to escape request parameters";
        return out;
    }
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url->c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     static_cast<curl_off_t>(kMaxResponseBytes));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK) {
        FCITX_WARN() << "get_candidates: curl error: "
                     << curl_easy_strerror(res) << " (itc=" << itc << ")";
        return out;
    }
    if (http_code != 200) {
        FCITX_WARN() << "get_candidates: HTTP " << http_code
                     << " (itc=" << itc << ")";
        return out;
    }


    return parse_candidates_response(response, itc);
}
