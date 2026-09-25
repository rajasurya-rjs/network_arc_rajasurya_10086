# Design and internals

This document explains how the server is built and why. For the wire protocol see [protocol.md](protocol.md); for byte-level examples see [network-trace.md](network-trace.md).

## 1. Module map

| File | Responsibility | Socket calls |
|---|---|---|
| [`src/main.cpp`](../src/main.cpp) | command-line options, signal handling (SIGINT/SIGTERM → stop flag, SIGPIPE ignored) | `sigaction` |
| [`src/socket_utils.cpp`](../src/socket_utils.cpp) | create the listening socket, non-blocking mode, `TCP_NODELAY`, address formatting | `getaddrinfo` `socket` `setsockopt` `bind` `listen` `fcntl` |
| [`src/server.cpp`](../src/server.cpp) | the `poll()` event loop; accepting connections; timers; shutdown | `poll` `accept` |
| [`src/connection.cpp`](../src/connection.cpp) | one TCP connection: read → parse → respond → write, keep-alive, backpressure, closing | `recv` `send` `shutdown` `close` |
| [`src/request_parser.cpp`](../src/request_parser.cpp) | turns the byte stream back into requests (framing) | none (pure) |
| [`src/request.cpp`](../src/request.cpp) | request representation, keep-alive rule | none |
| [`src/router.cpp`](../src/router.cpp) | HTTP semantics: Host, routing, methods, operands → status codes | none |
| [`src/calculator.cpp`](../src/calculator.cpp) / [`src/query.cpp`](../src/query.cpp) | overflow-checked int64 arithmetic; query-string and percent decoding | none |
| [`src/response.cpp`](../src/response.cpp) | exact response bytes, including `Content-Length` | none |
| [`include/calc/config.hpp`](../include/calc/config.hpp) | every limit and timeout, with the reason for its value | — |

The parser, router, calculator and serializer never touch a socket, so they can be unit-tested on plain byte strings. All I/O lives in `Server` and `Connection`.

## 2. Concurrency model: one thread, `poll()`, non-blocking sockets

A persistent connection spends most of its life idle, waiting for the client's next request. The design has to keep one idle client from stalling the rest. The options were:

| Model | Idle client blocks others? | Partial writes | Shared state | Verdict |
|---|---|---|---|---|
| iterative (accept, serve until close, repeat) | **yes**, the second client waits for the first to disconnect | hidden in blocking `send()` | none | too weak for keep-alive |
| thread per connection, blocking I/O | no | hidden in blocking `send()` | needs care (log, counters, shutdown) | workable, but threads for their own sake |
| **one thread + `poll()` + non-blocking I/O** | no | **explicit**: output buffer + `POLLOUT` | **none** | **chosen** |

The event loop (`Server::run`):

```
loop until SIGINT/SIGTERM:
    poll set = [listener: POLLIN if below the connection limit]
             + [each connection: the events it wants right now]
    timeout  = time until the earliest connection deadline (capped at 1 s)
    poll(...)                          EINTR -> re-check the stop flag
    for each connection: handle revents; if its deadline has passed, handle that
    drop closed connections
    if the listener is readable: accept() until EAGAIN
```

Each connection declares what it is waiting for (`Connection::poll_events`), so the loop itself stays generic. Everything runs on one thread, so there are no locks, and responses on a connection are produced in exactly the order their requests were parsed.

## 3. Connection state machine

```
                 response with "Connection: close",
                 framing error, 408, or 500
    ┌────────┐  ───────────────────────────────▶  ┌─────────┐
    │ Active │                                    │ Closing │  flush the final response
    └────────┘                                    └─────────┘
      │  ▲ read request → respond → read next           │ output empty → shutdown(SHUT_WR) (FIN)
      │  └──────────── (keep-alive loop) ──────         ▼
      │                                           ┌──────────┐
      │ client FIN + all answered,                │ Draining │  read and discard until the client's FIN,
      │ idle timeout, reset, error                └──────────┘  2 s, or 256 KiB
      ▼                                                 │
    ┌────────┐ ◀────────────────────────────────────────┘
    │ Closed │  close(fd); the server removes the connection
    └────────┘
```

**Active** is the persistent-connection state. After each response the connection keeps reading the same socket; nothing resets or reconnects. Handling a readiness event looks like this:

