# calc_server: a calculator that stays on the same TCP connection

A calculator server written from scratch in C++17 on raw POSIX sockets. There is no web framework, HTTP library, or networking library. It speaks a small HTTP/1.1-style protocol and keeps each TCP connection open, so a client can send any number of requests, sequentially or **pipelined**, over **one** connection. It parses the byte stream itself: it finds where each request ends, consumes **exactly** `Content-Length` body bytes, and frames every response with an exact `Content-Length`.

```
GET /add?a=2&b=3   -> 200  5
GET /sub?a=10&b=4  -> 200  6
GET /mul?a=6&b=7   -> 200  42
GET /div?a=1&b=0   -> 400  division by zero
GET /pow?a=2&b=8   -> 404  unknown operation
POST /add          -> 405  (Allow: GET)
GET /add (no Host) -> 400  missing Host header
```

## Quick start for evaluators

```sh
make          # build: any C++17 compiler, no third-party dependencies
make demo     # ONE TCP connection, 7 requests + 1 probe, socket verified still open
make test     # 49 unit test cases + 46 integration tests against the real binary
```

Requirements: macOS or Linux, a C++17 compiler (clang++ or g++), `make`, and `python3` (standard library only, for the integration tests).

---

## Contents

1. [What the project does](#1-what-the-project-does)
2. [Why it is not a normal HTTP framework server](#2-why-it-is-not-a-normal-http-framework-server)
3. [Architecture](#3-architecture)
4. [Protocol format](#4-protocol-format)
5. [Request examples](#5-request-examples)
6. [Response examples](#6-response-examples)
7. [Build](#7-build)
8. [Run](#8-run)
9. [Test](#9-test)
10. [Persistent connections](#10-persistent-connections)
11. [Request framing](#11-request-framing)
12. [Pipelining](#12-pipelining)
13. [Status codes](#13-status-codes)
14. [Design decisions](#14-design-decisions)
15. [Limitations](#15-limitations)
16. [Example evaluator session](#16-example-evaluator-session)

Deeper documentation: [docs/protocol.md](docs/protocol.md) (the protocol specification), [docs/network-trace.md](docs/network-trace.md) (annotated byte-level traces captured from real runs), and [docs/design.md](docs/design.md) (internals: event loop, state machine, buffers, error handling).

---

## 1. What the project does

- Listens on a TCP port (default `127.0.0.1:8080`) and serves `/add`, `/sub`, `/mul`, and `/div` on 64-bit signed integers, with overflow detection.
- **Keeps every connection open** after each response (HTTP/1.1 keep-alive). One connection can carry any number of requests.
- Correctly splits the incoming byte stream into requests, however TCP groups the bytes: half a request per `recv()`, one request, or several pipelined requests in a single `recv()`.
- Returns a status code with a precise meaning for every situation (200, 400, 404, 405, 408, 413, 431, 500, 501, 505) and decides per case whether the connection can stay open.
- Handles the real-world TCP cases: partial writes, slow readers (backpressure), half-closed and reset connections, idle timeouts, and a staged ("lingering") close so that closing the connection never puts the last response at risk.

## 2. Why it is not a normal HTTP framework server

Nothing between the socket and the calculator is borrowed. The server makes these system calls itself:

| System call | Where | Purpose |
|---|---|---|
| `socket`, `setsockopt(SO_REUSEADDR)`, `bind`, `listen` | [`create_listening_socket`](src/socket_utils.cpp) | create the listening socket |
| `poll` | [`Server::run`](src/server.cpp) | one event loop for the listener and every connection |
| `accept` | [`Server::accept_new_connections`](src/server.cpp) | take completed handshakes off the backlog |
| `fcntl(O_NONBLOCK)`, `setsockopt(TCP_NODELAY)` | [`socket_utils.cpp`](src/socket_utils.cpp) | non-blocking sockets; no Nagle delay on responses |
| `recv` | [`Connection::read_from_socket`](src/connection.cpp) | read whatever bytes have arrived |
| `send` | [`Connection::flush_output`](src/connection.cpp) | write responses, resuming after short writes |
| `shutdown(SHUT_WR)`, `close` | [`Connection::begin_lingering_close`](src/connection.cpp), [`FileDescriptor`](include/calc/file_descriptor.hpp) | send FIN after the final response, then close |

The request parser ([`src/request_parser.cpp`](src/request_parser.cpp)), router, and response serializer are all hand-written. The complete list of headers the server includes is the C++ standard library plus `<sys/socket.h> <netinet/in.h> <netinet/tcp.h> <arpa/inet.h> <netdb.h> <poll.h> <fcntl.h> <unistd.h>`. You can check with `grep -rh '#include <' src include | sort -u`.

## 3. Architecture

```
 TCP client(s)
     │
     │  one persistent TCP connection per client
     ▼
 ┌──────────────────────────┐  socket/bind/listen, then poll() loop; accept()
 │ Server   (server.cpp)    │  one thread, non-blocking sockets, per-connection timers
 └────────────┬─────────────┘
              ▼
 ┌──────────────────────────┐  recv() -> parser -> router -> output buffer -> send()
 │ Connection               │  keep-alive loop, backpressure, Connection: close,
 │ (connection.cpp)         │  lingering close, idle timeout / 408
 └────────────┬─────────────┘
              ▼
 ┌──────────────────────────┐  byte-stream buffer; request line + headers + blank line,
 │ RequestParser            │  then EXACTLY Content-Length body bytes; leftover bytes
 │ (request_parser.cpp)     │  stay buffered as the start of the next request
 └────────────┬─────────────┘
              ▼
 ┌──────────────────────────┐  Host check -> route (404) -> method (405)
 │ Router (router.cpp)      │  -> operands (400) -> overflow-checked arithmetic
 │ + calculator, query      │
 └────────────┬─────────────┘
              ▼
 ┌──────────────────────────┐  "HTTP/1.1 200 OK", exact Content-Length,
 │ Response (response.cpp)  │  Connection: keep-alive | close
 └────────────┬─────────────┘
              ▼
       same TCP connection
```

The parser, router, calculator, and serializer never touch a socket, so they are unit-tested on plain bytes. All I/O is in `Server` and `Connection`. Every limit and timeout is defined in [`include/calc/config.hpp`](include/calc/config.hpp). The full module map is in [docs/design.md](docs/design.md#1-module-map).

## 4. Protocol format

**Request**: CRLF line endings (a bare LF is also accepted, so requests typed into `nc` work):

```
GET /add?a=2&b=3 HTTP/1.1\r\n        request line: METHOD SP /path?query SP HTTP/1.1
Host: localhost:8080\r\n             required for HTTP/1.1
\r\n                                 blank line: end of head
[exactly Content-Length body bytes, if a Content-Length header is present]
```

**Response**:

```
HTTP/1.1 200 OK\r\n
Content-Length: 2\r\n                exact body size in BYTES
Content-Type: text/plain\r\n
Connection: keep-alive\r\n           or "close" on the last response
X-Connection-Id: 1\r\n               which accepted TCP connection answered
X-Request-Number: 1\r\n              position of this response on that connection
\r\n
5\n                                  body
```

`X-Connection-Id` and `X-Request-Number` are evidence headers. N responses that share one connection id and count 1..N show, from the server's side, that one TCP connection carried all N requests. The operands `a` and `b` must each appear exactly once and match `-?[0-9]+` within the int64 range. Division truncates toward zero. The full grammar, framing rules, and limits are in [docs/protocol.md](docs/protocol.md).

## 5. Request examples

```
GET /add?a=2&b=3 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n                  -> 200 "5"
GET /div?a=-7&b=2 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n                 -> 200 "-3"
GET /add?a=%2D5&b=1 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n               -> 200 "-4" (percent-decoded)
GET /add?a=1&a=2&b=3 HTTP/1.1\r\nHost: x\r\n\r\n                          -> 400 duplicate parameter
GET /add?a=9223372036854775807&b=1 HTTP/1.1\r\nHost: x\r\n\r\n            -> 400 integer overflow
POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: 7\r\n\r\na=2&b=3        -> 405, body consumed exactly
GET /add?a=1&b=2 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n          -> 200 "3", then server closes
GET /add?a=1&b=2 HTTP/1.0\r\n\r\n                                          -> 200 "3", closes (HTTP/1.0 default)
```

## 6. Response examples

```
HTTP/1.1 200 OK\r\n                      HTTP/1.1 405 Method Not Allowed\r\n
Content-Length: 3\r\n                    Content-Length: 33\r\n
Content-Type: text/plain\r\n             Content-Type: text/plain\r\n
Connection: keep-alive\r\n               Connection: keep-alive\r\n
X-Connection-Id: 1\r\n                   Allow: GET\r\n
X-Request-Number: 3\r\n                  X-Connection-Id: 1\r\n
\r\n                                     X-Request-Number: 6\r\n
42\n                                     \r\n
                                         method POST not allowed; use GET\n

HTTP/1.1 400 Bad Request\r\n             (a framing error: the server answers, then closes)
Content-Length: 71\r\n
Content-Type: text/plain\r\n
Connection: close\r\n
...
invalid Content-Length 'seven': must be a non-negative decimal integer\n
```

## 7. Build

```sh
make                 # build/calc_server, build/demo_client, build/unit_tests
make CXX=g++         # choose a compiler
make clean
```

The build uses `-std=c++17 -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast` and compiles with **zero warnings** on Apple clang 17. There is no CMake and there are no external dependencies.

## 8. Run

```sh
make run                                          # 127.0.0.1:8080
build/calc_server --port 9000 --idle-timeout 60   # options below
build/calc_server --verbose                       # log every recv()/send() with byte counts
```

| Option | Default | Meaning |
|---|---|---|
| `--host HOST` | `127.0.0.1` | address to listen on (`0.0.0.0` for all interfaces) |
| `--port PORT` | `8080` | TCP port; `0` picks a free port, which is printed at startup |
| `--idle-timeout S` | `30` | close a keep-alive connection after S seconds without progress (fractions allowed) |
| `--max-connections N` | `64` | further clients wait in the kernel's listen backlog |
| `--quiet` / `--verbose` | info | startup and shutdown only / also every `recv()` and `send()` |

Stop with **Ctrl-C** (SIGINT) or SIGTERM. The server stops accepting, closes its connections, and exits with status 0. The log goes to stderr: one line per connection event and per request, including the request's exact byte range in the stream:

```
[conn 1] accepted connection from 127.0.0.1:62930 (1 open)
[conn 1] #6 POST /add HTTP/1.1 -> 405 Method Not Allowed | stream bytes [261,332) = 64 head + 7 body | keep-alive
[conn 1] closed connection from 127.0.0.1:62930: client closed the connection | requests=8 recv_calls=8 bytes_in=414 send_calls=8 bytes_out=1249 short_sends=0 blocked_sends=0
```

## 9. Test

```sh
make test            # everything below (about 10 s)
make unit-test       # C++ unit tests: parser, router/calculator, response, Connection over real loopback TCP
make integration-test  # Python (stdlib only) against the real server binary over real sockets
make mutation-check  # plants 11 protocol bugs one at a time; the unit tests must catch each one
make sanitize        # all tests under AddressSanitizer + UBSan (see the macOS note below)
```

**Unit tests (49 cases)**, in [`tests/test_parser.cpp`](tests/test_parser.cpp), [`tests/test_router.cpp`](tests/test_router.cpp), and [`tests/test_connection.cpp`](tests/test_connection.cpp). The connection tests create a real TCP connection over loopback in-process (listen, connect, accept) and control exactly which bytes sit in the kernel buffers. That makes "three requests in one `recv()`", "one request over 46 `recv()` calls", and "short writes" **deterministic** assertions, not timing luck.

**Integration tests (46)**, in [`tests/integration_test.py`](tests/integration_test.py). The first 21 follow the assignment's required list in order:

| # | Required behaviour | Test |
|---|---|---|
| 1–4 | add, subtract, multiply, normal divide | `test_01` … `test_04` |
| 5 | divide by zero → 400, connection stays open | `test_05_divide_by_zero` |
| 6 | unknown operation → 404 | `test_06_unknown_operation` |
| 7 | unsupported method → 405 + `Allow: GET` | `test_07_unsupported_method` |
| 8 | missing Host → 400 | `test_08_missing_host` |
| 9 | malformed request line → 400 + close | `test_09_malformed_request_line` |
| 10–12 | missing / invalid / duplicate query parameter → 400 | `test_10` … `test_12` |
| 13 | one request per connection | `test_13_one_request_per_connection` |
| 14 | **multiple requests over ONE connection, socket still open** | `test_14_multiple_requests_over_one_connection` |
| 15 | multiple requests in one `recv()` (the server's stats show `recv_calls=1`) | `test_15_multiple_requests_in_one_recv` |
| 16 | one request fragmented across many `recv()` calls | `test_16_fragmented_request_across_multiple_recv_calls` |
| 17 | pipelined requests, responses in order (plus a 500-request mixed pipeline) | `test_17_pipelined_requests` |
| 18 | `Connection: close` (and requests after it are ignored) | `test_18_connection_close` |
| 19 | malformed `Content-Length` → 400 + close | `test_19_malformed_content_length` |
| 20 | client disconnect: FIN, half request, RST, RST mid-response | `test_20_client_disconnect` |
| 21 | partial writes: 50,000 pipelined requests (~6 MB of responses) to a slow reader | `test_21_partial_send_write_handling` |

After these come request bodies (including a body that *looks like* a request, and a `HEAD` response that must have no body on the wire), limits (413/431/501/505), HTTP/1.0 keep-alive rules, half-close, lingering close, idle timeout and 408, the connection limit, concurrent clients, a seeded fuzz test, and SIGINT shutdown.

`test_14` proves connection reuse from both ends. On the client side, the same socket and the same local port are used throughout. On the server side, all seven responses carry `X-Connection-Id: 1` with `X-Request-Number` 1…7, and the server log contains exactly **one** `accepted connection` line. A non-blocking `MSG_PEEK` then shows no FIN or RST is pending, and one more request on the same socket succeeds.

> **macOS note on `make sanitize`:** on the development machine (Apple clang 17, macOS 26) the AddressSanitizer runtime hangs at startup even for a hello-world program, which is a toolchain problem. There, `make sanitize SANITIZERS=undefined` runs everything cleanly under UBSan. Heap checking was done with Apple's Guard Malloc (`DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib`) and `leaks`, which reported no errors and 0 leaks. On Linux the default `make sanitize` (ASan + UBSan) is expected to work.

## 10. Persistent connections

TCP has no notion of messages at all. HTTP/1.0 marked the end of a response by closing the connection, which costs a new TCP handshake for every request. Here, as in HTTP/1.1, **the connection is persistent by default** (RFC 9112 §9.3):

- After writing a response, the connection goes back to reading the **same socket**. `Connection::advance` loops "respond to buffered requests → write output" and never closes on its own after a normal response.
- The response always says `Connection: keep-alive`, except on the final response.
- The connection closes only when:
  - the client sends `Connection: close` (the response says `Connection: close`, and anything pipelined after it is ignored, per RFC 9112 §9.6);
  - it is an HTTP/1.0 request without `Connection: keep-alive`;
  - the client closes (FIN), after everything it sent has been answered;
  - a framing error or internal error occurs;
  - it has been idle for 30 s (with a 408 if a request was half-received).
- Application errors (400 bad operands, `/0`, missing Host; 404; 405) **do not** close the connection. The request was framed correctly, so the server knows exactly where the next one begins.
- When the server does close, it sends FIN with `shutdown(SHUT_WR)` after the final response and reads and discards whatever the client still sends until the client closes. An immediate `close()` with unread input would turn the FIN into an RST, which RFC 9112 §9.6 warns can erase the final response before the client reads it.

## 11. Request framing

TCP delivers a byte stream, so the server has to find message boundaries itself. It never assumes that one `recv()` is one request, or that it contains one complete header ([`RequestParser`](include/calc/request_parser.hpp)):

1. Every `recv()` result is **appended** to a per-connection buffer.
2. The **head** ends at the first empty line (`\r\n\r\n`). Until it arrives, the parser reports *incomplete* and waits for more bytes (up to 8 KiB, then 431).
3. **No `Content-Length`** → no body; the request ends right after the blank line.
4. **`Content-Length: N`** → the body is **exactly the next N bytes**. If fewer are buffered, the parser remembers the parsed head and waits. When N are available it takes exactly N, **never byte N+1**.
5. Everything after the request **stays in the buffer** as the start of the next request.
6. Anything that makes the boundary ambiguous is rejected and the connection is closed: a non-numeric, signed, or listed `Content-Length`, duplicate `Content-Length`, `Transfer-Encoding`, or whitespace before a colon. Guessing there is how request smuggling happens.

A POST whose 7-byte body is followed in the same packet by a GET is parsed as `[0,70) = 63 head + 7 body`, then `[70,121)` for the GET. This is Trace 2 in [docs/network-trace.md](docs/network-trace.md#trace-2-content-length-decides-where-the-body-ends). The unit tests split a 3-request stream at every possible byte position and feed requests one byte at a time. They also check that a body containing the bytes of a complete GET request stays a body.

Responses use the same principle in reverse: `Content-Length` is always the exact byte count of the body, so a client knows where each response ends without the connection closing. The one exception is a response to `HEAD`, which by definition has no body on the wire (RFC 9112 §6.3), so the server sends none.

## 12. Pipelining

A client may send several requests without waiting for responses (RFC 9112 §9.3.2):

- After each `recv()`, the connection calls `parser.next()` **in a loop** until no complete request remains, so three requests in one read produce three responses.
- Responses are appended to one output buffer in parse order and so leave **in request order**. Often several go out in a single `send()`: in Trace 3, one `recv()` of 154 bytes turns into one `send()` of 394 bytes.
- **Backpressure**: if a client pipelines but doesn't read, unsent responses grow until they reach 64 KiB. The connection then stops parsing and stops reading. TCP flow control pushes back on the client until it reads, which bounds memory.
- A request with `Connection: close` in a pipeline is answered, and the requests after it are not processed.

`make demo` runs the seven assignment requests once sequentially and once pipelined in a single write.

## 13. Status codes

| Status | When | Connection |
|---|---|---|
| **200 OK** | arithmetic succeeded; body is the result | stays open |
| **400 Bad Request** | well-framed request with bad content: missing/duplicate/invalid **Host** (HTTP/1.1 requires one, RFC 9112 §3.2); missing, empty, duplicate, unknown, or non-integer operand; out-of-range literal; **division by zero**; overflow | stays open |
| **400 Bad Request** | framing or syntax error: malformed request line or header, invalid or duplicate `Content-Length`, `Transfer-Encoding` together with `Content-Length` | **closed** (the next request's start is unknown) |
| **404 Not Found** | path is not `/add`, `/sub`, `/mul`, or `/div` (e.g. `/pow`) | stays open |
| **405 Method Not Allowed** | known path, method other than `GET`; response includes `Allow: GET` (RFC 9110 §15.5.6) | stays open |
| **408 Request Timeout** | a request was started but not completed within the idle timeout | closed |
| **413 Content Too Large** | `Content-Length` > 64 KiB | closed |
| **431 Request Header Fields Too Large** | head > 8 KiB or > 100 header lines | closed |
| **500 Internal Server Error** | unexpected exception while handling a request (tested with an injected faulty handler) | closed |
| **501 Not Implemented** | request uses `Transfer-Encoding` (RFC 9112 §6.1) | closed |
| **505 HTTP Version Not Supported** | well-formed version other than HTTP/1.x | closed |

Checks run in this order: **Host → path → method → operands → arithmetic**. So `POST /pow` is 404 (no such resource to list methods for), and `POST /add` without Host is 400 (a protocol rule comes before routing). Division by zero and overflow are 400, not 500, because the input is at fault, not the server.

## 14. Design decisions

- **One thread, `poll()`, non-blocking sockets.** Many clients are served at once, so an idle keep-alive client never blocks anyone, and there are no threads, locks, or shared state. Partial writes are explicit (an output buffer plus `POLLOUT`), not hidden inside a blocking `send()`. See [docs/design.md §2](docs/design.md#2-concurrency-model-one-thread-poll-non-blocking-sockets).
- **Strict on framing, lenient where it is harmless.** Anything that affects where a message ends is rejected rather than guessed. RFC-permitted variations (bare LF, blank lines before a request, `HTTP/1.2` handled as 1.1) are accepted.
- **Connections survive application errors, but not framing errors.** Once framing is lost, continuing would mean guessing.
- **Lingering close.** Half-close with `shutdown(SHUT_WR)`, then drain, so an RST does not endanger the last response.
- **Bounded everything.** Input ≤ 72 KiB, output paused at 64 KiB, at most 64 connections, idle timeout 30 s, and a cap on bytes discarded while closing. Values and reasons are in [`config.hpp`](include/calc/config.hpp).
- **int64 with explicit overflow errors**, using `__builtin_*_overflow`. Results are exact or rejected, never wrapped. Division truncates toward zero.
- **Evidence headers and byte-range logging**, so connection reuse and request boundaries can be observed without a packet capture.
- **No dependencies**, including a tiny test harness (`tests/test_framework.hpp`) instead of GoogleTest, so `make test` works on a fresh machine.
- **POSIX details handled**: EINTR retries, SIGPIPE ignored plus `MSG_NOSIGNAL`, `SO_REUSEADDR`, `TCP_NODELAY`, explicit `O_NONBLOCK` on accepted sockets (Linux does not inherit it), EMFILE back-off, and `POLLHUP` treated as readable (macOS reports a client's *half*-close as `POLLHUP`).

## 15. Limitations

These are deliberate, to keep the protocol small:

- **Not a general HTTP server.** Only `GET` on four paths. `HEAD` gets a 405 (sent correctly, with no body on the wire). There are no `OPTIONS`, absolute-form (`GET http://host/...`), or `*` targets, no `Expect: 100-continue` support (a client that sends it waits for its own timeout, about 1 s in curl, before sending the body), and no `Date` or `Server` headers.
- **No `Transfer-Encoding: chunked`** for request bodies. Such requests get 501 and the connection is closed. Responses never use it either.
- **No TLS**: plain TCP only.
- **Integers only.** No floating point; division truncates.
- **Slow-sender (slowloris) mitigation is basic**: bounded buffers, the idle timeout, and the connection cap. A client sending one byte every 29 s keeps its slot. A total per-request deadline would fix this.
- **`poll()` scales linearly** with the number of connections. It is fine for the 64-connection default; thousands of connections would call for `epoll` or `kqueue`.
- **Tested on macOS** (Apple clang 17, arm64). The code uses only portable POSIX APIs and handles the known Linux differences, but it has not been run on Linux in this environment.

## 16. Example evaluator session

**`make demo`**, sequential part (real output). The pipelined run and the server log follow it in the actual output:

```
connect() called exactly once: local 127.0.0.1:62930 -> server 127.0.0.1:62928 (socket fd 3)
mode: sequential - send a request, read its response, send the next

 #  request                    expect  got  response body                       conn-id  req#  result
 1  GET /add?a=2&b=3           200     200  5                                   1        1     PASS
 2  GET /sub?a=10&b=4          200     200  6                                   1        2     PASS
 3  GET /mul?a=6&b=7           200     200  42                                  1        3     PASS
 4  GET /div?a=1&b=0           400     400  division by zero                    1        4     PASS
 5  GET /pow?a=2&b=8           404     404  unknown operation '/pow'; suppor... 1        5     PASS
 6  POST /add (7-byte body)    405     405  method POST not allowed; use GET    1        6     PASS
 7  GET /add (no Host header)  400     400  missing Host header (required in... 1        7     PASS

same connection?
  client side: same socket fd 3, local endpoint 127.0.0.1:62930 before and 127.0.0.1:62930 after (unchanged)
  server side: every response has X-Connection-Id 1 and X-Request-Number counts 1..7 in order
socket still open after 7 requests? YES (poll(): no FIN, RST or data pending)
liveness probe on the same socket: GET /div?a=84&b=2 -> 200 42 (X-Connection-Id 1, X-Request-Number 8)

RESULT: PASS - 8 requests, 8 responses, 1 TCP connection, socket still open
```

`./scripts/demo.sh --show-bytes` also prints every request and response as escaped wire bytes.

**By hand with `nc`.** Type the requests and press Enter twice after each one. The connection stays open between them:

```
$ nc localhost 8080
GET /add?a=2&b=3 HTTP/1.1
Host: localhost

HTTP/1.1 200 OK
Content-Length: 2
Content-Type: text/plain
Connection: keep-alive
X-Connection-Id: 1
X-Request-Number: 1

5
GET /div?a=1&b=0 HTTP/1.1
Host: localhost

HTTP/1.1 400 Bad Request
Content-Length: 17
...
X-Connection-Id: 1
X-Request-Number: 2

division by zero
```

**With `curl`**, which reuses one connection for several URLs:

```
$ curl -sv 'http://127.0.0.1:8080/add?a=2&b=3' 'http://127.0.0.1:8080/mul?a=6&b=7' 2>&1 | grep -E 'Connected|Re-using|X-Conn|X-Req|^[0-9]'
* Connected to 127.0.0.1 (127.0.0.1) port 8080
< X-Connection-Id: 1
< X-Request-Number: 1
5
* Re-using existing connection with host 127.0.0.1
< X-Connection-Id: 1
< X-Request-Number: 2
42
```

---

## Repository layout

```
include/calc/        headers: config (limits), file_descriptor (RAII), socket_utils, request,
                     request_parser, response, calculator, query, router, connection, server, logger
src/                 implementation of the above + main.cpp (CLI, signals)
tools/demo_client.cpp  evaluator demo: ONE connect(), 7 requests, Content-Length response framing
tests/               unit tests (C++, tiny built-in harness) + integration_test.py (Python stdlib)
scripts/demo.sh      `make demo`: start server, run demo sequential + pipelined, show server log
scripts/mutation_check.py  `make mutation-check`: prove the tests catch planted protocol bugs
docs/                protocol.md, network-trace.md, design.md
Makefile
```
