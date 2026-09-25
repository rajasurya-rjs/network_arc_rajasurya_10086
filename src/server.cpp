#include "calc/server.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <system_error>
#include <utility>

#include "calc/socket_utils.hpp"

namespace calc {

namespace {

constexpr int kListenBacklog = 128;

// Upper bound on a single poll() wait. A signal normally interrupts poll()
// straight away (EINTR). One that lands just before poll() is entered would
// otherwise go unnoticed until the next event, so this caps shutdown
// latency at one second.
constexpr std::chrono::milliseconds kMaxPollWait{1000};

constexpr std::chrono::milliseconds kAcceptPause{1000};

}  // namespace

Server::Server(ServerConfig config, RequestHandler handler, const Logger& logger)
    : config_(std::move(config)),
      handler_(std::move(handler)),
      logger_(logger),
      listener_(create_listening_socket(config_.host, config_.port, kListenBacklog)) {}

std::uint16_t Server::port() const { return local_port(listener_.get()); }

void Server::run(const volatile std::sig_atomic_t& stop_requested) {
    logger_.notice("listening on ", local_address(listener_.get()), " (idle timeout ",
                   static_cast<double>(config_.idle_timeout.count()) / 1000.0, "s, max ", config_.max_connections,
                   " connections)");

    std::vector<pollfd> poll_set;
    while (!stop_requested) {
        Clock::time_point now = Clock::now();
        const bool accepting = connections_.size() < config_.max_connections && now >= accept_paused_until_;

        // Rebuilt every iteration, because the events a connection cares
        // about change as it reads, writes, and changes state.
        // poll_set[0] is the listener; poll_set[i + 1] is connections_[i].
        poll_set.clear();
        poll_set.push_back(pollfd{listener_.get(), static_cast<short>(accepting ? POLLIN : 0), 0});
        for (const auto& connection : connections_) {
            poll_set.push_back(pollfd{connection->fd(), connection->poll_events(), 0});
        }

        const int ready = ::poll(poll_set.data(), static_cast<nfds_t>(poll_set.size()), poll_timeout_ms(now));
        if (ready < 0) {
            if (errno == EINTR) continue;  // interrupted by a signal: loop back and re-check stop_requested
            throw std::system_error(errno, std::generic_category(), "poll");
        }

        now = Clock::now();
        // Existing connections first. accept() appends new ones afterwards,
        // so the indices into poll_set stay valid here.
        for (std::size_t i = 0; i < connections_.size(); ++i) {
            Connection& connection = *connections_[i];
            if (poll_set[i + 1].revents != 0) connection.on_poll_events(poll_set[i + 1].revents, now);
            if (!connection.closed() && now >= connection.deadline()) connection.on_deadline(now);
        }
        connections_.erase(std::remove_if(connections_.begin(), connections_.end(),
                                          [](const std::unique_ptr<Connection>& c) { return c->closed(); }),
                           connections_.end());

        if (poll_set[0].revents & POLLIN) accept_new_connections(now);
    }

    logger_.notice("shutdown requested; closing ", connections_.size(), " open connection(s)");
    for (auto& connection : connections_) connection->close("server shutting down");
    connections_.clear();
    listener_.reset();
}

void Server::accept_new_connections(Clock::time_point now) {
    // By this point the kernel has already completed the TCP three-way
    // handshake for every queued connection. accept() just hands them over,
    // one new socket each. Keep going until the queue is empty (EAGAIN) or
    // the connection limit is reached. Any excess waits in the listen
    // backlog.
    while (connections_.size() < config_.max_connections) {
        sockaddr_storage address{};
        socklen_t length = sizeof address;
        FileDescriptor socket(::accept(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length));
        if (!socket.valid()) {
            const int error = errno;
            if (error == EAGAIN || error == EWOULDBLOCK) return;
            // EINTR: interrupted, retry. ECONNABORTED / EPROTO: that client
            // reset while still queued. Skip it and take the next one.
            if (error == EINTR || error == ECONNABORTED || error == EPROTO) continue;
            // Something persistent, such as EMFILE (out of descriptors). The
            // listener stays readable, so retrying at once would spin. Pause
            // accepting briefly instead.
            logger_.notice("accept() failed: ", errno_message(error), "; pausing new connections for 1s");
            accept_paused_until_ = now + kAcceptPause;
            return;
        }

        // Accepted sockets inherit O_NONBLOCK on BSD/macOS but not on Linux.
        if (!set_nonblocking(socket.get())) {
            logger_.notice("fcntl(O_NONBLOCK) on accepted socket failed: ", errno_message(errno));
            continue;
        }
        set_tcp_nodelay(socket.get());

        const std::uint64_t id = next_connection_id_++;
        std::string peer = format_address(reinterpret_cast<const sockaddr*>(&address), length);
        logger_.info("[conn ", id, "] accepted connection from ", peer, " (", connections_.size() + 1, " open)");
        connections_.push_back(
            std::make_unique<Connection>(std::move(socket), id, std::move(peer), config_, handler_, logger_, now));
    }
}

int Server::poll_timeout_ms(Clock::time_point now) const {
    Clock::time_point wake = now + kMaxPollWait;
    for (const auto& connection : connections_) wake = std::min(wake, connection->deadline());
    if (accept_paused_until_ > now) wake = std::min(wake, accept_paused_until_);
    if (wake <= now) return 0;
    // Round up. Rounding down would wake just before the deadline and then
    // spin on zero-length timeouts until it actually passed.
    return static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(wake - now).count());
}

}  // namespace calc
