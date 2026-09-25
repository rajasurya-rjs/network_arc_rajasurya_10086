#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace calc {

// ASCII case-insensitive comparison, as header names and connection options require.
bool iequals(std::string_view a, std::string_view b);

// Strips optional whitespace (spaces and tabs) from both ends.
std::string_view trim_whitespace(std::string_view text);

// Client-supplied text made safe to quote in an error message: truncated to
// max_length and with non-printable bytes replaced by '?'.
std::string excerpt(std::string_view text, std::size_t max_length = 40);

}  // namespace calc
