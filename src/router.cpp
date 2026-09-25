#include "calc/router.hpp"

#include <cstdint>
#include <optional>
#include <string>

#include "calc/calculator.hpp"
#include "calc/query.hpp"
#include "calc/text.hpp"

namespace calc {

namespace {

bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }
bool is_ascii_alnum(char c) { return is_ascii_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_ascii_hex(char c) { return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

// Host = uri-host [ ":" port ]  (RFC 9110 §7.2), simplified to:
// "name[:port]", "1.2.3.4[:port]", or "[ipv6][:port]".
bool is_valid_host(std::string_view host) {
    if (host.empty()) return false;
    std::string_view port;
    if (host.front() == '[') {
        const std::size_t close = host.find(']');
        if (close == std::string_view::npos || close == 1) return false;
        for (const char c : host.substr(1, close - 1)) {
            if (!is_ascii_hex(c) && c != ':' && c != '.') return false;
        }
        const std::string_view rest = host.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return false;
            port = rest.substr(1);
        }
    } else {
        const std::size_t colon = host.find(':');
        const std::string_view name = host.substr(0, colon);
        if (name.empty()) return false;
        for (const char c : name) {
            if (!is_ascii_alnum(c) && c != '-' && c != '.' && c != '_' && c != '~') return false;
        }
        if (colon != std::string_view::npos) port = host.substr(colon + 1);
    }
    for (const char c : port) {
        if (!is_ascii_digit(c)) return false;
    }
    return true;
}

std::optional<std::string> parse_operand(const char* name, const std::string& text, std::int64_t& value) {
    const std::string quoted = std::string("query parameter '") + name + "'";
    switch (parse_int64(text, value)) {
        case IntParseStatus::Ok:           return std::nullopt;
        case IntParseStatus::Empty:        return quoted + " is empty";
        case IntParseStatus::NotAnInteger: return quoted + " is not an integer: '" + excerpt(text) + "'";
        case IntParseStatus::OutOfRange:   return quoted + " is out of range for a signed 64-bit integer";
    }
    return quoted + " is invalid";
}

// The calculator's interface is exactly {a, b}. Missing, duplicated, empty,
// or unknown parameters are all rejected rather than guessed at. For
// example, "a=1&a=2" could mean either value, and silently ignoring "c=3"
// would hide a client bug.
std::optional<std::string> parse_operands(std::string_view query, std::int64_t& a, std::int64_t& b) {
    const QueryParseResult parsed = parse_query(query);
    if (!parsed.ok()) return parsed.error;

    const std::string* a_text = nullptr;
    const std::string* b_text = nullptr;
    for (const auto& [name, value] : parsed.parameters) {
        if (name != "a" && name != "b") {
            return "unknown query parameter '" + excerpt(name) + "'; only a and b are accepted";
        }
        const std::string*& slot = name == "a" ? a_text : b_text;
        if (slot != nullptr) return "duplicate query parameter '" + name + "'";
        slot = &value;
    }

    if (a_text == nullptr && b_text == nullptr) return std::string("missing query parameters 'a' and 'b'");
    if (a_text == nullptr) return std::string("missing query parameter 'a'");
    if (b_text == nullptr) return std::string("missing query parameter 'b'");
    if (auto error = parse_operand("a", *a_text, a)) return error;
    if (auto error = parse_operand("b", *b_text, b)) return error;
    return std::nullopt;
}

}  // namespace

Response handle_request(const Request& request) {
    // 1. RFC 9112 §3.2: respond 400 to an HTTP/1.1 request without a Host
    //    header, and to any request with more than one Host header or an
    //    invalid one. This is a protocol rule, so it comes before routing.
    const std::size_t host_headers = request.header_count("Host");
    if (host_headers > 1) return text_response(400, "multiple Host headers");
    if (host_headers == 0 && request.version == HttpVersion::Http11) {
        return text_response(400, "missing Host header (required in HTTP/1.1)");
    }
    if (host_headers == 1 && !is_valid_host(*request.header("Host"))) {
        return text_response(400, "invalid Host header '" + excerpt(*request.header("Host")) + "'");
    }

    // 2. Does the resource exist at all? An unknown path is 404 whatever the
    //    method, because there is no resource whose methods could be listed.
    const std::optional<Operation> operation = operation_from_path(request.path());
    if (!operation) {
        return text_response(404, "unknown operation '" + excerpt(request.path()) +
                                      "'; supported: /add /sub /mul /div");
    }

    // 3. The resource exists but only supports GET. 405 must name the
    //    allowed methods in an Allow header (RFC 9110 §15.5.6).
    if (request.method != "GET") {
        Response response = text_response(405, "method " + excerpt(request.method) + " not allowed; use GET");
        response.headers.push_back(Header{"Allow", "GET"});
        return response;
    }

    // 4. Operands.
    std::int64_t a = 0;
    std::int64_t b = 0;
    if (auto error = parse_operands(request.query(), a, b)) return text_response(400, *error);

    // 5. Arithmetic. Division by zero and overflow are problems with the
    //    client's input, so they are 400, not 500.
    const CalcResult result = calculate(*operation, a, b);
    if (!result.ok) return text_response(400, result.error);

    return text_response(200, std::to_string(result.value));
}

}  // namespace calc
