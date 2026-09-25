#include "calc/calculator.hpp"

#include <charconv>
#include <limits>

namespace calc {

std::optional<Operation> operation_from_path(std::string_view path) {
    if (path == "/add") return Operation::Add;
    if (path == "/sub") return Operation::Subtract;
    if (path == "/mul") return Operation::Multiply;
    if (path == "/div") return Operation::Divide;
    return std::nullopt;
}

IntParseStatus parse_int64(std::string_view text, std::int64_t& value) {
    if (text.empty()) return IntParseStatus::Empty;

    const std::size_t digits_start = text.front() == '-' ? 1 : 0;
    if (digits_start == text.size()) return IntParseStatus::NotAnInteger;  // just "-"
    for (std::size_t i = digits_start; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return IntParseStatus::NotAnInteger;
    }

    // The grammar is already checked, so from_chars can only fail on range.
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error == std::errc::result_out_of_range) return IntParseStatus::OutOfRange;
    if (error != std::errc{} || end != text.data() + text.size()) return IntParseStatus::NotAnInteger;
    return IntParseStatus::Ok;
}

CalcResult calculate(Operation operation, std::int64_t a, std::int64_t b) {
    CalcResult result;
    // Signed overflow is undefined behaviour in C++, so it has to be detected
    // before it happens. __builtin_*_overflow (GCC and Clang) computes the
    // exact result and reports whether it fits.
    bool overflow = false;
    switch (operation) {
        case Operation::Add:
            overflow = __builtin_add_overflow(a, b, &result.value);
            break;
        case Operation::Subtract:
            overflow = __builtin_sub_overflow(a, b, &result.value);
            break;
        case Operation::Multiply:
            overflow = __builtin_mul_overflow(a, b, &result.value);
            break;
        case Operation::Divide:
            if (b == 0) {
                result.error = "division by zero";
                return result;
            }
            // The one division that overflows: -2^63 / -1 = 2^63.
            overflow = a == std::numeric_limits<std::int64_t>::min() && b == -1;
            if (!overflow) result.value = a / b;
            break;
    }
    if (overflow) {
        result.value = 0;
        result.error = "integer overflow: result does not fit in a signed 64-bit integer";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace calc
