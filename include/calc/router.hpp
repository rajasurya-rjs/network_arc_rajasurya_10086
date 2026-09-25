#pragma once

#include "calc/request.hpp"
#include "calc/response.hpp"

namespace calc {

// Maps one complete, well-framed request to a response. All HTTP semantics
// (Host rules, routing, method checks, parameter validation, status codes)
// live here. The Connection only decides keep-alive vs. close.
//
// Checks are applied in this order, and the first failure decides the status:
//
//   1. Host header valid (HTTP/1.1 needs exactly one)    -> 400
//   2. path is a known operation                         -> 404
//   3. method is GET                                     -> 405 (+ Allow: GET)
//   4. query has exactly a and b, both valid int64       -> 400
//   5. arithmetic succeeds (no /0, no overflow)          -> 400
//   6. success                                           -> 200, body = result
//
// Every outcome in this list keeps the connection open. The request was
// framed correctly, so the server knows exactly where the next one starts.
Response handle_request(const Request& request);

}  // namespace calc
