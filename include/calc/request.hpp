#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace calc {

enum class HttpVersion { Http10, Http11 };

struct Header {
    std::string name;
    std::string value;
};

// One fully framed request: everything from the first byte of the request
// line to the last byte of its Content-Length body.
struct Request {
    std::string method;
    std::string target;  // raw origin-form target, e.g. "/add?a=2&b=3"
    HttpVersion version = HttpVersion::Http11;
    std::vector<Header> headers;  // arrival order; duplicates are kept so they can be rejected
    std::string body;             // exactly Content-Length bytes

    // Where this request sat in the connection's byte stream, counted from
    // the first byte the client ever sent: [stream_begin, stream_end).
    // Logged so the boundaries between pipelined requests are visible.
    std::uint64_t stream_begin = 0;
    std::uint64_t stream_end = 0;
    std::size_t head_bytes = 0;  // request line + headers + blank line

    // First header with this name (names are case-insensitive), or nullptr.
    const std::string* header(std::string_view name) const;
    std::size_t header_count(std::string_view name) const;

    std::string_view path() const;   // target before '?'
    std::string_view query() const;  // target after '?', or empty

    // Persistent-connection rule (RFC 9112 §9.3): HTTP/1.1 connections stay
    // open unless the client sends "Connection: close". HTTP/1.0 connections
    // close unless the client sends "Connection: keep-alive".
    bool wants_keep_alive() const;
};

}  // namespace calc
