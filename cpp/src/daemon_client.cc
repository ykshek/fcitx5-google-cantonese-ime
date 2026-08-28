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
std::string build_url(CURL* curl, const std::string& text,
                     const std::string& itc, int num) {
    char* esc_text = curl_easy_escape(curl, text.c_str(), 0);
    char* esc_itc = curl_easy_escape(curl, itc.c_str(), 0);
    std::string url = std::string(kGoogleInputToolsUrl) + "?text=" +
                      (esc_text ? esc_text : "") +
                      "&itc=" + (esc_itc ? esc_itc : "") +
                      "&num=" + std::to_string(num);
    if (esc_text) curl_free(esc_text);
    if (esc_itc) curl_free(esc_itc);
    return url;
}
}  // namespace

std::vector<std::string> get_candidates(const std::string& text,
                                        const std::string& itc, int num) {
    std::vector<std::string> out;
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
    //   ["SUCCESS", [["<original_text>", ["cand1", "cand2", ...], [], {...}]]]
    //
    // nlohmann::json decodes \uXXXX escapes (and UTF-16 surrogate pairs) into
    // real UTF-8 for us, so the candidate strings returned here are already
    // valid UTF-8 -- no manual unescaping is needed in the engine.
    //
    // We parse strictly: require data[0] == "SUCCESS" and data[1][0][1] to be
    // an array. We deliberately do NOT fall back to scanning every string in
    // the JSON, since that would surface junk like "SUCCESS" or the original
    // query as candidates in the IME panel.
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
        const auto& cands = first[1];
        if (!cands.is_array()) return out;
        for (const auto& c : cands) {
            if (c.is_string()) out.push_back(c.get<std::string>());
        }
    } catch (const std::exception& e) {
        std::cerr << "get_candidates: JSON parse error: " << e.what() << "\n";
    }

    return out;
}
