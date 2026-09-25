#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace calc {

// Pure arithmetic with no networking or HTTP. Operands and results are
// 64-bit signed integers, and every operation reports overflow explicitly
// instead of wrapping around or triggering undefined behaviour.

enum class Operation { Add, Subtract, Multiply, Divide };

// "/add" -> Add, and so on. Exact, case-sensitive match. Anything else is nullopt.
std::optional<Operation> operation_from_path(std::string_view path);

enum class IntParseStatus { Ok, Empty, NotAnInteger, OutOfRange };

// Accepts exactly  -?[0-9]+  that fits in int64_t. No '+', no spaces, no
// hex, no decimals. Leading zeros are allowed ("007" is 7).
IntParseStatus parse_int64(std::string_view text, std::int64_t& value);

struct CalcResult {
    bool ok = false;
    std::int64_t value = 0;
    std::string error;  // set when !ok
};

// Division truncates toward zero (7 / 2 = 3, -7 / 2 = -3), as in C++.
CalcResult calculate(Operation operation, std::int64_t a, std::int64_t b);

}  // namespace calc
