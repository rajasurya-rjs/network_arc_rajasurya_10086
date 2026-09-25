#pragma once

#include <csignal>
#include <cstdint>
#include <memory>
#include <vector>

#include "calc/config.hpp"
#include "calc/connection.hpp"
#include "calc/file_descriptor.hpp"
#include "calc/logger.hpp"

namespace calc {

// Owns the listening socket and every accepted connection, and runs the
// single-threaded poll() event loop that drives them.
//
// Why one thread and poll() rather than a thread per connection:
//   * Many clients are served at once, so one idle keep-alive connection
//     cannot block anyone else, and there are no threads, locks, or shared
//     mutable state.
//   * Sockets are non-blocking, so partial writes are real and handled
//     explicitly (a per-connection output buffer plus POLLOUT) rather than
//     hidden inside a blocking send().
class Server {
public:
    // Creates the listening socket (bind + listen). Throws on failure.
    Server(ServerConfig config, RequestHandler handler, const Logger& logger);

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    std::uint16_t port() const;

    // Runs until `stop_requested` becomes non-zero (set by the SIGINT or
    // SIGTERM handler), then closes every connection and the listener.
    void run(const volatile std::sig_atomic_t& stop_requested);

private:
    void accept_new_connections(Clock::time_point now);
    int poll_timeout_ms(Clock::time_point now) const;

    ServerConfig config_;  // Connections hold a reference to this, so Server is non-movable
    RequestHandler handler_;
    const Logger& logger_;
    FileDescriptor listener_;
    std::vector<std::unique_ptr<Connection>> connections_;
    std::uint64_t next_connection_id_ = 1;
    Clock::time_point accept_paused_until_{};
};

}  // namespace calc
