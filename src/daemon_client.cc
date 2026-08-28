#include "daemon_client.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {
// Google Input Tools public endpoint. The daemon used to proxy this same URL
// through a local Flask server; we now call it directly from the plugin.
constexpr char kGoogleInputToolsUrl[] = "https://inputtools.google.com/request";

// libcurl requires a one-time global initialization before curl_easy_init()
// can be used. Because get_candidates() runs on detached worker threads inside
// the fcitx5 process, guard the init with std::call_once so it happens exactly
// once even under concurrent keystrokes.
std::once_flag g_curl_once;

void ensure_curl_global_init() {
    std::call_once(g_curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    static_cast<std::string*>(userp)->append(
        static_cast<char*>(contents), size * nmemb);
    return size * nmemb;
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
// If the pool is momentarily empty when a request arrives (all handles busy),
// we spin up an extra handle on the spot so that request is never blocked
// behind an in-flight one. Returned handles are kept idle up to a small cap;
// extra handles beyond that are simply cleaned up to avoid unbounded growth.
class CurlHandlePool {
public:
    // Upper bound on idle handles we keep around between bursts. Plenty for
    // realistic typing concurrency, and bounded so idle handles don't leak.
    static constexpr size_t kMaxIdle = 8;

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
        curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
        curl_easy_setopt(handle, CURLOPT_FORBID_REUSE, 0L);
        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
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
        // All pooled handles are busy: create a new one so this request can run
        // in parallel rather than waiting. curl_easy_init may return null on a
        // truly catastrophic failure (out of memory), which the caller handles
        // by treating it as an empty result.
        CURL* handle = curl_easy_init();
        if (handle) {
            applyStaticOptions(handle);
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
std::string build_url(CURL* curl, const std::string& text,
                     const std::string& itc, int num) {
    char* esc_text = curl_easy_escape(curl, text.c_str(), 0);
    char* esc_itc = curl_easy_escape(curl, itc.c_str(), 0);
    std::string url = std::string(kGoogleInputToolsUrl) + "?text=" +
                      (esc_text ? esc_text : "") +
                      "&itc=" + (esc_itc ? esc_itc : "") +
                      "&num=" + std::to_string(num) +
                      "&cp=0&cs=1&ie=utf-8&oe=utf-8&app=demopage";
    if (esc_text) curl_free(esc_text);
    if (esc_itc) curl_free(esc_itc);
    return url;
}
}  // namespace

std::vector<GoogleCandidate> get_candidates(const std::string& text,
                                            const std::string& itc, int num) {
    std::vector<GoogleCandidate> out;
    if (text.empty()) return out;

    ensure_curl_global_init();

    PooledHandle pooled;
    CURL* curl = pooled.handle;
    if (!curl) {
        std::cerr << "get_candidates: curl_easy_init failed\n";
        return out;
    }

    std::string url = build_url(curl, text, itc, num);
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    if (res != CURLE_OK) {
        std::cerr << "get_candidates: curl error: " << curl_easy_strerror(res)
                  << "\n";
        return out;
    }
    if (http_code != 200) {
        std::cerr << "get_candidates: HTTP " << http_code << "\n";
        return out;
    }


    // Google Input Tools response shape:
    //   ["SUCCESS", [["<echoed preedit>", ["cand1", "cand2", ...], [], {"matched_length":[...], "annotation":[...], ...}]]]
    //
    // nlohmann::json decodes \uXXXX escapes (and UTF-16 surrogate pairs) into
    // real UTF-8 for us, so the candidate strings returned here are already
    // valid UTF-8 -- no manual unescaping is needed in the engine.
    //
    // data[1][0][0] echoes back the preedit segment that matched_length is
    // relative to (Google strips any committed context we prefixed). When the
    // optional "matched_length" array is absent, each candidate is assumed to
    // consume the whole echoed preedit (i.e. a full match).
    try {
        json data = json::parse(response);
        if (!data.is_array() || data.size() < 2) return out;
        if (!data[0].is_string() || data[0].get<std::string>() != "SUCCESS") {
            std::cerr << "get_candidates: Google status not SUCCESS\n";
            return out;
        }
        const auto& results = data[1];
        if (!results.is_array() || results.empty()) return out;
        const auto& first = results[0];
        if (!first.is_array() || first.size() < 2) return out;

        // Echoed preedit segment (latin bytes == codepoints). Falls back to
        // 1 so a missing matched_length can never yield 0 (always in [1, N]).
        int defaultLen = 1;
        if (first[0].is_string()) {
            int n = static_cast<int>(first[0].get<std::string>().size());
            if (n > defaultLen) defaultLen = n;
        }

        // Optional per-candidate matched_length array.
        std::vector<int> matchedLengths;
        if (first.size() >= 4 && first[3].is_object() &&
            first[3].contains("matched_length") &&
            first[3]["matched_length"].is_array()) {
            for (const auto& m : first[3]["matched_length"]) {
                int v = defaultLen;
                if (m.is_number_integer()) v = m.get<int>();
                if (v < 1) v = 1;
                if (v > defaultLen) v = defaultLen;
                matchedLengths.push_back(v);
            }
        }

        const auto& cands = first[1];
        if (!cands.is_array()) return out;
        for (size_t i = 0; i < cands.size(); ++i) {
            if (!cands[i].is_string()) continue;
            GoogleCandidate c;
            c.text = cands[i].get<std::string>();
            c.matchedLength =
                (i < matchedLengths.size()) ? matchedLengths[i] : defaultLen;
            out.push_back(std::move(c));
        }
    } catch (const std::exception& e) {
        std::cerr << "get_candidates: JSON parse error: " << e.what() << "\n";
    }

    return out;
}
