#pragma once

#include <string>
#include <vector>

// Query Google Input Tools directly over HTTPS and return a list of candidate
// strings.
//
// This replaces the former local Python daemon: the fcitx5 plugin now performs
// the HTTP request and JSON parsing itself, so there is no separate process to
// start and no Python runtime dependency.
//
// text: raw romanized input typed by the user (e.g. latin letters / jyutping)
// itc: Google Input Tools code, e.g. "yue-hant-t-i0-und" (Cantonese, Traditional)
//      or "zh-t-i0-pinyin" (Mandarin pinyin)
// num: number of candidates requested
std::vector<std::string> get_candidates(const std::string& text,
                                       const std::string& itc = "yue-hant-t-i0-und",
                                       int num = 8);
