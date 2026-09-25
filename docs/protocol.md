# Protocol specification

`calc_server` speaks a deliberately small subset of HTTP/1.1 (RFC 9112 / RFC 9110). Everything below is implemented by hand in [`src/request_parser.cpp`](../src/request_parser.cpp), [`src/router.cpp`](../src/router.cpp) and [`src/response.cpp`](../src/response.cpp). Where this document says MUST, the code enforces it and a test checks it.

## 1. Transport

- TCP (IPv4 or IPv6; the default listen address is `127.0.0.1:8080`).
- A connection carries **any number of requests, one after another**. The server never closes a connection after a successful response unless the client asked it to (see §5).
- TCP is a byte stream, so message boundaries come only from the framing rules in §2 and §4. They never depend on how bytes happen to be grouped into `recv()` calls.

## 2. Request format

```
request      = request-line *( header-line ) CRLF [ body ]
request-line = method SP request-target SP HTTP-version CRLF
header-line  = field-name ":" OWS field-value OWS CRLF
```

| Element | Rule | If violated |
|---|---|---|
| line terminator | `CRLF`. A bare `LF` is also accepted (RFC 9112 §2.2 allows it, and it lets requests typed into `nc` work). A bare `CR` is rejected. | 400 |
| leading empty lines | ignored before a request line (RFC 9112 §2.2) | — |
| `method` | a token (RFC 9110 §5.6.2), case-sensitive | 400 |
| separators | exactly one `SP` between the three parts | 400 |
| `request-target` | origin-form only: starts with `/`, visible ASCII (0x21–0x7E) | 400 |
| `HTTP-version` | `HTTP/1.1` or `HTTP/1.0`. `HTTP/1.x` with x > 1 is handled as 1.1. | malformed: 400, other major version: **505** |
| `field-name` | token; **no whitespace before the colon** (RFC 9112 §5.1) | 400 |
| `field-value` | surrounding spaces/tabs stripped; no control characters except HTAB | 400 |
| obsolete line folding | a header line starting with SP/HTAB is rejected (RFC 9112 §5.2) | 400 |
| head size | request line + headers + blank line ≤ **8192 bytes** | **431** |
| header count | ≤ **100** header lines | **431** |

### 2.1 Where a request ends (framing)

This is the rule that makes persistent connections and pipelining possible (RFC 9112 §6.3):

1. The **head** ends at the first empty line (`CRLF CRLF`).
2. If the head has **no `Content-Length`** (and no `Transfer-Encoding`), the request has **no body**. It ends right after the blank line, whatever the method.
3. If the head has **exactly one valid `Content-Length: N`**, the body is **exactly the next N bytes**. Byte N+1 is the first byte of the next request.
4. Anything ambiguous is a **framing error**. The server can no longer know where the next request starts, so it answers once and closes the connection:

| Condition | Status | Why |
|---|---|---|
| `Content-Length` not `1*DIGIT` (e.g. `abc`, `-1`, `+7`, `1.5`, `7, 7`, empty) | 400 | RFC 9112 §6.3: an invalid Content-Length is unrecoverable |
| more than one `Content-Length` header (even with equal values) | 400 | different parsers disagree on which one wins, a classic request-smuggling vector |
| `Content-Length` > 65536 | **413** | the body must be read to find the next request, so it is capped |
| `Transfer-Encoding` present | **501** | chunked decoding is not implemented (RFC 9112 §6.1) |
| `Transfer-Encoding` and `Content-Length` both present | 400 | RFC 9112 §6.3: ambiguous, likely smuggling |

A body is accepted on any method, including `GET`, so the stream stays in sync. No calculator endpoint uses the body.

## 3. Resources

| Path | Operation | Result |
|---|---|---|
| `/add?a=A&b=B` | A + B | decimal integer |
| `/sub?a=A&b=B` | A − B | decimal integer |
| `/mul?a=A&b=B` | A × B | decimal integer |
| `/div?a=A&b=B` | A ÷ B, truncated toward zero (`7/2 = 3`, `-7/2 = -3`) | decimal integer |

- Paths match exactly and case-sensitively (`/ADD`, `/add/`, and `/%61dd` are 404). They are not percent-decoded.
- Query string: `name=value` pairs separated by `&`. Names and values are percent-decoded (RFC 3986 §2.1). `+` stays a literal `+`.
- Operands: **signed 64-bit integers**, grammar `-?[0-9]+` (leading zeros allowed, `-0` is 0). No `+`, spaces, hex, exponents, or decimals.
- Exactly the parameters `a` and `b`, each exactly once. Missing, empty, duplicate, and unknown parameters are all rejected, and so are empty segments (`a=1&&b=2`) and segments without `=`.
- Arithmetic is overflow-checked. A result outside `[-2^63, 2^63-1]` is an error, not a wrapped value (this includes `-2^63 / -1`).

## 4. Response format

```
HTTP/1.1 <status> <reason>\r\n
Content-Length: <exact body size in bytes>\r\n
Content-Type: text/plain\r\n
Connection: keep-alive | close\r\n
[Allow: GET\r\n]                      only on 405
X-Connection-Id: <n>\r\n              which accepted TCP connection answered (1, 2, 3, ...)
X-Request-Number: <k>\r\n             k-th response on that connection (1, 2, 3, ...)
\r\n
<body>
```

