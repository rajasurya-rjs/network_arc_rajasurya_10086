// Unit tests for RequestParser: request boundaries, Content-Length framing,
// fragmentation, pipelining and framing errors. No sockets are involved.
// The parser is fed bytes exactly as recv() might deliver them.

#include <string>
#include <vector>

#include "calc/request_parser.hpp"
#include "test_framework.hpp"

namespace {

using calc::HttpVersion;
using calc::ParseResult;
using calc::Request;
using calc::RequestParser;
using Kind = ParseResult::Kind;
using namespace std::string_literals;

const std::string kAdd = "GET /add?a=2&b=3 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n";
const std::string kSub = "GET /sub?a=10&b=4 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n";
const std::string kMul = "GET /mul?a=6&b=7 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n";
const std::string kPost = "POST /add HTTP/1.1\r\nHost: localhost:8080\r\nContent-Length: 7\r\n\r\na=2&b=3";

// Pulls every complete request currently available out of the parser.
std::vector<Request> drain(RequestParser& parser) {
    std::vector<Request> requests;
    while (true) {
        ParseResult result = parser.next();
        if (result.kind == Kind::Incomplete) return requests;
        REQUIRE(result.kind == Kind::Complete);
        requests.push_back(std::move(result.request));
    }
}

ParseResult parse_one(const std::string& bytes, calc::Limits limits = {}) {
    RequestParser parser(limits);
    parser.feed(bytes);
    return parser.next();
}

int error_status(const std::string& bytes, calc::Limits limits = {}) {
    const ParseResult result = parse_one(bytes, limits);
    return result.kind == Kind::Error ? result.error.status : 0;
}

void parses_a_single_request() {
    RequestParser parser;
    parser.feed(kAdd);
    ParseResult result = parser.next();
    REQUIRE(result.kind == Kind::Complete);
    const Request& r = result.request;
    CHECK_EQ(r.method, "GET");
    CHECK_EQ(r.target, "/add?a=2&b=3");
    CHECK(r.version == HttpVersion::Http11);
    CHECK_EQ(r.path(), "/add");
    CHECK_EQ(r.query(), "a=2&b=3");
    REQUIRE(r.header("host") != nullptr);  // header names are case-insensitive
    CHECK_EQ(*r.header("host"), "localhost:8080");
    CHECK_EQ(r.body, "");
    CHECK_EQ(r.stream_begin, 0u);
    CHECK_EQ(r.stream_end, kAdd.size());
    CHECK_EQ(r.head_bytes, kAdd.size());
    CHECK(parser.next().kind == Kind::Incomplete);
    CHECK_EQ(parser.buffered_bytes(), 0u);
    CHECK(!parser.has_partial_request());
}

void waits_for_the_blank_line() {
    RequestParser parser;
    parser.feed(kAdd.substr(0, kAdd.size() - 2));  // everything except the final CRLF
    CHECK(parser.next().kind == Kind::Incomplete);
    CHECK(parser.has_partial_request());
    parser.feed("\r\n");
    CHECK(parser.next().kind == Kind::Complete);
}

void three_pipelined_requests_in_one_read() {
    RequestParser parser;
    parser.feed(kAdd + kSub + kMul);  // what one recv() can return
    const std::vector<Request> requests = drain(parser);
    REQUIRE(requests.size() == 3);
    CHECK_EQ(requests[0].target, "/add?a=2&b=3");
    CHECK_EQ(requests[1].target, "/sub?a=10&b=4");
    CHECK_EQ(requests[2].target, "/mul?a=6&b=7");
    // Boundaries are contiguous: each request starts where the previous ended.
    CHECK_EQ(requests[0].stream_end, requests[1].stream_begin);
    CHECK_EQ(requests[1].stream_end, requests[2].stream_begin);
    CHECK_EQ(requests[2].stream_end, kAdd.size() + kSub.size() + kMul.size());
}

void split_at_every_possible_byte_boundary() {
    // Whatever point TCP splits the stream at, the same requests must come out.
    const std::string stream = kAdd + kPost + kMul;
    for (std::size_t split = 0; split <= stream.size(); ++split) {
        RequestParser parser;
        parser.feed(stream.substr(0, split));
        std::vector<Request> requests = drain(parser);
        parser.feed(stream.substr(split));
        for (Request& r : drain(parser)) requests.push_back(std::move(r));

        if (!CHECK_EQ(requests.size(), 3u)) {
            std::cout << "      (split at byte " << split << ")\n";
            continue;
        }
        CHECK_EQ(requests[0].target, "/add?a=2&b=3");
        CHECK_EQ(requests[1].method, "POST");
        CHECK_EQ(requests[1].body, "a=2&b=3");
        CHECK_EQ(requests[2].target, "/mul?a=6&b=7");
        CHECK_EQ(parser.buffered_bytes(), 0u);
    }
}

void one_byte_at_a_time() {
    // The slowest possible client: every recv() returns a single byte. Each
    // request must complete exactly when its last byte arrives.
    const std::string stream = kAdd + kPost + kMul;
    RequestParser parser;
    std::vector<Request> requests;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        parser.feed(stream.substr(i, 1));
        for (Request& r : drain(parser)) {
            CHECK_EQ(r.stream_end, i + 1);
            requests.push_back(std::move(r));
        }
    }
    REQUIRE(requests.size() == 3);
    CHECK_EQ(requests[1].body, "a=2&b=3");
}