1. `read_from_socket`: **one** `recv()`, appending whatever arrived to the parser's buffer.
2. `advance`: alternate between *respond to every complete buffered request* and *write as much output as the kernel accepts* until neither makes progress. Each pass consumes input or sends output, so the loop terminates.
3. Lifecycle checks: the client sent FIN and everything is answered → close. Final response flushed → start the lingering close.

## 4. Buffers

Each connection owns exactly two buffers, and both are bounded.

**Input** (inside `RequestParser`): bytes received but not yet consumed as part of a request.
- `feed()` appends; `next()` removes at most one complete request from the front.
- A read offset (`start_`) marks the consumed prefix. The prefix is erased only when it makes up at least half the buffer, so a pipeline of N requests costs O(total bytes), not O(N × buffer).
- The search for the blank line resumes where it stopped (`scan_from_`), so a client sending one byte per packet costs linear work.
- Bound: the connection stops calling `recv()` once `max_request_bytes` (72 KiB) are buffered. At that size the parser is guaranteed to have a complete request or a 413/431 error, so reading pauses only until that request is handled. It cannot deadlock waiting for bytes it refuses to read.

**Output** (`Connection::output_`): serialized responses the kernel has not accepted yet, plus a sent-prefix offset compacted the same way.

## 5. Partial reads

`recv()` returns whatever bytes are available. That may be part of a request, one request, several requests, or several and a half. The connection never interprets a single read. It only feeds the parser, which knows where requests begin and end:

- **Incomplete head**: `next()` returns `Incomplete`; the bytes stay buffered.
- **Complete head, incomplete body**: the parsed head is stored (`pending_`) with the body length. Later calls wait until `Content-Length` bytes are buffered and then take **exactly** that many.
- **Several requests in one read**: `next()` is called in a loop; each call takes one request and leaves the rest.

The parser test `split at every possible byte boundary` feeds a 3-request stream split at every one of its ~200 positions. `one byte at a time` checks that each request completes exactly when its last byte arrives.

## 6. Partial writes

On a non-blocking socket, `send()` takes only as many bytes as fit in the kernel's send buffer. A short count, or `EAGAIN`, is normal when the client reads slowly or the response is large:

```
while output pending:
    n = send(fd, output + sent, pending)
    n > 0           -> sent += n        (a short write is fine; the loop tries again)
    EINTR           -> retry
    EAGAIN          -> stop; poll_events() now includes POLLOUT, resume when writable
    EPIPE/ECONNRESET-> the client is gone: close
```

The connection unit test `short writes are resumed until complete` sends a 1 MiB response through a 4 KiB send buffer and checks every byte. Integration test `test_21` pipelines 50,000 requests (~6 MB of responses, more than any kernel send buffer) to a client that does not read for 0.5 s. It checks that each response arrives intact and in order, and that the server's statistics show writes blocking.

## 7. Backpressure

A client can pipeline requests without reading any responses. With no limit, the server would buffer responses without bound. Once `output_high_water` (64 KiB) response bytes are waiting, the connection:

- stops parsing buffered requests (`respond_to_buffered_requests` checks the mark), and
- stops asking `poll()` for `POLLIN`, so it does not `recv()`.

The kernel receive buffer then fills, TCP flow control closes the window, and the client's `send()` blocks. When the client reads and `POLLOUT` lets the backlog drain below the mark, parsing and reading resume. The unit test `backpressure pauses reading until responses drain` checks that `POLLIN` really is dropped and that all 3,000 responses arrive in order afterwards.

## 8. Closing a connection properly

- **Client closes first (FIN).** `recv()` returns 0. Every complete request already buffered is still answered, because a client that half-closes (`printf ... | nc -N`) is still reading. Then the connection closes.
- **Server closes first** (`Connection: close`, framing error, 408, 500). The server flushes the final response, calls `shutdown(SHUT_WR)` to send a FIN behind it, then reads and discards until the client's FIN, 2 s, or 256 KiB (the *lingering close*, RFC 9112 §9.6). An immediate `close()` with unread client bytes makes the kernel send an RST instead of a FIN. RFC 9112 §9.6 warns that the RST *might* erase the client's unread data, including the response. Whether it does depends on the client's TCP stack. In this project's tests on macOS, an immediate `close()` makes the connection end with `ECONNRESET` rather than a clean end-of-stream, and the mutation check catches that.
- **Idle.** With no request for `idle_timeout` (30 s), the connection closes silently. If a request was half-received, it gets a 408 first.
- **Reset or error.** `ECONNRESET`, `EPIPE`, or `POLLERR` close the connection at once; the rest of the server is unaffected.

