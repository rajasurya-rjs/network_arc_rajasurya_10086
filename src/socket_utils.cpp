#include "calc/socket_utils.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace calc {

std::string errno_message(int error_number) { return std::strerror(error_number); }

FileDescriptor create_listening_socket(const std::string& host, std::uint16_t port, int backlog) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;      // IPv4 or IPv6, whichever the host string resolves to
    hints.ai_socktype = SOCK_STREAM;  // TCP: a reliable, ordered byte stream (no message boundaries)
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;

    const std::string port_text = std::to_string(port);
    const std::string where = host + ":" + port_text;

    addrinfo* candidates = nullptr;
    if (const int rc = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &candidates); rc != 0) {
        throw std::runtime_error("resolve " + where + ": " + ::gai_strerror(rc));
    }

    std::string last_error = "no usable address";
    FileDescriptor listener;
    for (const addrinfo* candidate = candidates; candidate != nullptr; candidate = candidate->ai_next) {
        FileDescriptor fd(::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol));
        if (!fd.valid()) {
            last_error = "socket: " + errno_message(errno);
            continue;
        }

        // Without SO_REUSEADDR, restarting the server right after stopping it
        // fails with EADDRINUSE while connections from the previous run sit
        // in TIME_WAIT.
        const int enable = 1;
        ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof enable);

        if (::bind(fd.get(), candidate->ai_addr, candidate->ai_addrlen) != 0) {
            last_error = "bind " + where + ": " + errno_message(errno);
            continue;
        }
        // From here on the kernel completes TCP handshakes on our behalf and
        // queues up to `backlog` finished connections for accept().
        if (::listen(fd.get(), backlog) != 0) {
            last_error = "listen " + where + ": " + errno_message(errno);
            continue;
        }
        // The server's single poll() loop must never block inside accept().
        if (!set_nonblocking(fd.get())) {
            last_error = "fcntl(O_NONBLOCK): " + errno_message(errno);
            continue;
        }
        listener = std::move(fd);
        break;
    }
    ::freeaddrinfo(candidates);

    if (!listener.valid()) throw std::runtime_error(last_error);
    return listener;
}

bool set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void set_tcp_nodelay(int fd) {
    // Nagle's algorithm holds back a small segment while an earlier one is
    // still unacknowledged. The server always writes whole responses, so
    // Nagle never saves anything here. It can, however, delay a pipelined
    // response by a full delayed-ACK interval (up to about 40 ms on Linux).
    const int enable = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof enable);
}

std::string format_address(const sockaddr* address, socklen_t length) {
    char host[NI_MAXHOST];
    char service[NI_MAXSERV];
    if (::getnameinfo(address, length, host, sizeof host, service, sizeof service,
                      NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "<unknown>";
    }
    if (address->sa_family == AF_INET6) return std::string("[") + host + "]:" + service;
    return std::string(host) + ":" + service;
}

std::string local_address(int fd) {
    sockaddr_storage address{};
    socklen_t length = sizeof address;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) return "<unknown>";
    return format_address(reinterpret_cast<const sockaddr*>(&address), length);
}

std::uint16_t local_port(int fd) {
    sockaddr_storage address{};
    socklen_t length = sizeof address;
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) return 0;
    if (address.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
    }
    if (address.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
    }
    return 0;
}

}  // namespace calc