void consumes_exactly_content_length_bytes() {
    RequestParser parser;
    const std::string head = "POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\n";
    parser.feed(head + "a=2&b=");  // 6 of the 7 body bytes
    CHECK(parser.next().kind == Kind::Incomplete);
    CHECK(parser.has_partial_request());

    // The 7th body byte arrives in the same read as the next request.
    parser.feed("3" + kMul);
    ParseResult first = parser.next();
    REQUIRE(first.kind == Kind::Complete);
    CHECK_EQ(first.request.body, "a=2&b=3");
    CHECK_EQ(first.request.stream_end, head.size() + 7);
    // Byte n+1 was not eaten: the next request is intact.
    CHECK_EQ(parser.buffered_bytes(), kMul.size());
    ParseResult second = parser.next();
    REQUIRE(second.kind == Kind::Complete);
    CHECK_EQ(second.request.target, "/mul?a=6&b=7");
}

void body_that_looks_like_a_request_stays_a_body() {
    // If the parser ignored Content-Length, the body would be misread as a
    // second GET request, and the client would get an extra response.
    const std::string post = "POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(kAdd.size()) +
                             "\r\n\r\n" + kAdd;
    RequestParser parser;
    parser.feed(post + kMul);
    const std::vector<Request> requests = drain(parser);
    REQUIRE(requests.size() == 2);
    CHECK_EQ(requests[0].method, "POST");
    CHECK_EQ(requests[0].body, kAdd);
    CHECK_EQ(requests[1].target, "/mul?a=6&b=7");
}

void zero_length_and_get_bodies() {
    RequestParser parser;
    parser.feed("POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n");
    parser.feed("GET /add?a=1&b=1 HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");
    parser.feed(kMul);
    const std::vector<Request> requests = drain(parser);
    REQUIRE(requests.size() == 3);
    CHECK_EQ(requests[0].body, "");
    CHECK_EQ(requests[1].body, "hello");  // a GET body is framed (and ignored) too
    CHECK_EQ(requests[2].target, "/mul?a=6&b=7");
}

void content_length_whitespace_and_leading_zeros() {
    RequestParser parser;
    parser.feed("POST /add HTTP/1.1\r\nHost: x\r\nContent-Length:   007  \r\n\r\nabcdefg");
    const ParseResult result = parser.next();
    REQUIRE(result.kind == Kind::Complete);
    CHECK_EQ(result.request.body, "abcdefg");
}

void accepts_bare_lf_line_endings() {
    // RFC 9112 §2.2 lets a recipient accept a bare LF, which is what a
    // request typed into `nc` without -c looks like.
    RequestParser parser;
    parser.feed("GET /add?a=1&b=2 HTTP/1.1\nHost: x\n\nGET /sub?a=1&b=2 HTTP/1.1\r\nHost: x\n\r\n");
    const std::vector<Request> requests = drain(parser);
    REQUIRE(requests.size() == 2);
    CHECK_EQ(requests[0].target, "/add?a=1&b=2");
    CHECK_EQ(*requests[0].header("Host"), "x");
    CHECK_EQ(requests[1].target, "/sub?a=1&b=2");
}

void ignores_empty_lines_between_requests() {
    RequestParser parser;
    parser.feed("\r\n\r\n" + kAdd + "\r\n" + kSub);
    const std::vector<Request> requests = drain(parser);
    REQUIRE(requests.size() == 2);
    CHECK_EQ(requests[0].stream_begin, 4u);
    CHECK_EQ(requests[1].stream_begin, 4u + kAdd.size() + 2);
}

