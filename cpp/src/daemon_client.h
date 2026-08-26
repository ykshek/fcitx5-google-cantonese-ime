#pragma once

#include <string>
#include <vector>

std::vector<std::string> query_daemon(const std::string& text, const std::string& itc = "zh-t-i0-pinyin", int num = 8);
