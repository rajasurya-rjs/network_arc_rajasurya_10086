#include "calc/connection.hpp"

#include <poll.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <exception>
#include <utility>

#include "calc/socket_utils.hpp"
#include "calc/text.hpp"

namespace calc {

namespace {

constexpr std::size_t kRecvChunkBytes = 16 * 1024;

// On Linux, MSG_NOSIGNAL makes send() to a reset connection return EPIPE
// instead of raising SIGPIPE, which would kill the process. macOS lacks the
// flag, so main() also ignores SIGPIPE for the whole process.
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

const char* version_name(HttpVersion version) {
    return version == HttpVersion::Http10 ? "HTTP/1.0" : "HTTP/1.1";
}

int pending_socket_error(int fd) {
    int error = 0;
    socklen_t length = sizeof error;
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
    return error;
}

}  // namespace

Connection::Connection(FileDescriptor socket, std::uint64_t id, std::string peer, const ServerConfig& config,
                       RequestHandler handler, const Logger& logger, Clock::time_point now)
    : socket_(std::move(socket)),
      id_(id),
      peer_(std::move(peer)),
      config_(config),
      handler_(std::move(handler)),
      logger_(logger),
      parser_(config.limits),
      last_activity_(now) {}

short Connection::poll_events() const {
    switch (state_) {
        case State::Active: {
            short events = 0;
            if (wants_to_read()) events |= POLLIN;
            if (pending_output_bytes() > 0) events |= POLLOUT;
            return events;
        }
        case State::Closing:
            return pending_output_bytes() > 0 ? POLLOUT : 0;
        case State::Draining:
            return POLLIN;
        case State::Closed:
            return 0;
    }
    return 0;
}

// Reading stops, and TCP flow control pushes back on the client, when:
//  * the client has already sent FIN, so there is nothing more to read;
//  * responses are piling up because the client pipelines requests without
//    reading the responses (backpressure);
//  * a maximum-size request's worth of bytes is already buffered. The parser
//    is then guaranteed to return a complete request or an error, so reading
//    resumes once that request is handled. It can never deadlock waiting for
//    bytes it refuses to read.
bool Connection::wants_to_read() const {
    return state_ == State::Active && !peer_finished_sending_ &&
           pending_output_bytes() < config_.output_high_water &&
           parser_.buffered_bytes() < config_.limits.max_request_bytes();
}

void Connection::on_poll_events(short revents, Clock::time_point now) {
    if (state_ == State::Closed) return;
    if (revents & POLLNVAL) {
        close_now("invalid socket descriptor");
        return;
    }
    if (revents & POLLERR) {
        close_now("socket error: " + errno_message(pending_socket_error(fd())));
        return;
    }
    if (state_ == State::Draining) {
        if (revents & (POLLIN | POLLHUP)) discard_lingering_input();
        return;
    }

    // POLLHUP is handled like POLLIN, not as "the client is gone". macOS
    // reports POLLHUP when the client only half-closes (sends FIN but keeps
    // receiving). recv() says exactly what happened: data, EOF, or an error.
    if ((revents & (POLLIN | POLLHUP)) && wants_to_read()) read_from_socket(now);

    // Always try to make progress: answer any complete requests now buffered,
    // then write output. This covers POLLOUT too, including a POLLOUT that
    // POLLHUP hides (POSIX makes the two mutually exclusive).
    advance(now);
}

void Connection::read_from_socket(Clock::time_point now) {
    // One recv() per readiness event. poll() said data is waiting, so this
    // will not block. If more is left afterwards, poll() reports the socket
    // readable again immediately (level-triggered). Nothing here assumes the
    // bytes form whole requests. They are simply appended to the parser's
    // buffer.
    char chunk[kRecvChunkBytes];
    const std::size_t room = config_.limits.max_request_bytes() - parser_.buffered_bytes();
    const std::size_t want = std::min(sizeof chunk, room);

    ssize_t received = 0;
    do {
        received = ::recv(fd(), chunk, want, 0);
    } while (received < 0 && errno == EINTR);  // a signal arrived before any data was copied: retry
    const int error = errno;

    if (received > 0) {
        const auto count = static_cast<std::size_t>(received);
        ++stats_.recv_calls;
        stats_.bytes_received += count;
        last_activity_ = now;
        parser_.feed(std::string_view(chunk, count));
        logger_.debug("[conn ", id_, "] recv() -> ", count, " bytes (", parser_.buffered_bytes(),
                      " unparsed bytes buffered)");
        return;
    }
    if (received == 0) {
        // Orderly shutdown (FIN): the client will send nothing more. It may
        // still be reading, so requests already received are still answered.
        peer_finished_sending_ = true;
        last_activity_ = now;
        logger_.debug("[conn ", id_, "] recv() -> 0: client finished sending (FIN)");
        return;
    }
    if (error == EAGAIN || error == EWOULDBLOCK) return;  // spurious wakeup
    close_now("recv() failed: " + errno_message(error));  // e.g. ECONNRESET
}

void Connection::advance(Clock::time_point now) {
    // Alternate between turning buffered bytes into responses and pushing
    // responses into the socket until neither step makes progress. Every
    // pass consumes input or sends output, so the loop terminates.
    while (state_ == State::Active || state_ == State::Closing) {
        bool progress = false;
        if (state_ == State::Active) progress = respond_to_buffered_requests();
        if (flush_output(now)) progress = true;
        if (!progress) break;
    }

    if (state_ == State::Active && peer_finished_sending_ && pending_output_bytes() == 0) {
        // The client sent FIN and every complete request it sent has been answered.
        close_now(parser_.has_partial_request() ? "client closed the connection in the middle of a request"
                                                : "client closed the connection");
        return;
    }
    if (state_ == State::Closing && pending_output_bytes() == 0) {
        if (peer_finished_sending_) {
            close_now("closed after final response");  // client already sent FIN: nothing to drain
        } else {
            begin_lingering_close(now);
        }
    }
}

bool Connection::respond_to_buffered_requests() {
    // Answer every complete request already buffered, strictly in arrival
    // order. That is what makes pipelining correct: responses are appended to
    // a single output buffer in parse order, so they leave in request order.
    // The high-water check pauses parsing while a client that pipelines
    // without reading has a large backlog of unsent responses.
    bool responded = false;
    while (state_ == State::Active && pending_output_bytes() < config_.output_high_water) {
        ParseResult parsed = parser_.next();
        if (parsed.kind == ParseResult::Kind::Incomplete) break;
        responded = true;

        if (parsed.kind == ParseResult::Kind::Error) {
            // Framing is lost: there is no reliable way to tell where the
            // next request would begin. Answer, then close. RFC 9112 §6.3
            // treats invalid framing as an unrecoverable error.
            Response response = text_response(parsed.error.status, parsed.error.message);
            response.close_connection = true;
            logger_.info("[conn ", id_, "] #", stats_.requests + 1, " unparseable request -> ", response.status, ' ',
                         reason_phrase(response.status), " (", parsed.error.message, ") | close");
            queue_response(std::move(response));
            break;
        }

        const Request& request = parsed.request;
        Response response = run_handler(request);
        // Keep-alive is decided per request. "Connection: close" (or
        // HTTP/1.0 without keep-alive) makes this the final response.
        if (!request.wants_keep_alive()) response.close_connection = true;
        // A response to HEAD never carries a body on the wire (RFC 9112 §6.3).
        if (request.method == "HEAD") response.omit_body = true;
        logger_.info("[conn ", id_, "] #", stats_.requests + 1, ' ', excerpt(request.method, 16), ' ',
                     excerpt(request.target, 80), ' ', version_name(request.version), " -> ", response.status, ' ',
                     reason_phrase(response.status), " | stream bytes [", request.stream_begin, ',',
                     request.stream_end, ") = ", request.head_bytes, " head + ", request.body.size(), " body | ",
                     response.close_connection ? "close" : "keep-alive");
        queue_response(std::move(response));
    }
    return responded;
}

Response Connection::run_handler(const Request& request) {
    try {
        return handler_(request);
    } catch (const std::exception& e) {
        logger_.info("[conn ", id_, "] internal error while handling request: ", e.what());
    } catch (...) {
        logger_.info("[conn ", id_, "] internal error while handling request: unknown exception");
    }
    // The request was framed correctly, so the stream itself is still
    // intact. But the server has hit a condition it did not expect, so the
    // conservative choice is to report 500 and close. The client reconnects
    // to a clean state.
    Response response = text_response(500, "internal server error");
    response.close_connection = true;
    return response;
}

void Connection::queue_response(Response response) {
    ++stats_.requests;
    // Evidence headers: which accepted connection produced this response,
    // and its position on that connection. N responses that share one
    // X-Connection-Id and count 1..N show, from the server's side, that one
    // TCP connection carried all N requests.
    response.headers.push_back(Header{"X-Connection-Id", std::to_string(id_)});
    response.headers.push_back(Header{"X-Request-Number", std::to_string(stats_.requests)});
    output_ += serialize(response);
    if (response.close_connection) state_ = State::Closing;  // last response: stop reading and parsing
}

bool Connection::flush_output(Clock::time_point now) {
    bool sent_any = false;
    while (pending_output_bytes() > 0) {
        const std::size_t remaining = pending_output_bytes();
        const ssize_t sent = ::send(fd(), output_.data() + output_sent_, remaining, kSendFlags);
        const int error = errno;

        if (sent > 0) {
            const auto count = static_cast<std::size_t>(sent);
            output_sent_ += count;
            sent_any = true;
            last_activity_ = now;
            ++stats_.send_calls;
            stats_.bytes_sent += count;
            if (count < remaining) {
                // Short write: the kernel's send buffer filled part-way
                // through. This is not an error. The unsent tail stays
                // queued and the loop tries again, which normally hits
                // EAGAIN and then waits for POLLOUT.
                ++stats_.short_sends;
                logger_.debug("[conn ", id_, "] send() accepted ", count, " of ", remaining, " bytes (short write)");
            } else {
                logger_.debug("[conn ", id_, "] send() -> ", count, " bytes");
            }
            continue;
        }
        if (sent < 0 && error == EINTR) continue;
        if (sent == 0 || error == EAGAIN || error == EWOULDBLOCK) {
            // The send buffer is full because the client is not reading fast
            // enough. Keep the bytes and let poll() report POLLOUT when
            // there is room again.
            ++stats_.blocked_sends;
            logger_.debug("[conn ", id_, "] send() would block; ", remaining, " bytes wait for POLLOUT");
            break;
        }
        close_now("send() failed: " + errno_message(error));  // EPIPE / ECONNRESET: the client is gone
        return sent_any;
    }

    // Reclaim the sent prefix, again only when it is at least half the
    // buffer, to avoid quadratic copying under heavy pipelining.
    if (output_sent_ == output_.size()) {
        output_.clear();
        output_sent_ = 0;
    } else if (output_sent_ >= output_.size() / 2) {
        output_.erase(0, output_sent_);
        output_sent_ = 0;
    }
    return sent_any;
}

// The final response has been handed to the kernel, but calling close() now
// could lose it. The client may already have pipelined more requests that
// sit unread in our receive buffer, and closing a socket with unread data
// makes the kernel send an RST instead of a FIN. RFC 9112 §9.6 warns that
// the RST might erase data the client has received but not yet read,
// including our final response. At best, the client sees ECONNRESET
// instead of a clean end-of-stream.
//
// So the connection is closed in stages. shutdown(SHUT_WR) queues a FIN
// behind the response ("no more responses"), and the server keeps reading
// and discarding input until the client closes its side too, or the linger
// timeout expires.
void Connection::begin_lingering_close(Clock::time_point now) {
    if (::shutdown(fd(), SHUT_WR) != 0) {
        close_now("closed after final response");
        return;
    }
    logger_.debug("[conn ", id_, "] final response sent; shutdown(SHUT_WR) queued FIN, discarding input until the client closes");
    state_ = State::Draining;
    linger_deadline_ = now + config_.linger_timeout;
}

void Connection::discard_lingering_input() {
    char chunk[kRecvChunkBytes];
    ssize_t received = 0;
    do {
        received = ::recv(fd(), chunk, sizeof chunk, 0);
    } while (received < 0 && errno == EINTR);
    const int error = errno;

    if (received > 0) {
        lingered_bytes_ += static_cast<std::size_t>(received);
        if (lingered_bytes_ > config_.max_linger_bytes) close_now("closed after final response (discard limit reached)");
        return;
    }
    if (received < 0 && (error == EAGAIN || error == EWOULDBLOCK)) return;
    close_now("closed after final response");  // client's FIN (or reset): it is done too
}

Clock::time_point Connection::deadline() const {
    switch (state_) {
        case State::Active:
        case State::Closing:
            return last_activity_ + config_.idle_timeout;
        case State::Draining:
            return linger_deadline_;
        case State::Closed:
            break;
    }
    return Clock::time_point::max();
}

void Connection::on_deadline(Clock::time_point now) {
    switch (state_) {
        case State::Active:
            if (pending_output_bytes() > 0) {
                close_now("idle timeout: client stopped reading responses");
            } else if (parser_.has_partial_request()) {
                // The client started a request but did not finish it in time.
                // Tell it why before closing.
                Response response = text_response(408, "timed out waiting for the rest of the request");
                response.close_connection = true;
                logger_.info("[conn ", id_, "] #", stats_.requests + 1,
                             " incomplete request timed out -> 408 Request Timeout | close");
                queue_response(std::move(response));
                last_activity_ = now;  // a fresh window in which to write the 408
                advance(now);
            } else {
                // RFC 9112 §9.5: a server may close an idle persistent
                // connection at any time. Nothing is in flight, so no response.
                close_now("idle timeout (no new request)");
            }
            return;
        case State::Closing:
            close_now("timed out writing the final response");
            return;
        case State::Draining:
            close_now("closed after final response (linger timeout)");
            return;
        case State::Closed:
            return;
    }
}

void Connection::close_now(std::string_view reason) {
    if (state_ == State::Closed) return;
    socket_.reset();
    state_ = State::Closed;
    logger_.info("[conn ", id_, "] closed connection from ", peer_, ": ", reason, " | ", describe_stats());
}

std::string Connection::describe_stats() const {
    return "requests=" + std::to_string(stats_.requests) + " recv_calls=" + std::to_string(stats_.recv_calls) +
           " bytes_in=" + std::to_string(stats_.bytes_received) + " send_calls=" + std::to_string(stats_.send_calls) +
           " bytes_out=" + std::to_string(stats_.bytes_sent) + " short_sends=" + std::to_string(stats_.short_sends) +
           " blocked_sends=" + std::to_string(stats_.blocked_sends);
}

}  // namespace calc
