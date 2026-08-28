#pragma once

#include <string>
#include <vector>

// Query Google Input Tools directly over HTTPS and return a list of
// candidates.
//
// This replaces the former local Python daemon: the fcitx5 plugin now
// performs the HTTP request and JSON parsing itself, so there is no separate
// process to start and no Python runtime dependency.

// One candidate as returned by Google, including how many latin characters
// of the *preedit* (not the committed context) it consumes. This is what
// makes word-by-word candidate selection possible: a candidate does not
// necessarily consume the whole preedit, e.g. for preedit "singsidaih",
// Google may offer "城市大學" (matchedLength=10, consumes everything) as well
// as "城市" (matchedLength=6, consumes only "singsi").
struct GoogleCandidate {
    std::string text;
    // Number of leading bytes (latin, so bytes == chars) of the *preedit*
    // this candidate consumes. Always in [1, preedit.size()].
    int matchedLength = 0;
};

// text: raw romanized input typed by the user (e.g. latin letters / jyutping)
//       optionally prefixed with committed Chinese context in the format
//       Google's own web client uses: "|<committed context>,<preedit>".
//       When there is no committed context yet, just pass the preedit alone.
// itc: Google Input Tools code, e.g. "yue-hant-t-i0-und" (Cantonese, Traditional)
//      or "zh-t-i0-pinyin" (Mandarin pinyin)
// num: number of candidates requested
std::vector<GoogleCandidate> get_candidates(const std::string& text,
                                            const std::string& itc = "yue-hant-t-i0-und",
                                            int num = 13);
