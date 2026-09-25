# Annotated network traces

Every byte and log line below was **captured from real runs** of `calc_server --verbose` on port 8080, not written by hand. The server's `--verbose` log records each `recv()`/`send()` and the exact stream byte range of every request, so it shows what happened at the socket level.

Notation: `\r\n` is CRLF (2 bytes), and `[a,b)` means stream bytes a through b−1, counted from the first byte the client sent on that connection.

To reproduce: `build/calc_server --verbose` in one terminal, and `build/demo_client --show-bytes` (or `--pipeline --show-bytes`) in another.

---

## Trace 1: two requests on the same TCP connection

```
Client -> Server   (one send, 51 bytes)
  offset
       0  GET /add?a=2&b=3 HTTP/1.1\r\n
      27  Host: localhost:8080\r\n
      49  \r\n                                <- blank line: request #1 ends at byte 51
                                                 (no Content-Length, so no body)

Server -> Client   (one send, 131 bytes)
       0  HTTP/1.1 200 OK\r\n
      17  Content-Length: 2\r\n               <- the body is exactly 2 bytes
      36  Content-Type: text/plain\r\n
      62  Connection: keep-alive\r\n          <- the server will keep reading this socket
      86  X-Connection-Id: 1\r\n              <- 1st connection this server accepted
     106  X-Request-Number: 1\r\n             <- 1st response on it
     127  \r\n                                <- response head ends at byte 129
     129  5\n                                 <- body [129,131); the response ends at byte 131

Client -> Server   (same socket, 52 bytes)
      51  GET /sub?a=10&b=4 HTTP/1.1\r\n      <- request #2 begins at stream byte 51
      79  Host: localhost:8080\r\n
     101  \r\n                                <- request #2 ends at byte 103

Server -> Client   (131 bytes)
          HTTP/1.1 200 OK\r\n ... X-Connection-Id: 1\r\nX-Request-Number: 2\r\n\r\n6\n
```

Server log for this connection, every socket call in order:

```
[conn 1] accepted connection from 127.0.0.1:62949 (1 open)
[conn 1] recv() -> 51 bytes (51 unparsed bytes buffered)
[conn 1] #1 GET /add?a=2&b=3 HTTP/1.1 -> 200 OK | stream bytes [0,51) = 51 head + 0 body | keep-alive
[conn 1] send() -> 131 bytes
[conn 1] recv() -> 52 bytes (52 unparsed bytes buffered)
[conn 1] #2 GET /sub?a=10&b=4 HTTP/1.1 -> 200 OK | stream bytes [51,103) = 52 head + 0 body | keep-alive
[conn 1] send() -> 131 bytes
[conn 1] recv() -> 0: client finished sending (FIN)
[conn 1] closed connection from 127.0.0.1:62949: client closed the connection | requests=2 recv_calls=2 bytes_in=103 send_calls=2 bytes_out=262 ...
```

**Where request #1 ends.** At the first empty line, byte 51. There is no `Content-Length`, so the body is empty (RFC 9112 §6.3). The parser removes exactly bytes `[0,51)` from its buffer.

**Where request #2 begins.** At stream byte 51, the next byte the client sent. Request #2 starts exactly where #1 ended.

**How the client knows response #1 is complete.** From `Content-Length: 2`: after the blank line it reads exactly 2 bytes (`5\n`) and stops. It does **not** wait for the connection to close. Content-Length is the length-delimited framing that makes persistent connections possible.

**Why the socket is not closed.** The request is HTTP/1.1 without `Connection: close`, so the connection is persistent (RFC 9112 §9.3). After `send()` the server just goes back to `poll()`/`recv()` on the **same** file descriptor. There is only one `accepted connection` line, and both responses carry `X-Connection-Id: 1`. The connection ends only when the client closes it: `recv()` returns 0.

---

## Trace 2: Content-Length decides where the body ends

A `POST` with a 7-byte body, followed in the **same `send()`** by a `GET`:

