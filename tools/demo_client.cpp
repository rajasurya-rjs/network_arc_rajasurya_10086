// demo_client: opens exactly ONE TCP connection to calc_server, sends the
// assignment's representative requests over it, prints each response, and
// then shows that the socket is still open.
//
// It uses raw POSIX sockets, like the server, and frames responses by their
// Content-Length. That is the client-side mirror of how the server frames
// requests.
//
//   demo_client [--host H] [--port P] [--pipeline] [--show-bytes]
//
// Exit status 0 means every response matched and the connection survived.

#include <netdb.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::string host = "127.0.0.1";
    std::string port = "8080";
    bool pipeline = false;
    bool show_bytes = false;
};

struct Exchange {
    std::string label;
    std::string request;  // exact bytes put on the wire
    int expected_status;
    std::string expected_body;  // empty = don't check the body
};

struct HttpResponse {
    int status = 0;
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string raw;  // exact bytes of this response

    std::string header(std::string_view name) const {
        for (const auto& [key, value] : headers) {
            if (key.size() == name.size() && strncasecmp(key.c_str(), name.data(), name.size()) == 0) return value;
        }
        return "";
    }
};

std::string socket_endpoint(int fd, bool peer) {
    sockaddr_storage address{};
    socklen_t length = sizeof address;
    auto* raw = reinterpret_cast<sockaddr*>(&address);
    if ((peer ? ::getpeername(fd, raw, &length) : ::getsockname(fd, raw, &length)) != 0) return "?";
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];
    if (::getnameinfo(raw, length, host, sizeof host, service, sizeof service, NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "?";
    }
    return std::string(host) + ":" + service;
}

// Shows CR and LF explicitly so message boundaries are visible.
std::string escape(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) {
        if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

int connect_once(const Options& options) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* candidates = nullptr;
    if (const int rc = ::getaddrinfo(options.host.c_str(), options.port.c_str(), &hints, &candidates); rc != 0) {
        throw std::runtime_error("resolve " + options.host + ": " + ::gai_strerror(rc));
    }
    int fd = -1;
    std::string error = "no address";
    for (const addrinfo* candidate = candidates; candidate != nullptr; candidate = candidate->ai_next) {
        fd = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, candidate->ai_addr, candidate->ai_addrlen) == 0) break;
        error = std::strerror(errno);
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(candidates);
    if (fd < 0) throw std::runtime_error("connect to " + options.host + ":" + options.port + " failed: " + error);
    return fd;
}

// send() may accept only part of the buffer, so loop until all of it is written.
void send_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t sent = ::send(fd, data.data(), data.size(), 0);
        if (sent < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
}

// Reads responses off the byte stream. Like the server's request parser, it
// never assumes one recv() returns one message. Bytes after the current
// response, such as the start of the next pipelined response, stay buffered.
class ResponseReader {
public:
    explicit ResponseReader(int fd) : fd_(fd) {}

    HttpResponse read_one() {
        std::size_t head_end;
        while ((head_end = buffer_.find("\r\n\r\n")) == std::string::npos) fill();
        head_end += 4;

        HttpResponse response;
        std::string_view head(buffer_.data(), head_end - 2);  // keep the last header's CRLF
        const std::size_t status_line_end = head.find("\r\n");
        const std::string_view status_line = head.substr(0, status_line_end);
        // "HTTP/1.1 200 OK"
        const std::size_t sp1 = status_line.find(' ');
        const std::size_t sp2 = status_line.find(' ', sp1 + 1);
        if (sp1 == std::string_view::npos || status_line.substr(0, 5) != "HTTP/") {
            throw std::runtime_error("malformed status line: " + std::string(status_line));
        }
        response.status = std::stoi(std::string(status_line.substr(sp1 + 1, sp2 - sp1 - 1)));
        response.reason = sp2 == std::string_view::npos ? "" : std::string(status_line.substr(sp2 + 1));

        std::size_t pos = status_line_end + 2;
        while (pos < head.size()) {
            const std::size_t line_end = head.find("\r\n", pos);
            const std::string_view line = head.substr(pos, line_end - pos);
            const std::size_t colon = line.find(':');
            std::string value(line.substr(colon + 1));
            value.erase(0, value.find_first_not_of(' '));
            response.headers.emplace_back(std::string(line.substr(0, colon)), value);
            pos = line_end + 2;
        }

        // Content-Length says exactly how many body bytes follow the head.
        // The server does not close the connection to mark the end of a
        // response, so this is the only way to know where it ends.
        const std::string length_text = response.header("Content-Length");
        if (length_text.empty()) throw std::runtime_error("response has no Content-Length");
        const std::size_t body_length = std::stoul(length_text);
        while (buffer_.size() < head_end + body_length) fill();

        response.body = buffer_.substr(head_end, body_length);
        response.raw = buffer_.substr(0, head_end + body_length);
        buffer_.erase(0, head_end + body_length);
        return response;
    }

private:
    void fill() {
        char chunk[4096];
        ssize_t received;
        do {
            received = ::recv(fd_, chunk, sizeof chunk, 0);
        } while (received < 0 && errno == EINTR);
        if (received == 0) throw std::runtime_error("server closed the connection unexpectedly");
        if (received < 0) throw std::runtime_error(std::string("recv failed: ") + std::strerror(errno));
        buffer_.append(chunk, static_cast<std::size_t>(received));
    }

