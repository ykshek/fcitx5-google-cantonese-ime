#pragma once

#include <string>
#include <vector>

std::vector<std::string> query_daemon(const std::string& text, const std::string& itc = "yue-hant-t-i0-und", int num = 8);
