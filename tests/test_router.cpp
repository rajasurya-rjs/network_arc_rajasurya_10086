// Unit tests for the application layer: arithmetic, query parsing, routing
// and status-code selection, and response serialization.

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "calc/calculator.hpp"
#include "calc/query.hpp"
#include "calc/request.hpp"
#include "calc/response.hpp"
#include "calc/router.hpp"
#include "test_framework.hpp"

namespace {

using calc::Header;
using calc::HttpVersion;
using calc::Request;
using calc::Response;

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

Response call(const std::string& method, const std::string& target,
              std::vector<Header> headers = {{"Host", "localhost:8080"}}, HttpVersion version = HttpVersion::Http11) {
    Request request;
    request.method = method;
    request.target = target;
    request.version = version;
    request.headers = std::move(headers);
    return calc::handle_request(request);
}

Response get(const std::string& target) { return call("GET", target); }

void assignment_examples() {
    CHECK_EQ(get("/add?a=2&b=3").status, 200);
    CHECK_EQ(get("/add?a=2&b=3").body, "5\n");
    CHECK_EQ(get("/sub?a=10&b=4").body, "6\n");
    CHECK_EQ(get("/mul?a=6&b=7").body, "42\n");
    CHECK_EQ(get("/div?a=1&b=0").status, 400);
    CHECK_EQ(get("/pow?a=2&b=8").status, 404);
    const Response post = call("POST", "/add");
    CHECK_EQ(post.status, 405);
    REQUIRE(post.headers.size() == 1);
    CHECK_EQ(post.headers[0].name, "Allow");
    CHECK_EQ(post.headers[0].value, "GET");
    CHECK_EQ(call("GET", "/add?a=2&b=3", {}).status, 400);  // no Host
}

void arithmetic_semantics() {
    CHECK_EQ(get("/div?a=84&b=2").body, "42\n");
    CHECK_EQ(get("/div?a=7&b=2").body, "3\n");     // integer division
    CHECK_EQ(get("/div?a=-7&b=2").body, "-3\n");   // truncates toward zero
    CHECK_EQ(get("/sub?a=4&b=10").body, "-6\n");
    CHECK_EQ(get("/mul?a=-6&b=-7").body, "42\n");
    CHECK_EQ(get("/add?a=007&b=-0").body, "7\n");  // leading zeros, negative zero
    CHECK_EQ(get("/add?b=3&a=2").body, "5\n");     // parameter order does not matter
    CHECK_EQ(get("/add?a=%2D5&b=%31").body, "-4\n");  // percent-encoded "-5" and "1"
    CHECK_EQ(get("/add?a=-9223372036854775808&b=0").body, "-9223372036854775808\n");
    CHECK_EQ(get("/add?a=9223372036854775807&b=0").body, "9223372036854775807\n");
}

void overflow_is_400() {
    const std::string max = std::to_string(kMax);
    const std::string min = std::to_string(kMin);
    CHECK_EQ(get("/add?a=" + max + "&b=1").status, 400);
    CHECK_EQ(get("/sub?a=" + min + "&b=1").status, 400);
    CHECK_EQ(get("/mul?a=" + max + "&b=2").status, 400);
    CHECK_EQ(get("/div?a=" + min + "&b=-1").status, 400);
    CHECK_EQ(get("/add?a=9223372036854775808&b=0").status, 400);  // literal out of range
    CHECK_EQ(get("/add?a=99999999999999999999999&b=0").status, 400);
}

void invalid_operands_are_400() {
    const std::vector<std::string> bad = {
        "/add",                  // no query at all
        "/add?",                 // empty query
        "/add?a=1",              // missing b
        "/add?b=1",              // missing a
        "/add?a=&b=1",           // empty value
        "/add?a=1&b=",
        "/add?a=1&a=2&b=3",      // duplicate
        "/add?a=1&b=2&b=2",
        "/add?a=1&b=2&c=3",      // unknown parameter
        "/add?a=1&&b=2",         // empty segment
        "/add?a=1&b=2&",
        "/add?a&b=2",            // no '='
        "/add?a=1.5&b=2",        // not an integer
        "/add?a=1e3&b=2",
        "/add?a=0x10&b=2",
        "/add?a=+1&b=2",
        "/add?a=--1&b=2",
        "/add?a=-&b=2",
        "/add?a=%201&b=2",       // decodes to " 1"
        "/add?a=abc&b=2",
        "/add?a=%zz&b=2",        // bad percent-escape
        "/add?a=%4&b=2",
        "/add?a=1&b=2%",
    };
    for (const std::string& target : bad) {
        const Response response = get(target);
        if (!CHECK_EQ(response.status, 400)) std::cout << "      (target: " << target << ")\n";
    }
    CHECK_EQ(get("/add?a=1").body, "missing query parameter 'b'\n");
    CHECK_EQ(get("/add?a=1&a=2&b=3").body, "duplicate query parameter 'a'\n");
}

void unknown_paths_are_404() {
    for (const std::string target : {"/", "/pow?a=2&b=8", "/add/", "/ADD?a=1&b=2", "/addx", "/%61dd?a=1&b=2"}) {
        if (!CHECK_EQ(get(target).status, 404)) std::cout << "      (target: " << target << ")\n";
    }
}

void other_methods_are_405() {
    for (const std::string method : {"POST", "PUT", "DELETE", "HEAD", "PATCH", "get"}) {
        const Response response = call(method, "/add?a=1&b=2");
        if (!CHECK_EQ(response.status, 405)) std::cout << "      (method: " << method << ")\n";
    }
}

void check_order_host_then_path_then_method() {
    CHECK_EQ(call("POST", "/pow").status, 404);            // no such resource: 404 even for POST
    CHECK_EQ(call("POST", "/add", {}).status, 400);        // missing Host beats 405
    CHECK_EQ(call("GET", "/pow?a=1&b=2", {}).status, 400);  // missing Host beats 404
    CHECK_EQ(call("POST", "/add?a=x").status, 405);        // method checked before operands
}

void host_header_rules() {
    CHECK_EQ(call("GET", "/add?a=1&b=2", {{"Host", "a"}, {"Host", "b"}}).status, 400);  // duplicate
    for (const std::string host : {"", "local host", "a/b", "x:80a", "[::1", "@evil", ":8080"}) {
        if (!CHECK_EQ(call("GET", "/add?a=1&b=2", {{"Host", host}}).status, 400)) {
            std::cout << "      (Host: '" << host << "')\n";
        }
    }
    for (const std::string host : {"localhost", "localhost:8080", "127.0.0.1:8080", "[::1]:8080", "example.com:"}) {
        if (!CHECK_EQ(call("GET", "/add?a=1&b=2", {{"Host", host}}).status, 200)) {
            std::cout << "      (Host: '" << host << "')\n";
        }
    }
    // HTTP/1.0 predates the Host requirement.
    CHECK_EQ(call("GET", "/add?a=1&b=2", {}, HttpVersion::Http10).status, 200);
}

void calculator_directly() {
    calc::CalcResult r = calc::calculate(calc::Operation::Divide, 1, 0);
    CHECK(!r.ok);
    CHECK_EQ(r.error, "division by zero");
    r = calc::calculate(calc::Operation::Multiply, kMin, -1);
    CHECK(!r.ok);
    r = calc::calculate(calc::Operation::Subtract, 0, kMin);
    CHECK(!r.ok);
    r = calc::calculate(calc::Operation::Add, kMax, kMin);
    CHECK(r.ok);
    CHECK_EQ(r.value, -1);

    std::int64_t value = 0;
    CHECK(calc::parse_int64("-42", value) == calc::IntParseStatus::Ok);
    CHECK_EQ(value, -42);
    CHECK(calc::parse_int64("", value) == calc::IntParseStatus::Empty);
    CHECK(calc::parse_int64("4 2", value) == calc::IntParseStatus::NotAnInteger);
    CHECK(calc::parse_int64("-9223372036854775809", value) == calc::IntParseStatus::OutOfRange);
}

void query_parsing() {
    const calc::QueryParseResult q = calc::parse_query("a=1&b=%2B2&c%3D=x+y");
    REQUIRE(q.ok());
    REQUIRE(q.parameters.size() == 3);
    CHECK_EQ(q.parameters[1].second, "+2");
    CHECK_EQ(q.parameters[2].first, "c=");
    CHECK_EQ(q.parameters[2].second, "x+y");  // '+' is not a space in a URI query
    CHECK(calc::parse_query("").ok());
    CHECK(!calc::parse_query("a=1&").ok());
}

void serializes_exact_wire_bytes() {
    Response response = calc::text_response(200, "5");
    CHECK_EQ(calc::serialize(response),
             "HTTP/1.1 200 OK\r\n"
             "Content-Length: 2\r\n"
             "Content-Type: text/plain\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "5\n");

    response = calc::text_response(405, "nope");
    response.headers.push_back({"Allow", "GET"});
    response.close_connection = true;
    CHECK_EQ(calc::serialize(response),
             "HTTP/1.1 405 Method Not Allowed\r\n"
             "Content-Length: 5\r\n"
             "Content-Type: text/plain\r\n"
             "Connection: close\r\n"
             "Allow: GET\r\n"
             "\r\n"
             "nope\n");
}

void head_response_omits_the_body() {
    Response response = calc::text_response(405, "nope");
    response.omit_body = true;
    const std::string wire = calc::serialize(response);
    CHECK(wire.find("Content-Length: 5\r\n") != std::string::npos);  // size the body would have had
    CHECK_EQ(wire.substr(wire.size() - 4), "\r\n\r\n");               // ...but the message ends at the blank line
}

void content_length_counts_bytes_not_characters() {
    Response response;
    response.body = "\xC3\xA9t\xC3\xA9";  // "été" in UTF-8: 3 characters, 5 bytes
    const std::string wire = calc::serialize(response);
    CHECK(wire.find("Content-Length: 5\r\n") != std::string::npos);
    CHECK_EQ(wire.substr(wire.size() - 5), response.body);
}

void every_status_has_a_reason_phrase() {
    for (const int status : {200, 400, 404, 405, 408, 413, 431, 500, 501, 505}) {
        CHECK(calc::reason_phrase(status) != "Unknown");
    }
}

}  // namespace

std::vector<test::TestCase> router_tests() {
    return {
        {"router: assignment examples", assignment_examples},
        {"router: arithmetic semantics", arithmetic_semantics},
        {"router: overflow is 400", overflow_is_400},
        {"router: invalid operands are 400", invalid_operands_are_400},
        {"router: unknown paths are 404", unknown_paths_are_404},
        {"router: other methods are 405", other_methods_are_405},
        {"router: check order Host -> path -> method", check_order_host_then_path_then_method},
        {"router: Host header rules", host_header_rules},
        {"calculator: direct arithmetic and integer parsing", calculator_directly},
        {"query: parsing and percent-decoding", query_parsing},
        {"response: serializes exact wire bytes", serializes_exact_wire_bytes},
        {"response: HEAD response omits the body", head_response_omits_the_body},
        {"response: Content-Length counts bytes, not characters", content_length_counts_bytes_not_characters},
        {"response: every status has a reason phrase", every_status_has_a_reason_phrase},
    };
}