    int fd_;
    std::string buffer_;
};

// Checks, without blocking, whether the server has closed or reset the
// connection. poll() with a zero timeout reports POLLIN if a FIN, an RST, or
// data is pending. MSG_PEEK then tells them apart without consuming anything.
bool socket_is_open(int fd, std::string& detail) {
    pollfd p{fd, POLLIN, 0};
    const int ready = ::poll(&p, 1, 0);
    if (ready == 0) {
        detail = "poll(): no FIN, RST or data pending";
        return true;
    }
    char byte;
    const ssize_t peeked = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    if (peeked > 0) {
        detail = "unexpected unsolicited data pending, but the connection is open";
        return true;
    }
    if (peeked < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        detail = "no FIN or RST pending";
        return true;
    }
    detail = peeked == 0 ? "server sent FIN (closed)" : std::string("socket error: ") + std::strerror(errno);
    return false;
}

std::vector<Exchange> build_exchanges(const std::string& host_header) {
    const std::string host = "Host: " + host_header + "\r\n";
    return {
        {"GET /add?a=2&b=3", "GET /add?a=2&b=3 HTTP/1.1\r\n" + host + "\r\n", 200, "5\n"},
        {"GET /sub?a=10&b=4", "GET /sub?a=10&b=4 HTTP/1.1\r\n" + host + "\r\n", 200, "6\n"},
        {"GET /mul?a=6&b=7", "GET /mul?a=6&b=7 HTTP/1.1\r\n" + host + "\r\n", 200, "42\n"},
        {"GET /div?a=1&b=0", "GET /div?a=1&b=0 HTTP/1.1\r\n" + host + "\r\n", 400, ""},
        {"GET /pow?a=2&b=8", "GET /pow?a=2&b=8 HTTP/1.1\r\n" + host + "\r\n", 404, ""},
        // The body must be consumed exactly (7 bytes), or its bytes would be
        // misread as the start of request #7.
        {"POST /add (7-byte body)",
         "POST /add HTTP/1.1\r\n" + host + "Content-Length: 7\r\n\r\na=2&b=3", 405, ""},
        {"GET /add (no Host header)", "GET /add?a=2&b=3 HTTP/1.1\r\n\r\n", 400, ""},
    };
}

// Fixed-width table cell that always keeps at least one space before the next column.
std::string pad(std::string text, std::size_t width) {
    if (text.size() >= width) text = text.substr(0, width - 4) + "...";
    text.resize(width, ' ');
    return text;
}

