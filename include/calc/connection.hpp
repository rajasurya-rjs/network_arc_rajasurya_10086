#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "calc/config.hpp"
#include "calc/file_descriptor.hpp"
#include "calc/logger.hpp"
#include "calc/request.hpp"
#include "calc/request_parser.hpp"
#include "calc/response.hpp"

namespace calc {

using Clock = std::chrono::steady_clock;
using RequestHandler = std::function<Response(const Request&)>;

// Counters that make the TCP-level behaviour visible in the log and in
// tests. For example, "3 requests from 1 recv()" is pipelining, and
// "short_sends > 0" means the kernel accepted only part of a write.
struct ConnectionStats {
    std::uint64_t requests = 0;        // responses queued (one per request)
    std::uint64_t bytes_received = 0;
    std::uint64_t recv_calls = 0;      // recv() calls that returned data
    std::uint64_t bytes_sent = 0;
    std::uint64_t send_calls = 0;      // send() calls that accepted data
    std::uint64_t short_sends = 0;     // send() accepted fewer bytes than offered
    std::uint64_t blocked_sends = 0;   // send() returned EAGAIN: kernel send buffer full
};

// One accepted TCP connection, carrying any number of requests.
//
// The socket is non-blocking and driven by the server's poll() loop. That
// loop calls poll_events() before poll(), then on_poll_events() with
// whatever poll() reported, and on_deadline() once deadline() has passed.
//
// Lifecycle:
//
//   Active   --(response with "Connection: close", or framing error)-->  Closing
//   Closing  --(final response fully written; shutdown(SHUT_WR))------>  Draining
//   Draining --(client's FIN, linger timeout, or discard limit)------->  Closed
//   any      --(socket error, peer reset, idle timeout)--------------->  Closed
//
// Active is the persistent-connection state. After each response the
// connection simply goes back to reading the next request from the same
// socket.
class Connection {
public:
    Connection(FileDescriptor socket, std::uint64_t id, std::string peer, const ServerConfig& config,
               RequestHandler handler, const Logger& logger, Clock::time_point now);

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    int fd() const { return socket_.get(); }
    std::uint64_t id() const { return id_; }
    bool closed() const { return state_ == State::Closed; }
    const ConnectionStats& stats() const { return stats_; }
    std::size_t pending_output_bytes() const { return output_.size() - output_sent_; }

    // Which readiness events poll() should report for this socket right now.
    short poll_events() const;

    void on_poll_events(short revents, Clock::time_point now);

    // When this connection next needs attention even without I/O.
    Clock::time_point deadline() const;
    void on_deadline(Clock::time_point now);

    // Immediate close, used when the server shuts down.
    void close(std::string_view reason) { close_now(reason); }

private:
    enum class State { Active, Closing, Draining, Closed };

    bool wants_to_read() const;
    void read_from_socket(Clock::time_point now);
    void advance(Clock::time_point now);
    bool respond_to_buffered_requests();
    Response run_handler(const Request& request);
    void queue_response(Response response);
    bool flush_output(Clock::time_point now);
    void begin_lingering_close(Clock::time_point now);
    void discard_lingering_input();
    void close_now(std::string_view reason);
    std::string describe_stats() const;

    FileDescriptor socket_;
    std::uint64_t id_;
    std::string peer_;
    const ServerConfig& config_;
    RequestHandler handler_;
    const Logger& logger_;

    RequestParser parser_;
    std::string output_;           // serialized responses not yet accepted by the kernel
    std::size_t output_sent_ = 0;  // prefix of output_ already sent

    State state_ = State::Active;
    bool peer_finished_sending_ = false;  // recv() returned 0: the client sent FIN
    Clock::time_point last_activity_;
    Clock::time_point linger_deadline_;
    std::size_t lingered_bytes_ = 0;
    ConnectionStats stats_;
};

}  // namespace calc
