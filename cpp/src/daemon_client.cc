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

    CURL* curl = curl_easy_init();
    if (!curl) {
        std::cerr << "get_candidates: curl_easy_init failed\n";
        return out;
    }

    std::string url = build_url(curl, text, itc, num);
    std::string response;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    // NOSIGNAL avoids libcurl using signals for DNS timeouts, which is unsafe
    // in multi-threaded code (this runs on detached worker threads in fcitx5).
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    // A normal browser-like User-Agent avoids being filtered out by naive
    // server-side checks on the public endpoint.
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                     "(KHTML, like Gecko) Chrome/120.0 Safari/537.36");

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

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
