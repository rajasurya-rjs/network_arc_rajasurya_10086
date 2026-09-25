#include "calc/logger.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace calc {

// Local wall-clock time with milliseconds, e.g. "14:03:27.512". Millisecond
// resolution makes timeouts and request ordering easy to follow in the log.
std::string Logger::timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const std::time_t seconds = system_clock::to_time_t(now);
    const auto millis = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;

    std::tm local{};
    localtime_r(&seconds, &local);

    char text[16];
    std::snprintf(text, sizeof text, "%02d:%02d:%02d.%03d", local.tm_hour, local.tm_min,
                  local.tm_sec, static_cast<int>(millis));
    return text;
}

}  // namespace calc
