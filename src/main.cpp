// calc_server: a calculator served over persistent TCP connections, using a
// small HTTP/1.1-style protocol implemented directly on POSIX sockets.

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include "calc/config.hpp"
#include "calc/logger.hpp"
#include "calc/router.hpp"
#include "calc/server.hpp"

namespace {

// The only global mutable state. A signal handler may only safely write to a
// volatile sig_atomic_t, so this flag is how SIGINT/SIGTERM reaches the
// event loop.
volatile std::sig_atomic_t g_stop_requested = 0;

extern "C" void request_stop(int) { g_stop_requested = 1; }

void install_signal_handlers() {
    struct sigaction action {};
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;  // no SA_RESTART, so a blocked poll() returns EINTR immediately
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    // Writing to a connection the client has reset raises SIGPIPE, which
    // kills the process by default. Ignoring it turns that into an ordinary
    // EPIPE error from send(), handled per connection.
    std::signal(SIGPIPE, SIG_IGN);
}

void print_usage(std::ostream& out) {
    out << "usage: calc_server [options]\n"
           "  --host HOST             address to listen on (default 127.0.0.1)\n"
           "  --port PORT             TCP port, 0 = any free port (default 8080)\n"
           "  --idle-timeout SECONDS  close idle keep-alive connections after this long (default 30)\n"
           "  --max-connections N     simultaneous connection limit (default 64)\n"
           "  --quiet                 log only startup and shutdown\n"
           "  --verbose               also log every recv()/send() with byte counts\n"
           "  --help                  show this help\n";
}

bool parse_number(const char* text, double min, double max, double& out) {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !(value >= min && value <= max)) return false;
    out = value;
    return true;
}

// Returns false (after printing a message) if the arguments are invalid.
bool parse_arguments(int argc, char** argv, calc::ServerConfig& config) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        double number = 0;

        if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            std::exit(0);
        } else if (arg == "--quiet") {
            config.log_level = calc::LogLevel::Notice;
        } else if (arg == "--verbose") {
            config.log_level = calc::LogLevel::Debug;
        } else if (arg == "--host" && has_value) {
            config.host = argv[++i];
        } else if (arg == "--port" && has_value && parse_number(argv[i + 1], 0, 65535, number)) {
            config.port = static_cast<std::uint16_t>(number);
            ++i;
        } else if (arg == "--idle-timeout" && has_value && parse_number(argv[i + 1], 0.001, 86400, number)) {
            config.idle_timeout = std::chrono::milliseconds(static_cast<long long>(number * 1000));
            ++i;
        } else if (arg == "--max-connections" && has_value && parse_number(argv[i + 1], 1, 10000, number)) {
            config.max_connections = static_cast<std::size_t>(number);
            ++i;
        } else {
            std::cerr << "calc_server: invalid or incomplete option '" << arg << "'\n";
            print_usage(std::cerr);
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    calc::ServerConfig config;
    if (!parse_arguments(argc, argv, config)) return 2;

    install_signal_handlers();
    const calc::Logger logger(config.log_level, std::cerr);

    try {
        calc::Server server(config, calc::handle_request, logger);
        server.run(g_stop_requested);
    } catch (const std::exception& e) {
        std::cerr << "calc_server: " << e.what() << '\n';
        return 1;
    }
    logger.notice("server stopped");
    return 0;
}
