#include "calc/request_parser.hpp"

#include <algorithm>
#include <utility>
#include <vector>

#include "calc/text.hpp"

namespace calc {

namespace {

// RFC 9110 §5.6.2 "tchar": the characters allowed in methods and header names.
bool is_token_char(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '*': case '+':
        case '-': case '.': case '^': case '_': case '`': case '|': case '~':
            return true;
        default:
            return false;
    }
}

bool is_token(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), is_token_char);
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// request-line = method SP request-target SP HTTP-version   (RFC 9112 §3)
std::optional<ParseError> parse_request_line(std::string_view line, Request& request) {
    const std::size_t first_space = line.find(' ');
    const std::size_t second_space =
        first_space == std::string_view::npos ? std::string_view::npos : line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos ||
        line.find(' ', second_space + 1) != std::string_view::npos) {
        return ParseError{400, "malformed request line '" + excerpt(line) +
                                   "': expected exactly 'METHOD /target HTTP/1.1'"};
    }

    const std::string_view method = line.substr(0, first_space);
    const std::string_view target = line.substr(first_space + 1, second_space - first_space - 1);
    const std::string_view version = line.substr(second_space + 1);

    if (!is_token(method)) {
        return ParseError{400, "malformed request line: invalid method '" + excerpt(method) + "'"};
    }

    // Only origin-form ("/path?query") is supported: that is what clients
    // send to an origin server. Absolute-form and "*" are rejected.
    if (target.empty() || target.front() != '/') {
        return ParseError{400, "malformed request line: target must start with '/', got '" +
                                   excerpt(target) + "'"};
    }
    for (const char c : target) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7F) {
            return ParseError{400, "malformed request line: invalid character in request target"};
        }
    }

    // HTTP-version = "HTTP/" DIGIT "." DIGIT
    if (version.size() != 8 || version.substr(0, 5) != "HTTP/" || !is_digit(version[5]) ||
        version[6] != '.' || !is_digit(version[7])) {
        return ParseError{400, "malformed request line: invalid HTTP version '" + excerpt(version) + "'"};
    }
    if (version[5] != '1') {
        return ParseError{505, "HTTP version " + std::string(version) +
                                   " is not supported; use HTTP/1.1 (or HTTP/1.0)"};
    }
    // A higher 1.x minor version is handled as 1.1 (RFC 9110 §2.5).
    request.version = version[7] == '0' ? HttpVersion::Http10 : HttpVersion::Http11;
    request.method = std::string(method);
    request.target = std::string(target);
    return std::nullopt;
}

// field-line = field-name ":" OWS field-value OWS   (RFC 9112 §5)
std::optional<ParseError> parse_header_line(std::string_view line, std::vector<Header>& headers) {
    if (line.front() == ' ' || line.front() == '\t') {
        return ParseError{400, "obsolete header line folding is not supported"};
    }
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) {
        return ParseError{400, "malformed header line '" + excerpt(line) + "': missing ':'"};
    }
    // A space before the colon is deliberately not trimmed; RFC 9112 §5.1
    // requires rejecting it. Different parsers disagree on what such a name
    // means, and that disagreement is a known request-smuggling vector.
    const std::string_view name = line.substr(0, colon);
    if (!is_token(name)) {
        return ParseError{400, "malformed header line: invalid header name '" + excerpt(name) + "'"};
    }
    const std::string_view value = trim_whitespace(line.substr(colon + 1));
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte < 0x20 && c != '\t') || byte == 0x7F) {
            return ParseError{400, "malformed header line: control character in value of '" +
                                       excerpt(name) + "'"};
        }
    }
    headers.push_back(Header{std::string(name), std::string(value)});
    return std::nullopt;
}