```
Client -> Server   (one send, 121 bytes)
  offset
       0  POST /add HTTP/1.1\r\n
      20  Host: localhost:8080\r\n
      42  Content-Length: 7\r\n               <- body = exactly the next 7 bytes after the head
      61  \r\n                                <- head ends at byte 63
      63  a=2&b=3                             <- body [63,70)
      70  GET /mul?a=6&b=7 HTTP/1.1\r\n       <- byte 70 = Content-Length + 1: next request begins
      97  Host: localhost:8080\r\n
     119  \r\n                                <- request #2 ends at byte 121

Server -> Client   (one send, 323 bytes = two responses)
          HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 33\r\n...Allow: GET\r\n...\r\n\r\n
          method POST not allowed; use GET\n
          HTTP/1.1 200 OK\r\nContent-Length: 3\r\n...X-Request-Number: 2\r\n\r\n42\n
```

```
[conn 2] recv() -> 121 bytes (121 unparsed bytes buffered)
[conn 2] #1 POST /add HTTP/1.1 -> 405 Method Not Allowed | stream bytes [0,70) = 63 head + 7 body | keep-alive
[conn 2] #2 GET /mul?a=6&b=7 HTTP/1.1 -> 200 OK | stream bytes [70,121) = 51 head + 0 body | keep-alive
[conn 2] send() -> 323 bytes
```

The server takes **exactly** 7 bytes as the body, then starts parsing the next request at byte 70. An off-by-one in either direction silently corrupts the next request. Taking 6 bytes glues the stray `3` onto the next request line (`3GET /mul ...`); taking 8 eats the `G` (`ET /mul ...`). Both are syntactically valid methods, so the client would get a 405 for a method it never sent instead of `42`. Ignoring the body altogether would parse `a=2&b=3GET /mul ...` as a request line, which fails with 400 and closes the connection. The 405 does not close the connection: the request was framed correctly, so the server still knows where the next one starts.

---

## Trace 3: pipelining (three requests in one `recv()`)

```
Client -> Server   (one send, 154 bytes; three requests back to back)
  [0,51)     GET /add?a=2&b=3 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n
  [51,103)   GET /sub?a=10&b=4 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n
  [103,154)  GET /mul?a=6&b=7 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n

Server -> Client   (one send, 394 bytes; three responses, same order)
  HTTP/1.1 200 OK ... X-Request-Number: 1\r\n\r\n5\n
  HTTP/1.1 200 OK ... X-Request-Number: 2\r\n\r\n6\n
  HTTP/1.1 200 OK ... X-Request-Number: 3\r\n\r\n42\n
```

```
[conn 3] recv() -> 154 bytes (154 unparsed bytes buffered)       <- ONE read holds THREE requests
[conn 3] #1 GET /add?a=2&b=3 HTTP/1.1 -> 200 OK | stream bytes [0,51) ...
[conn 3] #2 GET /sub?a=10&b=4 HTTP/1.1 -> 200 OK | stream bytes [51,103) ...
[conn 3] #3 GET /mul?a=6&b=7 HTTP/1.1 -> 200 OK | stream bytes [103,154) ...
[conn 3] send() -> 394 bytes                                      <- all three responses in one write
[conn 3] closed ... | requests=3 recv_calls=1 bytes_in=154 send_calls=1 bytes_out=394 ...
```

**How pipelined requests are handled.** The client did not wait for response #1 before sending #2 and #3. After the single `recv()`, the connection calls `parser.next()` in a loop. Each call removes one complete request from the front of the buffer and leaves the rest. The loop stops when the buffer holds no complete request. Each response is appended to one output buffer in parse order, so responses leave **in request order** (RFC 9112 §9.3.2), and one `send()` writes all three. If the client pipelines faster than it reads, the output buffer reaches its 64 KiB high-water mark. The server then stops reading and parsing until the client catches up (backpressure), so memory stays bounded.

---

## Trace 4: one request split across two `recv()` calls, mid-body

```
Client -> Server   send #1 (66 bytes):  POST /add HTTP/1.1\r\nHost: localhost:8080\r\nContent-Length: 7\r\n\r\na=2
                                         |----------------- head: 63 bytes ------------------------| |3 of 7 body bytes|
Server -> Client   (nothing: the request is not complete yet)

Client -> Server   send #2 (56 bytes):  &b=3GET /add?a=40&b=2 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n
                                         |4 |---------- request #2 (52 bytes) ----------------------|
Server -> Client   405 ..., then 200 ... 42\n   (one send, 323 bytes)
```