void malformed_request_lines_are_400() {
    const std::vector<std::string> bad = {
        "GET\r\n\r\n",
        "GET /add\r\n\r\n",                         // HTTP/0.9 style, no version
        "GET  /add HTTP/1.1\r\n\r\n",               // two spaces
        "GET /add HTTP/1.1 extra\r\n\r\n",
        " GET /add HTTP/1.1\r\n\r\n",
        "GET add HTTP/1.1\r\n\r\n",                 // target must start with '/'
        "GET http://x/add HTTP/1.1\r\n\r\n",        // absolute-form not supported
        "GET * HTTP/1.1\r\n\r\n",
        "G(T /add HTTP/1.1\r\n\r\n",                // '(' is not a token character
        "GET /add http/1.1\r\n\r\n",                // version is case-sensitive
        "GET /add HTTP/1.1x\r\n\r\n",
        "GET /add HTTP/11\r\n\r\n",
        "GET /add HTTP/a.b\r\n\r\n",
        std::string("GET /a\x01" "dd HTTP/1.1\r\n\r\n"),  // control character in target
        "GET /add HTTP/1.1\r\r\n\r\n",              // stray CR
        "\x16\x03\x01\x02\x00\x01\x00\x01\xfc\r\n\r\n"s,  // TLS ClientHello-like bytes (NULs included)
    };
    for (const std::string& request : bad) {
        if (!CHECK_EQ(error_status(request), 400)) std::cout << "      (input: " << request << ")\n";
    }
}

void http_versions() {
    CHECK_EQ(error_status("GET /add HTTP/2.0\r\n\r\n"), 505);
    CHECK_EQ(error_status("GET /add HTTP/0.9\r\n\r\n"), 505);
    CHECK_EQ(error_status("GET /add HTTP/3.0\r\n\r\n"), 505);

    ParseResult http10 = parse_one("GET /add HTTP/1.0\r\n\r\n");
    REQUIRE(http10.kind == Kind::Complete);
    CHECK(http10.request.version == HttpVersion::Http10);

    // A higher minor version is handled as the highest one we implement.
    ParseResult http12 = parse_one("GET /add HTTP/1.2\r\nHost: x\r\n\r\n");
    REQUIRE(http12.kind == Kind::Complete);
    CHECK(http12.request.version == HttpVersion::Http11);
}

void malformed_headers_are_400() {
    const std::vector<std::string> bad_lines = {
        "Host x",           // no colon
        "Host : x",         // whitespace before the colon (RFC 9112 §5.1)
        " folded",          // obsolete line folding
        ": no-name",
        "Bad Name: x",
        std::string("X-Test: a\x01" "b"),  // control character in value
    };
    for (const std::string& line : bad_lines) {
        const std::string request = "GET /add HTTP/1.1\r\nHost: x\r\n" + line + "\r\n\r\n";
        if (!CHECK_EQ(error_status(request), 400)) std::cout << "      (header line: " << line << ")\n";
    }
}

void invalid_content_length_is_400() {
    const std::vector<std::string> bad = {"abc", "-1", "+7", "1.5", "7 7", "0x10", "7,7", ""};
    for (const std::string& value : bad) {
        const std::string request = "POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: " + value + "\r\n\r\n";
        if (!CHECK_EQ(error_status(request), 400)) std::cout << "      (Content-Length: '" << value << "')\n";
    }
    // Two Content-Length headers, even with equal values, are rejected:
    // disagreeing lengths are a classic request-smuggling vector.
    CHECK_EQ(error_status("POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx"), 400);
}

void oversized_body_is_413() {
    calc::Limits limits;
    const std::string too_big = std::to_string(limits.max_body_bytes + 1);
    CHECK_EQ(error_status("POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: " + too_big + "\r\n\r\n"), 413);
    // 26 digits: must not overflow into a small number.
    CHECK_EQ(error_status("POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: 99999999999999999999999999\r\n\r\n"), 413);
    // Exactly at the limit is fine: the parser waits for the body.
    const std::string at_limit = std::to_string(limits.max_body_bytes);
    CHECK(parse_one("POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: " + at_limit + "\r\n\r\n").kind ==
          Kind::Incomplete);
}

void transfer_encoding_is_rejected() {
    CHECK_EQ(error_status("POST /add HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"), 501);
    CHECK_EQ(error_status("POST /add HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 3\r\n\r\n"),
             400);
}