// Works out where the body ends, which also tells us where the next request
// starts (RFC 9112 §6.3). This is the key framing decision. If it is wrong,
// every request after this one on the connection is misread, so anything
// ambiguous is rejected rather than guessed at.
std::optional<ParseError> determine_body_length(const std::vector<Header>& headers,
                                                std::size_t max_body_bytes, std::size_t& body_length) {
    std::size_t length_headers = 0;
    std::string_view length_text;
    bool has_transfer_encoding = false;
    for (const Header& h : headers) {
        if (iequals(h.name, "Content-Length")) {
            ++length_headers;
            length_text = h.value;
        } else if (iequals(h.name, "Transfer-Encoding")) {
            has_transfer_encoding = true;
        }
    }

    if (has_transfer_encoding && length_headers > 0) {
        return ParseError{400, "both Transfer-Encoding and Content-Length present: ambiguous message framing"};
    }
    if (has_transfer_encoding) {
        return ParseError{501, "Transfer-Encoding is not supported; send the body with Content-Length"};
    }
    if (length_headers > 1) {
        return ParseError{400, "multiple Content-Length headers: ambiguous message framing"};
    }

    body_length = 0;
    if (length_headers == 0) return std::nullopt;  // no body: the request ends at the blank line

    // Content-Length = 1*DIGIT. No sign, no whitespace inside, no list.
    if (length_text.empty()) return ParseError{400, "empty Content-Length"};
    std::size_t value = 0;
    bool too_large = false;
    for (const char c : length_text) {
        if (!is_digit(c)) {
            return ParseError{400, "invalid Content-Length '" + excerpt(length_text) +
                                       "': must be a non-negative decimal integer"};
        }
        // Stop accumulating once past the limit so a 30-digit value cannot overflow.
        if (!too_large) {
            value = value * 10 + static_cast<std::size_t>(c - '0');
            too_large = value > max_body_bytes;
        }
    }
    if (too_large) {
        return ParseError{413, "Content-Length exceeds the " + std::to_string(max_body_bytes) +
                                   "-byte body limit"};
    }
    body_length = value;
    return std::nullopt;
}

}  // namespace

RequestParser::RequestParser(Limits limits) : limits_(limits) {}

void RequestParser::feed(std::string_view bytes) {
    if (state_ == State::Failed) return;  // the stream is already unusable; don't buffer more

    // Drop the consumed prefix before growing the buffer. Only when it is at
    // least half the buffer, though: erasing after every request would move
    // the remaining bytes once per pipelined request, which is quadratic.
    if (start_ == buffer_.size()) {
        buffer_.clear();
        scan_from_ = start_ = 0;
    } else if (start_ > 0 && start_ >= buffer_.size() / 2) {
        buffer_.erase(0, start_);
        scan_from_ -= start_;
        start_ = 0;
    }
    buffer_.append(bytes.data(), bytes.size());
}

ParseResult RequestParser::next() {
    if (state_ == State::Failed) {
        ParseResult result;
        result.kind = ParseResult::Kind::Error;
        result.error = error_;
        return result;
    }

    if (state_ == State::Head) {
        skip_empty_lines_before_request();

        const std::optional<std::size_t> head_end = find_end_of_head();
        if (!head_end) {
            if (buffered_bytes() > limits_.max_header_bytes) {
                return fail(431, "request head exceeds " + std::to_string(limits_.max_header_bytes) + " bytes");
            }
            return {};  // Incomplete: the blank line has not arrived yet
        }

        const std::size_t head_size = *head_end - start_;
        if (head_size > limits_.max_header_bytes) {
            return fail(431, "request head exceeds " + std::to_string(limits_.max_header_bytes) + " bytes");
        }

        Request request;
        std::size_t body_length = 0;
        if (auto error = parse_head(std::string_view(buffer_).substr(start_, head_size), request, body_length)) {
            return fail(error->status, std::move(error->message));
        }
        request.stream_begin = stream_offset_;
        request.head_bytes = head_size;
        consume(head_size);

        pending_ = std::move(request);
        body_length_ = body_length;
        state_ = State::Body;
    }

    // State::Body. Wait until the whole body is buffered, then take exactly
    // body_length_ bytes. Whatever follows is the next request and stays put.
    if (buffered_bytes() < body_length_) return {};
    pending_.body.assign(buffer_, start_, body_length_);
    consume(body_length_);
    pending_.stream_end = stream_offset_;
    state_ = State::Head;

    ParseResult result;
    result.kind = ParseResult::Kind::Complete;
    result.request = std::move(pending_);
    pending_ = Request{};
    return result;
}