```
[conn 4] recv() -> 66 bytes (66 unparsed bytes buffered)
                                                   <- head parsed (63 bytes consumed); 3 body bytes wait; no response
[conn 4] recv() -> 56 bytes (59 unparsed bytes buffered)       <- 59 = 3 leftover body bytes + 56 new
[conn 4] #1 POST /add HTTP/1.1 -> 405 Method Not Allowed | stream bytes [0,70) = 63 head + 7 body | keep-alive
[conn 4] #2 GET /add?a=40&b=2 HTTP/1.1 -> 200 OK | stream bytes [70,122) = 52 head + 0 body | keep-alive
[conn 4] send() -> 323 bytes
```

After the first read the parser has the complete head, knows the body is 7 bytes, and holds only 3. It returns "incomplete" and remembers where it is, so the head is not parsed again. The second read brings the missing 4 body bytes **and** all of request #2. The parser takes exactly 4 more bytes to finish the body and parses request #2 from what is left. No assumption holds that one `recv()` is one request, or one header.

---

## Trace 5: `Connection: close` and the closing sequence

```
Client -> Server   (one send, 173 bytes)
  [0,51)     GET /add?a=2&b=3 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n
  [51,121)   GET /mul?a=6&b=7 HTTP/1.1\r\nHost: localhost:8080\r\nConnection: close\r\n\r\n
  [121,173)  GET /sub?a=10&b=4 HTTP/1.1\r\nHost: localhost:8080\r\n\r\n       <- after "close": never processed

Server -> Client   (258 bytes, then FIN)
  HTTP/1.1 200 OK\r\n ... Connection: keep-alive\r\n ... 5\n
  HTTP/1.1 200 OK\r\n ... Connection: close\r\n ... 42\n
  <FIN>                                                                     <- recv() returns 0 at the client
```

```
[conn 5] recv() -> 173 bytes (173 unparsed bytes buffered)
[conn 5] #1 GET /add?a=2&b=3 HTTP/1.1 -> 200 OK | stream bytes [0,51) = 51 head + 0 body | keep-alive
[conn 5] #2 GET /mul?a=6&b=7 HTTP/1.1 -> 200 OK | stream bytes [51,121) = 70 head + 0 body | close
[conn 5] send() -> 258 bytes
[conn 5] final response sent; shutdown(SHUT_WR) queued FIN, discarding input until the client closes
[conn 5] closed connection from 127.0.0.1:62953: closed after final response | requests=2 ...
```

1. Request #2 carries `Connection: close`, so its response says `Connection: close` and is the last one. Request #3 is **not processed** (RFC 9112 §9.6).
2. Once the final response has been handed to the kernel, the server calls `shutdown(SHUT_WR)`. That sends a FIN **after** the response bytes, so the client reads both responses and then sees end-of-stream.
3. The server does **not** `close()` yet. Request #3's 52 bytes are still unread in its receive buffer. Closing a socket with unread data makes the kernel send an RST instead of a FIN, and RFC 9112 §9.6 warns that the RST *might* erase the client's unread data, including the final response. So the server keeps reading and discarding (a "lingering close") until the client closes too, capped at 2 s. Only then does it `close()`.

The unit test `lingering close delivers the final response despite unread input` and the mutation check both show this matters. With an immediate `close()` instead, the connection ends in an RST: the client's `recv()` fails with `ECONNRESET` instead of seeing a clean end-of-stream.

---

## Trace 6: a framing error ends the connection

```
Client -> Server   POST /add HTTP/1.1\r\nHost: localhost:8080\r\nContent-Length: seven\r\n\r\n

Server -> Client   HTTP/1.1 400 Bad Request\r\nContent-Length: 71\r\nContent-Type: text/plain\r\nConnection: close\r\n
                   X-Connection-Id: 6\r\nX-Request-Number: 1\r\n\r\n
                   invalid Content-Length 'seven': must be a non-negative decimal integer\n
                   <FIN>
```

```
[conn 6] #1 unparseable request -> 400 Bad Request (invalid Content-Length 'seven': must be a non-negative decimal integer) | close
[conn 6] final response sent; shutdown(SHUT_WR) queued FIN, discarding input until the client closes
```

With an invalid `Content-Length` there is no way to know where this request's body ends, and so no way to know where the next request begins. Guessing would risk running bytes from a body as a request (request smuggling). The server answers 400 and closes. Compare the 400 for `/div?a=1&b=0` in `make demo`: that request is framed correctly and only its *content* is bad, so the connection stays open and the next request is served.
