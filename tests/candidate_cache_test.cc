#include "candidate_cache.h"

#include <chrono>
#include <filesystem>
#include <system_error>

#include <unistd.h>

namespace {
std::vector<GoogleCandidate> candidates(const std::string &text) {
    return {{text, 1, text + "5"}};
}

struct TempCacheDirectory {
    std::filesystem::path path;
    ~TempCacheDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};
} // namespace

int main() {
    CandidateCache cache(2, std::chrono::minutes(5));
    cache.put("nei", "yue", 9, candidates("你"));
    cache.put("ngo", "yue", 9, candidates("我"));

    auto first = cache.get("nei", "yue", 9);
    if (!first || first->front().text != "你") return 1;

    cache.put("hou", "yue", 9, candidates("好"));
    if (!cache.get("nei", "yue", 9) ||
        cache.get("ngo", "yue", 9) ||
        !cache.get("hou", "yue", 9)) {
        return 2;
    }

    if (cache.get("hou", "other-itc", 9) ||
        cache.get("hou", "yue", 18)) {
        return 3;
    }

    cache.configure(1, std::chrono::minutes(5), false);
    if (cache.size() != 1 || !cache.get("hou", "yue", 9)) return 4;

    cache.configure(0, std::chrono::minutes(5), false);
    if (cache.size() != 0) return 5;
    cache.put("nei", "yue", 9, candidates("你"));
    if (cache.get("nei", "yue", 9)) return 6;

    cache.configure(1, std::chrono::minutes(5), false);
    cache.put("nei", "yue", 9, {});
    if (cache.size() != 0) return 7;

    CandidateCache noTtl(1, std::chrono::seconds(0));
    noTtl.put("nei", "yue", 9, candidates("你"));
    if (noTtl.get("nei", "yue", 9)) return 8;

    TempCacheDirectory tempDir{
        std::filesystem::temp_directory_path() /
        ("fcitx5-google-ime-cache-test-" + std::to_string(::getpid()))};
    std::error_code ec;
    std::filesystem::remove_all(tempDir.path, ec);
    const auto cacheFile = tempDir.path / "candidate-cache.json";
    {
        CandidateCache persistent(2, std::chrono::seconds(0), true, cacheFile);
        persistent.put("nei", "yue", 9, candidates("你"));
        if (!std::filesystem::exists(cacheFile)) return 9;
    }
    {
        CandidateCache restored(2, std::chrono::seconds(0), true, cacheFile);
        const auto persisted = restored.get("nei", "yue", 9);
        if (!persisted || persisted->front().text != "你") return 10;
        restored.configure(2, std::chrono::minutes(5), false);
        if (std::filesystem::exists(cacheFile) ||
            !restored.get("nei", "yue", 9)) {
            return 11;
        }
    }
}
