#pragma once

#include <ostream>
#include <sstream>
#include <string>

#include "calc/config.hpp"

namespace calc {

// Line-oriented logger. Each line is formatted in full and then written with
// a single call, so lines stay whole. Passed around by reference instead of
// being a global, so tests can silence it or capture its output.
class Logger {
public:
    Logger(LogLevel level, std::ostream& out) : level_(level), out_(out) {}

    template <typename... Parts>
    void notice(const Parts&... parts) const { write(LogLevel::Notice, parts...); }

    template <typename... Parts>
    void info(const Parts&... parts) const { write(LogLevel::Info, parts...); }

    template <typename... Parts>
    void debug(const Parts&... parts) const { write(LogLevel::Debug, parts...); }

    bool debug_enabled() const { return level_ >= LogLevel::Debug; }

private:
    template <typename... Parts>
    void write(LogLevel level, const Parts&... parts) const {
        if (level_ < level) return;
        std::ostringstream line;
        line << timestamp() << ' ';
        (line << ... << parts);
        line << '\n';
        out_ << line.str() << std::flush;
    }

    static std::string timestamp();

    LogLevel level_;
    std::ostream& out_;
};

}  // namespace calc