## 9. Error handling details

| Situation | Handling |
|---|---|
| `EINTR` from `poll`, `accept`, `recv`, or `send` | retried (or, for `poll`, the stop flag is re-checked) |
| `EAGAIN` / `EWOULDBLOCK` | normal on non-blocking sockets: wait for the next readiness event |
| `SIGPIPE` on writing to a reset socket | ignored process-wide; `send()` returns `EPIPE`, and `MSG_NOSIGNAL` is also used where it exists (Linux) |
| `ECONNABORTED` / `EPROTO` from `accept` | the client reset while still in the backlog: skip it |
| `EMFILE` / `ENFILE` from `accept` | stop polling the listener for 1 s instead of spinning on a permanently readable socket |
| `POLLHUP` | **handled as readable, not as "client gone".** On macOS, a client's half-close (FIN only) is reported as `POLLIN|POLLHUP` even though the client can still receive; this was verified with a test program during development. Linux reports `POLLIN|POLLRDHUP` instead. `recv()` then gives the precise answer: data, 0 (EOF), or an error. |
| `POLLERR` / `POLLNVAL` | close the connection (reason from `SO_ERROR`) |
| exception from the request handler | 500 + close for that connection only |
| `close()` returning `EINTR` | not retried: on Linux the descriptor is already released |

Accepted sockets are made non-blocking explicitly (they inherit `O_NONBLOCK` from the listener on BSD/macOS, but not on Linux). They also get `TCP_NODELAY`: the server always writes whole responses, so Nagle's algorithm never saves anything, but it can delay a pipelined response by a delayed-ACK interval.

## 10. Other decisions

- **Hand-written parser, strict where framing is concerned, lenient where it is harmless.** Anything that affects where a message ends (Content-Length syntax, duplicates, Transfer-Encoding, whitespace before a colon) is rejected, because guessing there causes request smuggling. Harmless variations the RFC allows (bare LF, leading blank lines, `HTTP/1.x` with x > 1) are accepted.
- **No `Transfer-Encoding: chunked`.** It is a third framing mechanism beyond connection-close and Content-Length, and nothing here needs it. Such requests get 501 and the connection is closed. The server can't find the end of a chunked body without decoding it, so closing is the only safe option.
- **64-bit signed integers with explicit overflow errors.** Results are exact or rejected with 400; they never wrap. Division truncates toward zero, as in C++.
- **Evidence headers** (`X-Connection-Id`, `X-Request-Number`). They make connection reuse visible from the client side, without packet capture, and they are useful for grading.
- **RAII for every descriptor** (`FileDescriptor`). No exit path can leak a socket.
- **One global**: the `volatile sig_atomic_t` stop flag, the only thing a signal handler may safely write.

## 11. Testing strategy

| Layer | Suite | What it proves |
|---|---|---|
| parser | [`tests/test_parser.cpp`](../tests/test_parser.cpp) (21 cases) | boundaries under every split point, exact Content-Length consumption, every framing error |
| application | [`tests/test_router.cpp`](../tests/test_router.cpp) (14 cases) | status code for every input class, overflow, check order, exact response bytes (including HEAD) |
| connection | [`tests/test_connection.cpp`](../tests/test_connection.cpp) (14 cases) | real loopback TCP created in-process, with **deterministic** control over kernel buffers: 3 requests in 1 `recv()`, 1 request over one `recv()` per byte, short writes, backpressure, half-close, RST, timeouts, lingering close, HEAD framing |
| whole server | [`tests/integration_test.py`](../tests/integration_test.py) (46 tests) | the real binary over real sockets: the assignment's 21 required behaviours in order, plus limits, HTTP/1.0, timeouts, the connection cap, concurrency, fuzzing, SIGINT |
| test quality | [`scripts/mutation_check.py`](../scripts/mutation_check.py) | plants 11 realistic protocol bugs one at a time (off-by-one body, ignored Content-Length, dropped short-write bytes, body sent after HEAD, no lingering close, ...); each one must fail the unit tests |

`make sanitize` runs everything under AddressSanitizer + UndefinedBehaviorSanitizer (see the README for a macOS caveat).
