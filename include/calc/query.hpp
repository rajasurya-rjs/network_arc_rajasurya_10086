#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace calc {

// Decodes %XX escapes (RFC 3986 §2.1). '+' is left as a literal '+'; the
// "+ means space" rule belongs to HTML form encoding, not to URIs in
// general. Returns nullopt for a malformed escape such as "%4" or "%zz".
std::optional<std::string> percent_decode(std::string_view text);

struct QueryParseResult {
    std::vector<std::pair<std::string, std::string>> parameters;  // decoded, in order
    std::string error;                                            // set on failure
    bool ok() const { return error.empty(); }
};

// Splits "a=2&b=3" into [("a","2"), ("b","3")]. Strict: an empty segment
// ("a=1&&b=2"), a segment without '=' ("a&b=2"), or a bad escape is an error.
QueryParseResult parse_query(std::string_view query);

}  // namespace calc