- The body is text ending in `\n`. On success it is the result (`5\n`); on error, a short explanation (`division by zero\n`).
- **Exception: a response to `HEAD`** (always a 405 here) ends at the blank line. No body is sent, though `Content-Length` still gives the size the body would have had. A client never reads a body after HEAD (RFC 9112 §6.3), so sending one would desynchronize the stream.
- `Content-Length` counts **bytes** and always equals the exact body size. The client finds the end of a response from `Content-Length`, **never** from the connection closing. That is the difference between length-delimited and connection-delimited messages.
- The status line always says `HTTP/1.1`, also when answering an HTTP/1.0 request (RFC 9110 §2.5).
- `X-Connection-Id` and `X-Request-Number` are evidence headers. They let any client check, from the server's side, that several responses came over one TCP connection.

## 5. Connection management

| Situation | Connection after the response |
|---|---|
| HTTP/1.1 request (default) | **stays open** |
| HTTP/1.1 with `Connection: close` | closed after this response. Any requests pipelined after it are **not processed** (RFC 9112 §9.6). |
| HTTP/1.0 request | closed, unless the request has `Connection: keep-alive` |
| 200, and 400/404/405 for a well-framed request (bad operands, `/0`, overflow, missing Host, unknown path, wrong method) | **stays open**: the request was framed correctly, so the next one can be found |
| framing error (400 malformed syntax, 413, 431, 501, 505) | answered, then closed |
| 500 internal error | answered, then closed (conservative: the server hit something unexpected) |
| no request for **30 s** (`--idle-timeout`) | closed silently (RFC 9112 §9.5 allows this at any time) |
| an unfinished request, then 30 s without progress | **408**, then closed |

`Connection` is parsed as a comma-separated, case-insensitive option list, and `close` wins over `keep-alive`.

**How the server closes.** After the final response is fully written, the server calls `shutdown(SHUT_WR)`, which sends a FIN behind the response. It then keeps reading and discarding input until the client closes, for at most 2 s or 256 KiB. Calling `close()` straight away while the client's unread bytes sit in the receive buffer would make the kernel send an RST instead of a FIN. The RST might erase the final response before the client reads it (RFC 9112 §9.6), and at best the client sees `ECONNRESET` instead of a clean end-of-stream.

## 6. Status codes

| Code | Meaning here | Keeps connection? |
|---|---|---|
| 200 OK | arithmetic succeeded | yes |
| 400 Bad Request | framing or syntax error (malformed request line or header, bad or duplicate `Content-Length`, TE+CL) | **no** |
| 400 Bad Request | well-framed request with bad content: missing, duplicate, or invalid Host (HTTP/1.1, RFC 9112 §3.2); missing, empty, duplicate, unknown, or non-integer operand; out-of-range literal; division by zero; overflow | yes |
| 404 Not Found | path is not `/add`, `/sub`, `/mul`, or `/div` | yes |
| 405 Method Not Allowed | known path, method other than `GET`; includes `Allow: GET` (RFC 9110 §15.5.6) | yes |
| 408 Request Timeout | a request was started but not finished within the idle timeout | no |
| 413 Content Too Large | `Content-Length` over 65536 | no |
| 431 Request Header Fields Too Large | head over 8192 bytes or more than 100 header lines | no |
| 500 Internal Server Error | unexpected exception while handling a request | no |
| 501 Not Implemented | `Transfer-Encoding` in the request | no |
| 505 HTTP Version Not Supported | well-formed version other than 1.x | no |

**Order of checks** for a well-framed request, where the first failure decides the status: Host (400) → path (404) → method (405) → operands (400) → arithmetic (400) → 200. So `POST /pow` is 404 (the resource doesn't exist), and `POST /add` without Host is 400 (a protocol rule comes before routing).

## 7. Limits

| Limit | Value | Reason |
|---|---|---|
| `MAX_HEADER_BYTES` | 8 KiB | the largest real request here is ~100 bytes; matches common defaults (nginx, Apache) |
| `MAX_HEADER_COUNT` | 100 | Apache's `LimitRequestFields` default |
| `MAX_BODY_BYTES` | 64 KiB | bodies are unused but must be read to stay in sync, so reading them is bounded |
| `MAX_REQUEST_BYTES` | 72 KiB (head + body) | a connection never buffers more unparsed input than this |
| `IDLE_TIMEOUT` | 30 s (`--idle-timeout`) | enough time for a human typing into `nc`; dead clients don't hold a slot forever |
| linger timeout | 2 s / 256 KiB | cap on waiting for the client's FIN during a close |
| output high-water mark | 64 KiB | stop reading and parsing while this many response bytes wait for a slow reader |
| `MAX_CONNECTIONS` | 64 (`--max-connections`) | further clients wait in the kernel's listen backlog |

All limits are defined in one place: [`include/calc/config.hpp`](../include/calc/config.hpp).
