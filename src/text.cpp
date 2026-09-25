#include "calc/text.hpp"

namespace calc {

namespace {

char to_lower_ascii(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (to_lower_ascii(a[i]) != to_lower_ascii(b[i])) return false;
    }
    return true;
}

std::string_view trim_whitespace(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

std::string excerpt(std::string_view text, std::size_t max_length) {
    std::string out;
    for (std::size_t i = 0; i < text.size() && i < max_length; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        out += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?';
    }
    if (text.size() > max_length) out += "...";
    return out;
}

}  // namespace calc
