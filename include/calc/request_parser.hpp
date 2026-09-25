#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "calc/config.hpp"
#include "calc/request.hpp"

namespace calc {

// A framing error: the byte stream cannot be split into requests reliably
// from this point on, so the connection must be answered with `status` and
// then closed.
struct ParseError {
    int status = 400;
    std::string message;
};

struct ParseResult {
    enum class Kind {
        Incomplete,  // no complete request buffered yet: read more from the socket
        Complete,    // `request` is one complete request, removed from the buffer
        Error,       // `error` describes the framing failure; parser is now stuck
    };
    Kind kind = Kind::Incomplete;
    Request request;
    ParseError error;
};

// Turns the TCP byte stream of one connection back into a sequence of
// requests.
//
// TCP delivers bytes, not messages. A single recv() can return half a
// request, exactly one request, or three pipelined requests plus the first
// few bytes of a fourth. The parser therefore never assumes anything about
// how bytes were grouped into reads:
//
//   * feed() appends whatever recv() returned to an internal buffer.
//   * next() takes at most one complete request off the front of that
//     buffer and leaves every byte after it untouched. Those bytes are the
//     start of the next request.
//
// A request is complete once its head (request line, headers, blank line)
// and then exactly Content-Length body bytes are buffered. Byte
// Content-Length + 1 is never read as body; it belongs to the next request.
//
// The parser is incremental. If next() returns Incomplete in the middle of a
// body, it remembers the parsed head and resumes from there once more bytes
// are fed.
class RequestParser {
public:
    explicit RequestParser(Limits limits = {});

    void feed(std::string_view bytes);
    ParseResult next();

    // Bytes received but not yet consumed as part of a returned request.
    std::size_t buffered_bytes() const { return buffer_.size() - start_; }

    // True if the client has started a request that is not finished yet.
    bool has_partial_request() const { return state_ == State::Body || buffered_bytes() > 0; }

private:
    enum class State { Head, Body, Failed };

    void skip_empty_lines_before_request();
    std::optional<std::size_t> find_end_of_head();
    std::optional<ParseError> parse_head(std::string_view head, Request& request,
                                         std::size_t& body_length) const;
    void consume(std::size_t count);
    ParseResult fail(int status, std::string message);

    Limits limits_;
    State state_ = State::Head;

    std::string buffer_;
    std::size_t start_ = 0;             // first unconsumed byte in buffer_
    std::size_t scan_from_ = 0;         // where the search for the blank line resumes
    std::uint64_t stream_offset_ = 0;   // stream position of buffer_[start_]

    Request pending_;                   // head parsed, waiting for its body
    std::size_t body_length_ = 0;
    ParseError error_;
};

}  // namespace calc
