#include "calc/response.hpp"

namespace calc {

Response text_response(int status, std::string_view text) {
    Response response;
    response.status = status;
    response.body.reserve(text.size() + 1);
    response.body.append(text);
    response.body += '\n';
    return response;
}

std::string_view reason_phrase(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Content Too Large";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown";
    }
}

std::string serialize(const Response& response) {
    std::string out;
    out.reserve(128 + response.body.size());

    out += "HTTP/1.1 ";
    out += std::to_string(response.status);
    out += ' ';
    out += reason_phrase(response.status);
    out += "\r\n";

    out += "Content-Length: ";
    out += std::to_string(response.body.size());  // bytes, not characters
    out += "\r\n";
    out += "Content-Type: text/plain\r\n";
    out += response.close_connection ? "Connection: close\r\n" : "Connection: keep-alive\r\n";

    for (const Header& header : response.headers) {
        out += header.name;
        out += ": ";
        out += header.value;
        out += "\r\n";
    }

    out += "\r\n";  // end of head; the body follows immediately
    if (!response.omit_body) out += response.body;
    return out;
}

}  // namespace calc
