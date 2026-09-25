#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "calc/request.hpp"

namespace calc {

struct Response {
    int status = 200;
    std::string body;
    std::vector<Header> headers;  // additional headers, e.g. Allow on a 405
    bool close_connection = false;

    // Set for a response to HEAD. RFC 9112 §6.3: such a response ends at the
    // blank line whatever Content-Length says, because the client will not
    // read a body. Sending one would make the client misread those bytes as
    // the start of the next response. Content-Length still reports the size
    // the body would have had.
    bool omit_body = false;
};

// A text/plain response whose body is `text` followed by a newline. The
// trailing newline keeps output readable when a response is printed by nc.
Response text_response(int status, std::string_view text);

std::string_view reason_phrase(int status);

// Serializes a response into its exact wire bytes:
//
//   HTTP/1.1 200 OK\r\n
//   Content-Length: 2\r\n       <- exact size of the body in bytes
//   Content-Type: text/plain\r\n
//   Connection: keep-alive\r\n  <- or "close" when this is the last response
//   ...extra headers...\r\n
//   \r\n
//   5\n                          <- body
//
// Content-Length is what lets the client find where this response ends
// without the server closing the connection. That is the difference
// between length-delimited and connection-delimited messages.
std::string serialize(const Response& response);

}  // namespace calc
