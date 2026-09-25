#include "calc/request.hpp"

#include "calc/text.hpp"

namespace calc {

const std::string* Request::header(std::string_view name) const {
    for (const Header& h : headers) {
        if (iequals(h.name, name)) return &h.value;
    }
    return nullptr;
}

std::size_t Request::header_count(std::string_view name) const {
    std::size_t count = 0;
    for (const Header& h : headers) {
        if (iequals(h.name, name)) ++count;
    }
    return count;
}

std::string_view Request::path() const {
    const std::string_view view(target);
    return view.substr(0, view.find('?'));
}

std::string_view Request::query() const {
    const std::string_view view(target);
    const std::size_t mark = view.find('?');
    return mark == std::string_view::npos ? std::string_view{} : view.substr(mark + 1);
}

bool Request::wants_keep_alive() const {
    // "Connection" holds a comma-separated list of options and may appear
    // more than once, e.g. "Connection: Upgrade, close".
    bool close = false;
    bool keep_alive = false;
    for (const Header& h : headers) {
        if (!iequals(h.name, "Connection")) continue;
        std::string_view options(h.value);
        while (!options.empty()) {
            const std::size_t comma = options.find(',');
            const std::string_view option = trim_whitespace(options.substr(0, comma));
            if (iequals(option, "close")) close = true;
            if (iequals(option, "keep-alive")) keep_alive = true;
            options = comma == std::string_view::npos ? std::string_view{} : options.substr(comma + 1);
        }
    }
    if (close) return false;
    return version == HttpVersion::Http11 || keep_alive;
}

}  // namespace calc
