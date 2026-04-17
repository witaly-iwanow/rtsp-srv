#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <string>
#include <string_view>

namespace util {

inline auto to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

inline auto to_upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

inline auto trim(const std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return std::string{};
    const auto last = value.find_last_not_of(" \t");
    return std::string(value.substr(first, last - first + 1));
}

inline bool parse_int(const std::string_view text, int& out) {
    const auto trimmed = trim(text);
    if (trimmed.empty())
        return false;

    int value = 0;
    const auto begin = trimmed.data();
    const auto end = begin + trimmed.size();
    if (const auto [ptr, ec] = std::from_chars(begin, end, value); ec != std::errc() || ptr != end)
        return false;

    out = value;
    return true;
}

} // namespace util
