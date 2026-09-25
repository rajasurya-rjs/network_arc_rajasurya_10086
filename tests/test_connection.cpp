// Connection tests over a REAL loopback TCP connection, created in-process
// (listen, connect, accept) with no threads. The test controls exactly which
// bytes sit in the kernel buffers before each recv()/send() the Connection
// makes. That makes behaviour like "three requests in one recv()" and "short
// writes" deterministic to assert.

#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "calc/connection.hpp"
#include "calc/router.hpp"
#include "calc/socket_utils.hpp"
#include "test_framework.hpp"

namespace {

using calc::Clock;
using calc::Connection;
using calc::FileDescriptor;
using calc::Request;
using calc::Response;
using namespace std::chrono_literals;

const std::string kAdd = "GET /add?a=2&b=3 HTTP/1.1\r\nHost: localhost\r\n\r\n";
const std::string kSub = "GET /sub?a=10&b=4 HTTP/1.1\r\nHost: localhost\r\n\r\n";
const std::string kMul = "GET /mul?a=6&b=7 HTTP/1.1\r\nHost: localhost\r\n\r\n";

struct TcpPair {
    FileDescriptor client;
    FileDescriptor server;
};

// The kernel completes the TCP handshake and queues the connection by
// itself, so connect() and then accept() work from a single thread.
TcpPair make_tcp_pair() {
    FileDescriptor listener = calc::create_listening_socket("127.0.0.1", 0, 4);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(calc::local_port(listener.get()));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    FileDescriptor client(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(client.valid());
    REQUIRE(::connect(client.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0);
    calc::set_tcp_nodelay(client.get());  // so each small write goes out as its own segment

    pollfd p{listener.get(), POLLIN, 0};
    REQUIRE(::poll(&p, 1, 2000) == 1);
    FileDescriptor server(::accept(listener.get(), nullptr, nullptr));
    REQUIRE(server.valid());
    REQUIRE(calc::set_nonblocking(server.get()));
    return {std::move(client), std::move(server)};
}

void write_all(int fd, std::string_view data) {
    while (!data.empty()) {
        const ssize_t sent = ::send(fd, data.data(), data.size(), 0);
        REQUIRE(sent > 0);
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
}

// Waits until at least `count` unread bytes sit in fd's kernel receive
// buffer, so the test knows exactly what the Connection's next recv() sees.
void wait_for_unread_bytes(int fd, std::size_t count) {
    const auto give_up = Clock::now() + 2s;
    while (true) {
        int available = 0;
        REQUIRE(::ioctl(fd, FIONREAD, &available) == 0);
        if (static_cast<std::size_t>(available) >= count) return;
        REQUIRE(Clock::now() < give_up);
        ::usleep(200);
    }
}

void set_buffer_size(int fd, int option, int bytes) { ::setsockopt(fd, SOL_SOCKET, option, &bytes, sizeof bytes); }

struct ClientResponse {
    int status = 0;
    std::string head;
    std::string body;

    std::string header(const std::string& name) const {
        const std::string needle = "\r\n" + name + ": ";
        const std::size_t pos = head.find(needle);
        if (pos == std::string::npos) return "";
        const std::size_t start = pos + needle.size();
        return head.substr(start, head.find("\r\n", start) - start);
    }
};

// Client side of the test: collects bytes and splits them into responses by Content-Length.
class ClientReader {
public:
    explicit ClientReader(int fd) : fd_(fd) {}

    std::optional<ClientResponse> try_take() {
        const std::size_t head_end = buffer_.find("\r\n\r\n");
        if (head_end == std::string::npos) return std::nullopt;
        ClientResponse response;
        response.head = buffer_.substr(0, head_end + 2);
        response.status = std::stoi(buffer_.substr(9, 3));
        const std::size_t length = std::stoul(response.header("Content-Length"));
        if (buffer_.size() < head_end + 4 + length) return std::nullopt;
        response.body = buffer_.substr(head_end + 4, length);
        buffer_.erase(0, head_end + 4 + length);
        return response;
    }

    // Non-blocking: reads everything currently available.
    void read_available() {
        char chunk[64 * 1024];
        while (!eof_ && !reset_) {
            const ssize_t received = ::recv(fd_, chunk, sizeof chunk, MSG_DONTWAIT);
            if (received > 0) {
                buffer_.append(chunk, static_cast<std::size_t>(received));
            } else if (received == 0) {
                eof_ = true;
            } else if (errno == ECONNRESET) {
                reset_ = true;
            } else if (errno != EINTR) {
                return;  // EAGAIN: nothing more right now
            }
        }
    }

    // Waits for one complete response that the server has already written.
    ClientResponse take() {
        const auto give_up = Clock::now() + 2s;
        while (true) {
            if (auto response = try_take()) return *response;
            REQUIRE(!eof_ && !reset_);
            REQUIRE(Clock::now() < give_up);
            pollfd p{fd_, POLLIN, 0};
            ::poll(&p, 1, 50);
            read_available();
        }
    }

    // Waits until `marker` appears, then returns every buffered byte unframed.
    std::string take_raw_until(const std::string& marker) {
        const auto give_up = Clock::now() + 2s;
        while (buffer_.find(marker) == std::string::npos) {
            REQUIRE(!eof_ && !reset_);
            REQUIRE(Clock::now() < give_up);
            pollfd p{fd_, POLLIN, 0};
            ::poll(&p, 1, 50);
            read_available();
        }
        return std::exchange(buffer_, std::string());
    }

    // True if the server closed its side cleanly (FIN, not RST) with no
    // further response bytes before it.
    bool clean_eof_within(std::chrono::milliseconds timeout) {
        const auto give_up = Clock::now() + timeout;
        while (!eof_ && !reset_ && Clock::now() < give_up) {
            pollfd p{fd_, POLLIN, 0};
            ::poll(&p, 1, 20);
            read_available();
        }
        return eof_ && !reset_ && buffer_.empty();
    }

private:
    int fd_;
    std::string buffer_;
    bool eof_ = false;
    bool reset_ = false;
};

// A Connection wired to one end of a loopback TCP connection. The test holds
// the other end and plays the client.
struct Harness {
    calc::ServerConfig config;
    std::ostringstream log;
    calc::Logger logger{calc::LogLevel::Debug, log};
    TcpPair tcp;
    std::unique_ptr<Connection> connection;
    ClientReader reader;

    explicit Harness(calc::RequestHandler handler = calc::handle_request,
                     const std::function<void(calc::ServerConfig&)>& configure = {})
        : tcp(make_tcp_pair()), reader(tcp.client.get()) {
        if (configure) configure(config);
        connection = std::make_unique<Connection>(std::move(tcp.server), 1, "test-client", config, std::move(handler),
                                                  logger, Clock::now());
    }

    int client() const { return tcp.client.get(); }
    int server() const { return connection->fd(); }
    const calc::ConnectionStats& stats() const { return connection->stats(); }

    // Delivers exactly one readiness event, as the server loop would after poll().
    void deliver(short revents) { connection->on_poll_events(revents, Clock::now()); }

    // One iteration of the server's event loop for this connection.
    bool step(int timeout_ms = 50) {
        if (connection->closed()) return false;
        pollfd p{connection->fd(), connection->poll_events(), 0};
        if (::poll(&p, 1, timeout_ms) <= 0) return false;
        connection->on_poll_events(p.revents, Clock::now());
        return true;
    }

    void run_until_closed(std::chrono::milliseconds timeout = 2000ms) {
        const auto give_up = Clock::now() + timeout;
        while (!connection->closed() && Clock::now() < give_up) step(20);
    }
};

void pipelined_requests_arrive_in_one_recv() {
    Harness h;
    const std::string batch = kAdd + kSub + kMul;
    write_all(h.client(), batch);
    wait_for_unread_bytes(h.server(), batch.size());

    h.deliver(POLLIN);  // one readiness event, so exactly one recv()

    CHECK_EQ(h.stats().recv_calls, 1u);
    CHECK_EQ(h.stats().requests, 3u);  // three requests parsed out of that one read
    const ClientResponse r1 = h.reader.take();
    const ClientResponse r2 = h.reader.take();
    const ClientResponse r3 = h.reader.take();
    CHECK_EQ(r1.body, "5\n");
    CHECK_EQ(r2.body, "6\n");
    CHECK_EQ(r3.body, "42\n");
    CHECK_EQ(r1.header("X-Request-Number"), "1");
    CHECK_EQ(r3.header("X-Request-Number"), "3");
    CHECK_EQ(r3.header("Connection"), "keep-alive");
    CHECK(!h.connection->closed());
}

void one_request_across_many_recv_calls() {
    Harness h;
    for (std::size_t i = 0; i < kAdd.size(); ++i) {
        write_all(h.client(), kAdd.substr(i, 1));
        wait_for_unread_bytes(h.server(), 1);
        h.deliver(POLLIN);
        const bool last_byte = i + 1 == kAdd.size();
        if (!CHECK_EQ(h.stats().requests, last_byte ? 1u : 0u)) break;  // no response before the last byte
    }
    CHECK_EQ(h.stats().recv_calls, kAdd.size());  // one recv() per byte
    CHECK_EQ(h.reader.take().body, "5\n");
}

void body_split_across_reads_then_next_request() {
    Harness h;
    const std::string head = "POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\n";
    write_all(h.client(), head);
    wait_for_unread_bytes(h.server(), head.size());
    h.deliver(POLLIN);
    CHECK_EQ(h.stats().requests, 0u);

    write_all(h.client(), "a=2");
    wait_for_unread_bytes(h.server(), 3);
    h.deliver(POLLIN);
    CHECK_EQ(h.stats().requests, 0u);  // 3 of 7 body bytes: must keep waiting

    const std::string rest = "&b=3" + kMul;  // last 4 body bytes + the next request
    write_all(h.client(), rest);
    wait_for_unread_bytes(h.server(), rest.size());
    h.deliver(POLLIN);
    CHECK_EQ(h.stats().requests, 2u);
    CHECK_EQ(h.reader.take().status, 405);
    CHECK_EQ(h.reader.take().body, "42\n");
}

void head_response_has_no_body_on_the_wire() {
    Harness h;
    const std::string batch = "HEAD /add?a=2&b=3 HTTP/1.1\r\nHost: x\r\n\r\n" + kMul;
    write_all(h.client(), batch);
    wait_for_unread_bytes(h.server(), batch.size());
    h.deliver(POLLIN);
    const std::string raw = h.reader.take_raw_until("\r\n\r\n42\n");
    const std::size_t first_end = raw.find("\r\n\r\n") + 4;
    CHECK_EQ(raw.substr(0, 31), "HTTP/1.1 405 Method Not Allowed");
    // The next response starts immediately after the HEAD response's blank line.
    CHECK_EQ(raw.substr(first_end, 15), "HTTP/1.1 200 OK");
}

void short_writes_are_resumed_until_complete() {
    // A handler returning a 1 MiB body, with deliberately tiny kernel
    // buffers, so that send() cannot take the whole response at once.
    std::string big(1 << 20, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + (i * 7) % 26);
    Harness h([&big](const Request&) {
        Response response;
        response.body = big;
        return response;
    });
    set_buffer_size(h.server(), SO_SNDBUF, 4096);
    set_buffer_size(h.client(), SO_RCVBUF, 4096);

    write_all(h.client(), kAdd);
    wait_for_unread_bytes(h.server(), kAdd.size());
    h.deliver(POLLIN);
    CHECK(h.connection->pending_output_bytes() > 0);  // kernel accepted only part of it
    CHECK(h.stats().blocked_sends >= 1);              // ...then said EAGAIN

    // Act as a slow reader. Each time the client drains its buffer, poll()
    // reports POLLOUT and the connection writes the next piece.
    std::optional<ClientResponse> response;
    const auto give_up = Clock::now() + 10s;
    while (!response && Clock::now() < give_up) {
        h.reader.read_available();
        response = h.reader.try_take();
        h.step(5);
    }
    REQUIRE(response.has_value());
    CHECK_EQ(response->header("Content-Length"), std::to_string(big.size()));
    CHECK(response->body == big);  // every byte, in order, nothing duplicated or lost
    CHECK_EQ(h.connection->pending_output_bytes(), 0u);
    CHECK(h.stats().send_calls > 1);
    CHECK(h.stats().short_sends + h.stats().blocked_sends > 0);
    CHECK(!h.connection->closed());
}

void backpressure_pauses_reading_until_responses_drain() {
    Harness h(calc::handle_request, [](calc::ServerConfig& c) { c.output_high_water = 4096; });
    set_buffer_size(h.server(), SO_SNDBUF, 8192);
    set_buffer_size(h.client(), SO_RCVBUF, 8192);
    REQUIRE(calc::set_nonblocking(h.client()));

    constexpr int kRequests = 3000;
    std::string requests;
    for (int i = 0; i < kRequests; ++i) {
        requests += "GET /add?a=" + std::to_string(i) + "&b=0 HTTP/1.1\r\nHost: x\r\n\r\n";
    }
    std::size_t written = 0;
    const auto send_some = [&] {
        if (written == requests.size()) return false;
        const ssize_t sent = ::send(h.client(), requests.data() + written, requests.size() - written, 0);
        if (sent <= 0) return false;
        written += static_cast<std::size_t>(sent);
        return true;
    };

    // Phase 1: pipeline as much as possible WITHOUT reading any responses.
    int idle_rounds = 0;
    const auto phase1_end = Clock::now() + 3s;
    while (idle_rounds < 5 && Clock::now() < phase1_end) {
        const bool sent = send_some();
        const bool served = h.step(10);
        idle_rounds = (sent || served) ? 0 : idle_rounds + 1;
    }
    CHECK((h.connection->poll_events() & POLLIN) == 0);  // it stopped reading...
    CHECK(h.connection->pending_output_bytes() >= h.config.output_high_water);  // ...because of the backlog
    CHECK(h.stats().requests < static_cast<std::uint64_t>(kRequests));

    // Phase 2: the client starts reading, and everything flows again, in order.
    int received = 0;
    bool in_order = true;
    const auto give_up = Clock::now() + 20s;
    while (received < kRequests && Clock::now() < give_up) {
        send_some();
        h.reader.read_available();
        while (auto response = h.reader.try_take()) {
            in_order = in_order && response->body == std::to_string(received) + "\n" &&
                       response->header("X-Request-Number") == std::to_string(received + 1);
            ++received;
        }
        h.step(2);
    }
    CHECK_EQ(received, kRequests);
    CHECK(in_order);
    CHECK(!h.connection->closed());
}

void connection_close_makes_that_response_the_last() {
    Harness h;
    const std::string closing = "GET /sub?a=10&b=4 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    const std::string batch = kAdd + closing + kMul;
    write_all(h.client(), batch);
    wait_for_unread_bytes(h.server(), batch.size());
    h.deliver(POLLIN);

    CHECK_EQ(h.stats().requests, 2u);  // the request after "close" is never processed
    const ClientResponse first = h.reader.take();
    const ClientResponse last = h.reader.take();
    CHECK_EQ(first.header("Connection"), "keep-alive");
    CHECK_EQ(last.header("Connection"), "close");
    CHECK_EQ(last.body, "6\n");
    CHECK(h.reader.clean_eof_within(1000ms));  // FIN after the final response, no third response
    CHECK(!h.connection->closed());            // lingering: still draining the client's leftover bytes

    h.tcp.client.reset();  // client closes too
    h.run_until_closed();
    CHECK(h.connection->closed());
}

void framing_error_is_answered_then_closed() {
    Harness h;
    const std::string batch = "THIS IS NOT HTTP\r\n\r\n" + kAdd;
    write_all(h.client(), batch);
    wait_for_unread_bytes(h.server(), batch.size());
    h.deliver(POLLIN);
    const ClientResponse response = h.reader.take();
    CHECK_EQ(response.status, 400);
    CHECK_EQ(response.header("Connection"), "close");
    CHECK_EQ(h.stats().requests, 1u);  // the valid request after the garbage is not trusted
    CHECK(h.reader.clean_eof_within(1000ms));
}

void handler_exception_becomes_500_and_close() {
    Harness h([](const Request&) -> Response { throw std::runtime_error("simulated bug"); });
    write_all(h.client(), kAdd);
    wait_for_unread_bytes(h.server(), kAdd.size());
    h.deliver(POLLIN);
    const ClientResponse response = h.reader.take();
    CHECK_EQ(response.status, 500);
    CHECK_EQ(response.header("Connection"), "close");
    CHECK(h.reader.clean_eof_within(1000ms));
    CHECK(h.log.str().find("simulated bug") != std::string::npos);
}

void half_close_after_pipelining_still_gets_every_response() {
    Harness h;
    write_all(h.client(), kAdd + kSub + kMul);
    ::shutdown(h.client(), SHUT_WR);  // like `printf ... | nc -N`: send everything, then FIN
    h.run_until_closed();
    CHECK(h.connection->closed());
    CHECK_EQ(h.reader.take().body, "5\n");
    CHECK_EQ(h.reader.take().body, "6\n");
    CHECK_EQ(h.reader.take().body, "42\n");
    CHECK(h.reader.clean_eof_within(1000ms));
    CHECK(h.log.str().find("client closed the connection") != std::string::npos);
}

void client_reset_is_handled() {
    Harness h;
    write_all(h.client(), "GET /add?a=1");  // half a request...
    wait_for_unread_bytes(h.server(), 12);
    h.deliver(POLLIN);
    const linger abortive{1, 0};  // ...then close with RST instead of FIN
    ::setsockopt(h.client(), SOL_SOCKET, SO_LINGER, &abortive, sizeof abortive);
    h.tcp.client.reset();
    h.run_until_closed();
    CHECK(h.connection->closed());
}

void idle_connection_is_closed_at_its_deadline() {
    Harness h(calc::handle_request, [](calc::ServerConfig& c) { c.idle_timeout = 100ms; });
    write_all(h.client(), kAdd);
    wait_for_unread_bytes(h.server(), kAdd.size());
    h.deliver(POLLIN);
    CHECK_EQ(h.reader.take().body, "5\n");
    CHECK(!h.connection->closed());

    h.connection->on_deadline(h.connection->deadline());  // the timer fires
    CHECK(h.connection->closed());
    CHECK(h.reader.clean_eof_within(1000ms));
    CHECK(h.log.str().find("idle timeout") != std::string::npos);
}

void unfinished_request_times_out_with_408() {
    Harness h(calc::handle_request, [](calc::ServerConfig& c) { c.idle_timeout = 100ms; });
    const std::string partial = "GET /add?a=1&b=2 HTTP/1.1\r\nHo";
    write_all(h.client(), partial);
    wait_for_unread_bytes(h.server(), partial.size());
    h.deliver(POLLIN);
    CHECK_EQ(h.stats().requests, 0u);

    h.connection->on_deadline(h.connection->deadline());
    const ClientResponse response = h.reader.take();
    CHECK_EQ(response.status, 408);
    CHECK_EQ(response.header("Connection"), "close");
    CHECK(h.reader.clean_eof_within(1000ms));
}

void lingering_close_delivers_final_response_despite_unread_input() {
    // The client pipelines a "Connection: close" request followed by 32 KiB
    // the server will never parse. If the server just called close() with
    // those bytes unread, the kernel would send an RST, which might erase
    // the response (RFC 9112 §9.6) and at best ends the stream with
    // ECONNRESET. With the lingering close, the client gets the response
    // and then a clean FIN.
    Harness h;
    const std::string batch = "GET /mul?a=6&b=7 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n" +
                              std::string(32 * 1024, 'x');
    write_all(h.client(), batch);
    wait_for_unread_bytes(h.server(), batch.size());
    for (int i = 0; i < 20 && !h.connection->closed(); ++i) h.step(10);  // respond, FIN, drain

    const ClientResponse response = h.reader.take();
    CHECK_EQ(response.body, "42\n");
    CHECK(h.reader.clean_eof_within(1000ms));  // FIN, not ECONNRESET
    CHECK(h.log.str().find("shutdown(SHUT_WR)") != std::string::npos);
}

}  // namespace

std::vector<test::TestCase> connection_tests() {
    return {
        {"connection: pipelined requests arrive in one recv()", pipelined_requests_arrive_in_one_recv},
        {"connection: one request across many recv() calls", one_request_across_many_recv_calls},
        {"connection: body split across reads, then next request", body_split_across_reads_then_next_request},
        {"connection: HEAD response has no body on the wire", head_response_has_no_body_on_the_wire},
        {"connection: short writes are resumed until complete", short_writes_are_resumed_until_complete},
        {"connection: backpressure pauses reading until responses drain",
         backpressure_pauses_reading_until_responses_drain},
        {"connection: Connection: close makes that response the last", connection_close_makes_that_response_the_last},
        {"connection: framing error is answered, then closed", framing_error_is_answered_then_closed},
        {"connection: handler exception becomes 500 + close", handler_exception_becomes_500_and_close},
        {"connection: half-close after pipelining still gets every response",
         half_close_after_pipelining_still_gets_every_response},
        {"connection: client reset (RST) is handled", client_reset_is_handled},
        {"connection: idle connection is closed at its deadline", idle_connection_is_closed_at_its_deadline},
        {"connection: unfinished request times out with 408", unfinished_request_times_out_with_408},
        {"connection: lingering close delivers the final response despite unread input",
         lingering_close_delivers_final_response_despite_unread_input},
    };
}