// RFC 9112 §2.2: a server SHOULD ignore at least one empty line received
// before a request line. Some clients send an extra CRLF after a body.
void RequestParser::skip_empty_lines_before_request() {
    while (start_ < buffer_.size()) {
        if (buffer_[start_] == '\n') {
            consume(1);
        } else if (buffer_[start_] == '\r' && start_ + 1 < buffer_.size() && buffer_[start_ + 1] == '\n') {
            consume(2);
        } else {
            return;
        }
    }
}

// The head ends at the first empty line, i.e. a line terminator followed
// immediately by another one: "\r\n\r\n". A bare "\n" is also accepted as a
// line terminator (RFC 9112 §2.2 allows this) so requests typed into nc
// work. That also covers "\n\n" and "\n\r\n".
//
// Returns the index one past the blank line, or nullopt if it is not
// buffered yet. The search resumes where the previous call stopped, so a
// client sending one byte per packet costs linear, not quadratic, work.
std::optional<std::size_t> RequestParser::find_end_of_head() {
    std::size_t pos = std::max(scan_from_, start_);
    while (true) {
        const std::size_t newline = buffer_.find('\n', pos);
        if (newline == std::string::npos) {
            scan_from_ = buffer_.size();
            return std::nullopt;
        }
        const std::size_t next_line = newline + 1;
        if (next_line < buffer_.size() && buffer_[next_line] == '\n') return next_line + 1;
        if (next_line + 1 < buffer_.size() && buffer_[next_line] == '\r' && buffer_[next_line + 1] == '\n') {
            return next_line + 2;
        }
        const bool undecided = next_line == buffer_.size() ||
                               (next_line + 1 == buffer_.size() && buffer_[next_line] == '\r');
        if (undecided) {
            scan_from_ = newline;  // re-examine this line ending when more bytes arrive
            return std::nullopt;
        }
        pos = next_line;
    }
}

std::optional<ParseError> RequestParser::parse_head(std::string_view head, Request& request,
                                                    std::size_t& body_length) const {
    std::vector<std::string_view> lines;
    std::size_t pos = 0;
    while (pos < head.size()) {
        const std::size_t newline = head.find('\n', pos);
        std::string_view line = head.substr(pos, newline - pos);
        pos = newline + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) break;  // the blank line that ends the head
        lines.push_back(line);
    }

    // Empty lines before the request line were skipped, so lines[0] is the
    // request line. A stray CR anywhere else is rejected by the character
    // checks in the line parsers.
    if (lines.empty()) return ParseError{400, "malformed request: missing request line"};
    if (lines.size() - 1 > limits_.max_header_count) {
        return ParseError{431, "too many header fields (limit " + std::to_string(limits_.max_header_count) + ")"};
    }
    if (auto error = parse_request_line(lines[0], request)) return error;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (auto error = parse_header_line(lines[i], request.headers)) return error;
    }
    return determine_body_length(request.headers, limits_.max_body_bytes, body_length);
}

void RequestParser::consume(std::size_t count) {
    start_ += count;
    stream_offset_ += count;
    scan_from_ = std::max(scan_from_, start_);
}

ParseResult RequestParser::fail(int status, std::string message) {
    state_ = State::Failed;
    error_ = ParseError{status, std::move(message)};
    buffer_.clear();
    buffer_.shrink_to_fit();
    start_ = scan_from_ = 0;
    return next();
}

}  // namespace calc
