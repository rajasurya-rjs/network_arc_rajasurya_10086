#include "calc/query.hpp"

#include "calc/text.hpp"

namespace calc {

namespace {

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

std::optional<std::string> percent_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out += text[i];
            continue;
        }
        if (i + 2 >= text.size()) return std::nullopt;  // needs two hex digits after '%'
        const int high = hex_value(text[i + 1]);
        const int low = hex_value(text[i + 2]);
        if (high < 0 || low < 0) return std::nullopt;
        out += static_cast<char>(high * 16 + low);
        i += 2;
    }
    return out;
}

QueryParseResult parse_query(std::string_view query) {
    QueryParseResult result;
    if (query.empty()) return result;

    while (true) {
        const std::size_t amp = query.find('&');
        const std::string_view segment = query.substr(0, amp);

        if (segment.empty()) {
            result.error = "malformed query string: empty parameter (stray '&')";
            return result;
        }
        const std::size_t eq = segment.find('=');
        if (eq == std::string_view::npos) {
            result.error = "malformed query string: parameter '" + excerpt(segment) +
                           "' has no value (expected name=value)";
            return result;
        }
        auto name = percent_decode(segment.substr(0, eq));
        auto value = percent_decode(segment.substr(eq + 1));
        if (!name || !value) {
            result.error = "malformed query string: invalid percent-escape in '" + excerpt(segment) + "'";
            return result;
        }
        result.parameters.emplace_back(std::move(*name), std::move(*value));

        if (amp == std::string_view::npos) break;
        query.remove_prefix(amp + 1);
    }
    return result;
}

}  // namespace calc
