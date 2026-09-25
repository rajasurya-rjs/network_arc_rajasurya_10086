#pragma once

#include <sys/socket.h>

#include <cstdint>
#include <string>

#include "calc/file_descriptor.hpp"

namespace calc {

// socket() + setsockopt(SO_REUSEADDR) + bind() + listen(), returning a
// non-blocking listening socket. Throws std::runtime_error naming the failing
// call and the address, e.g. "bind 127.0.0.1:8080: Address already in use".
FileDescriptor create_listening_socket(const std::string& host, std::uint16_t port, int backlog);

// Puts the descriptor in O_NONBLOCK mode. Returns false on failure.
bool set_nonblocking(int fd);

// Turns off Nagle's algorithm on a TCP socket. Best effort.
void set_tcp_nodelay(int fd);

// "127.0.0.1:54321" or "[::1]:54321".
std::string format_address(const sockaddr* address, socklen_t length);
std::string local_address(int fd);
std::uint16_t local_port(int fd);

// strerror() for the given errno value, as a std::string.
std::string errno_message(int error_number);

}  // namespace calc
