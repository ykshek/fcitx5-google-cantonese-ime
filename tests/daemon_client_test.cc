#include "daemon_client.h"

#include <string>

int main() {
    const auto candidates = parse_candidates_response(
        R"(["SUCCESS",[["nei",["你","呢","妳",""],[],{"matched_length":[3,2,18446744073709551615,0],"annotation":["nei",null,"nei5"]}]]])",
        "yue-hant-t-i0-und");
    if (candidates.size() != 3 ||
        candidates[0].text != "你" ||
        candidates[0].matchedLength != 3 ||
        candidates[0].annotation != "nei" ||
        candidates[1].text != "呢" ||
        candidates[1].matchedLength != 2 ||
        !candidates[1].annotation.empty() ||
        candidates[2].text != "妳" ||
        candidates[2].matchedLength != 3 ||
        candidates[2].annotation != "nei5") {
        return 1;
    }

    const auto defaults = parse_candidates_response(
        R"(["SUCCESS",[["nei",["你","呢"],[]]]])");
    if (defaults.size() != 2 || defaults[0].matchedLength != 3 ||
        defaults[1].matchedLength != 3) {
        return 2;
    }

    if (!parse_candidates_response(R"(["ERROR",[]])").empty() ||
        !parse_candidates_response("not-json").empty() ||
        !parse_candidates_response(R"(["SUCCESS",[["nei",[],[]]]])").empty()) {
        return 3;
    }
}
