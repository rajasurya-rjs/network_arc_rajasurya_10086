#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace calc {

// Hard caps on how much a single client can make the server buffer. Every
// buffer that grows with client input is bounded by one of these values, so a
// broken or malicious client cannot grow memory without limit.
struct Limits {
    // Request line + header lines + the terminating blank line. The largest
    // legitimate request here is ~100 bytes; 8 KiB matches common server
    // defaults (nginx large_client_header_buffers, Apache LimitRequestLine).
    std::size_t max_header_bytes = 8 * 1024;

    // Same default as Apache's LimitRequestFields.
    std::size_t max_header_count = 100;

    // No calculator endpoint uses a body, but a body still has to be read in
    // full (and thrown away) to find where the next request starts. The cap
    // keeps that read bounded.
    std::size_t max_body_bytes = 64 * 1024;

    // Largest complete request the parser accepts. A connection never
    // buffers more unparsed input than this (see Connection::wants_to_read).
    std::size_t max_request_bytes() const { return max_header_bytes + max_body_bytes; }
};

enum class LogLevel {
    Notice,  // startup/shutdown only (--quiet)
    Info,    // + one line per connection event and per request (default)
    Debug,   // + every recv()/send() with byte counts (--verbose)
};

struct ServerConfig {
    std::string host = "127.0.0.1";
    std::uint16_t port = 8080;  // 0 = let the kernel pick a free port

    // Keep-alive idle timeout: how long a connection may sit without making
    // progress (no bytes received and none sent) before the server closes it.
    // Long enough for someone typing a request into nc/telnet by hand, short
    // enough that dead clients do not hold a connection slot indefinitely.
    std::chrono::milliseconds idle_timeout{30'000};

    // After half-closing (sending FIN following the final response), how long
    // to keep reading and discarding input while waiting for the client's
    // FIN. See Connection::begin_lingering_close.
    std::chrono::milliseconds linger_timeout{2'000};

    // Stop reading from a client once this many discarded bytes arrive during
    // the lingering close.
    std::size_t max_linger_bytes = 256 * 1024;

    // Bounds file descriptors and total buffer memory:
    // 64 * (72 KiB input + 64 KiB output) is roughly 9 MiB in the worst case.
    std::size_t max_connections = 64;

    // Backpressure threshold. When this many response bytes are waiting for a
    // client that pipelines requests but does not read responses, stop
    // parsing and reading until the backlog drains.
    std::size_t output_high_water = 64 * 1024;

    Limits limits;
    LogLevel log_level = LogLevel::Info;
};

}  // namespace calc
