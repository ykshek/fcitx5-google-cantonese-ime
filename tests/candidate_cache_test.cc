#include "candidate_cache.h"

namespace {
std::vector<GoogleCandidate> candidates(const std::string &text) {
    return {{text, 1, text + "5"}};
}
} // namespace

int main() {
    CandidateCache cache(2);
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

    cache.setCapacity(1);
    if (cache.size() != 1 || !cache.get("hou", "yue", 9)) return 4;

    cache.setCapacity(0);
    if (cache.size() != 0) return 5;
    cache.put("nei", "yue", 9, candidates("你"));
    if (cache.get("nei", "yue", 9)) return 6;

    cache.setCapacity(1);
    cache.put("nei", "yue", 9, {});
    if (cache.size() != 0) return 7;
}