std::string trim_newline(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

bool report(std::size_t index, const Exchange& exchange, const HttpResponse& response, bool show_bytes) {
    const bool pass = response.status == exchange.expected_status &&
                      (exchange.expected_body.empty() || response.body == exchange.expected_body);
    std::cout << ' ' << pad(std::to_string(index), 3) << pad(exchange.label, 27)
              << pad(std::to_string(exchange.expected_status), 8) << pad(std::to_string(response.status), 5)
              << pad(trim_newline(response.body), 36) << pad(response.header("X-Connection-Id"), 9)
              << pad(response.header("X-Request-Number"), 6) << (pass ? "PASS" : "FAIL") << '\n';
    if (show_bytes) {
        std::cout << "      >>> \"" << escape(exchange.request) << "\"\n"
                  << "      <<< \"" << escape(response.raw) << "\"\n";
    }
    return pass;
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGPIPE, SIG_IGN);
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) options.host = argv[++i];
        else if (arg == "--port" && i + 1 < argc) options.port = argv[++i];
        else if (arg == "--pipeline") options.pipeline = true;
        else if (arg == "--show-bytes") options.show_bytes = true;
        else {
            std::cerr << "usage: demo_client [--host H] [--port P] [--pipeline] [--show-bytes]\n";
            return 2;
        }
    }

    try {
        const int fd = connect_once(options);
        const std::string local_before = socket_endpoint(fd, false);
        std::cout << "connect() called exactly once: local " << local_before << " -> server "
                  << socket_endpoint(fd, true) << " (socket fd " << fd << ")\n";

        const std::vector<Exchange> exchanges = build_exchanges(options.host + ":" + options.port);
        ResponseReader reader(fd);
        std::vector<HttpResponse> responses;
        bool all_pass = true;

        std::cout << (options.pipeline
                          ? "mode: PIPELINED - all requests written with one send_all(), then all responses read\n\n"
                          : "mode: sequential - send a request, read its response, send the next\n\n");
        std::cout << ' ' << pad("#", 3) << pad("request", 27) << pad("expect", 8) << pad("got", 5)
                  << pad("response body", 36) << pad("conn-id", 9) << pad("req#", 6) << "result\n";

        if (options.pipeline) {
            std::string batch;
            for (const Exchange& exchange : exchanges) batch += exchange.request;
            send_all(fd, batch);
            for (std::size_t i = 0; i < exchanges.size(); ++i) responses.push_back(reader.read_one());
        }
        for (std::size_t i = 0; i < exchanges.size(); ++i) {
            if (!options.pipeline) {
                send_all(fd, exchanges[i].request);
                responses.push_back(reader.read_one());
            }
            all_pass = report(i + 1, exchanges[i], responses[i], options.show_bytes) && all_pass;
        }

        // Evidence that the same TCP connection carried every request.
        const std::string connection_id = responses.front().header("X-Connection-Id");
        bool same_connection = true;
        for (std::size_t i = 0; i < responses.size(); ++i) {
            same_connection = same_connection && responses[i].header("X-Connection-Id") == connection_id &&
                              responses[i].header("X-Request-Number") == std::to_string(i + 1);
        }
        const std::string local_after = socket_endpoint(fd, false);
        std::string open_detail;
        const bool open = socket_is_open(fd, open_detail);

        std::cout << "\nsame connection?\n"
                  << "  client side: same socket fd " << fd << ", local endpoint " << local_before << " before and "
                  << local_after << " after" << (local_before == local_after ? " (unchanged)" : " (CHANGED!)") << '\n'
                  << "  server side: every response has X-Connection-Id " << connection_id
                  << " and X-Request-Number counts 1.." << responses.size()
                  << (same_connection ? " in order" : " -- MISMATCH") << '\n'
                  << "socket still open after " << exchanges.size() << " requests? " << (open ? "YES" : "NO") << " ("
                  << open_detail << ")\n";

        // Final proof: the connection still works after all those errors.
        bool probe_ok = false;
        if (open) {
            send_all(fd, "GET /div?a=84&b=2 HTTP/1.1\r\nHost: " + options.host + ":" + options.port + "\r\n\r\n");
            const HttpResponse probe = reader.read_one();
            probe_ok = probe.status == 200 && probe.body == "42\n" && probe.header("X-Connection-Id") == connection_id;
            std::cout << "liveness probe on the same socket: GET /div?a=84&b=2 -> " << probe.status << ' '
                      << trim_newline(probe.body) << " (X-Connection-Id " << probe.header("X-Connection-Id")
                      << ", X-Request-Number " << probe.header("X-Request-Number") << ")\n";
        }

        const bool pass = all_pass && same_connection && local_before == local_after && open && probe_ok;
        std::cout << "\nRESULT: " << (pass ? "PASS" : "FAIL") << " - " << exchanges.size() + (probe_ok ? 1 : 0)
                  << " requests, " << responses.size() + (probe_ok ? 1 : 0)
                  << " responses, 1 TCP connection, socket " << (open ? "still open" : "closed") << "\n";
        ::close(fd);
        return pass ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "demo_client: " << e.what() << '\n';
        return 1;
    }
}