void oversized_head_is_431() {
    calc::Limits limits;
    // No blank line yet, but already more bytes than any head may have: give up now.
    CHECK_EQ(error_status("GET /" + std::string(limits.max_header_bytes, 'a'), limits), 431);

    // A complete head one byte over the limit.
    const std::string prefix = "GET /add HTTP/1.1\r\nHost: x\r\nX-Pad: ";
    const std::string suffix = "\r\n\r\n";
    const std::string exact = prefix + std::string(limits.max_header_bytes - prefix.size() - suffix.size(), 'p') + suffix;
    CHECK_EQ(exact.size(), limits.max_header_bytes);
    CHECK(parse_one(exact, limits).kind == Kind::Complete);
    const std::string over = prefix + std::string(limits.max_header_bytes - prefix.size() - suffix.size() + 1, 'p') + suffix;
    CHECK_EQ(error_status(over, limits), 431);

    // Too many header lines.
    std::string many = "GET /add HTTP/1.1\r\n";
    for (std::size_t i = 0; i <= limits.max_header_count; ++i) many += "X-" + std::to_string(i) + ": v\r\n";
    CHECK_EQ(error_status(many + "\r\n", limits), 431);
}

void errors_are_sticky() {
    RequestParser parser;
    parser.feed("NOT A VALID REQUEST LINE\r\n\r\n");
    CHECK(parser.next().kind == Kind::Error);
    parser.feed(kAdd);  // once framing is lost, later bytes cannot be trusted
    const ParseResult again = parser.next();
    CHECK(again.kind == Kind::Error);
    CHECK_EQ(again.error.status, 400);
}

void duplicate_headers_are_preserved() {
    const ParseResult result = parse_one("GET /add HTTP/1.1\r\nHost: a\r\nhost: b\r\n\r\n");
    REQUIRE(result.kind == Kind::Complete);
    CHECK_EQ(result.request.header_count("HOST"), 2u);  // so the router can reject them
}

void keep_alive_rules() {
    const auto keep_alive = [](const std::string& version, const std::string& connection_header) {
        std::string raw = "GET /add " + version + "\r\nHost: x\r\n";
        if (!connection_header.empty()) raw += "Connection: " + connection_header + "\r\n";
        const ParseResult result = parse_one(raw + "\r\n");
        REQUIRE(result.kind == Kind::Complete);
        return result.request.wants_keep_alive();
    };
    CHECK(keep_alive("HTTP/1.1", ""));                    // HTTP/1.1: persistent by default
    CHECK(!keep_alive("HTTP/1.1", "close"));
    CHECK(!keep_alive("HTTP/1.1", "Upgrade, CLOSE"));     // option lists, case-insensitive
    CHECK(keep_alive("HTTP/1.1", "keep-alive"));
    CHECK(!keep_alive("HTTP/1.0", ""));                   // HTTP/1.0: closes by default
    CHECK(keep_alive("HTTP/1.0", "Keep-Alive"));
    CHECK(!keep_alive("HTTP/1.0", "keep-alive, close"));  // close wins
}

}  // namespace

std::vector<test::TestCase> parser_tests() {
    return {
        {"parser: parses a single request", parses_a_single_request},
        {"parser: waits for the blank line", waits_for_the_blank_line},
        {"parser: three pipelined requests in one read", three_pipelined_requests_in_one_read},
        {"parser: split at every possible byte boundary", split_at_every_possible_byte_boundary},
        {"parser: one byte at a time", one_byte_at_a_time},
        {"parser: consumes exactly Content-Length bytes", consumes_exactly_content_length_bytes},
        {"parser: body that looks like a request stays a body", body_that_looks_like_a_request_stays_a_body},
        {"parser: zero-length and GET bodies", zero_length_and_get_bodies},
        {"parser: Content-Length whitespace and leading zeros", content_length_whitespace_and_leading_zeros},
        {"parser: accepts bare LF line endings", accepts_bare_lf_line_endings},
        {"parser: ignores empty lines between requests", ignores_empty_lines_between_requests},
        {"parser: malformed request lines are 400", malformed_request_lines_are_400},
        {"parser: HTTP versions (1.0/1.1 ok, others 505)", http_versions},
        {"parser: malformed headers are 400", malformed_headers_are_400},
        {"parser: invalid Content-Length is 400", invalid_content_length_is_400},
        {"parser: oversized body is 413", oversized_body_is_413},
        {"parser: Transfer-Encoding is rejected", transfer_encoding_is_rejected},
        {"parser: oversized head is 431", oversized_head_is_431},
        {"parser: errors are sticky", errors_are_sticky},
        {"parser: duplicate headers are preserved", duplicate_headers_are_preserved},
        {"parser: keep-alive rules", keep_alive_rules},
    };
}
