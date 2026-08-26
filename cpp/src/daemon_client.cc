#include "daemon_client.h"
#include <curl/curl.h>
#include <string>
#include <vector>
#include <cctype>
#include <iostream>

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::vector<std::string> query_daemon(const std::string& text, const std::string& itc, int num) {
    std::vector<std::string> out;
    CURL* curl = curl_easy_init();
    if (!curl) return out;

    char* esc_text = curl_easy_escape(curl, text.c_str(), 0);
    char* esc_itc = curl_easy_escape(curl, itc.c_str(), 0);
    std::string url = std::string("http://127.0.0.1:8765/suggest?q=") + (esc_text ? esc_text : "")
                      + "&itc=" + (esc_itc ? esc_itc : "")
                      + "&num=" + std::to_string(num);
    if (esc_text) curl_free(esc_text);
    if (esc_itc) curl_free(esc_itc);

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "curl error: " << curl_easy_strerror(res) << std::endl;
        curl_easy_cleanup(curl);
        return out;
    }
    curl_easy_cleanup(curl);

    auto pos = response.find("\"candidates\"");
    if (pos == std::string::npos) return out;
    auto start = response.find('[', pos);
    if (start == std::string::npos) return out;
    auto end = response.find(']', start);
    if (end == std::string::npos) return out;
    std::string arr = response.substr(start + 1, end - start - 1);

    size_t i = 0;
    while (i < arr.size()) {
        while (i < arr.size() && isspace((unsigned char)arr[i])) ++i;
        if (i >= arr.size()) break;
        if (arr[i] == '"') {
            size_t j = i + 1;
            std::string s;
            while (j < arr.size()) {
                if (arr[j] == '"' && arr[j-1] != '\\') break;
                s.push_back(arr[j]);
                ++j;
            }
            out.push_back(s);
            i = j + 1;
        } else {
            size_t j = i;
            while (j < arr.size() && arr[j] != ',') ++j;
            i = j + 1;
        }
    }
    return out;
}
