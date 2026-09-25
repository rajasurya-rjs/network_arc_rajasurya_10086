#!/usr/bin/env python3
"""Integration tests for calc_server.

Starts the real server binary and talks to it over real TCP sockets, using
only the Python standard library (socket, subprocess, unittest). No HTTP
library is used on the client side either. Requests are written as raw bytes
and responses are framed by Content-Length, so each test states exactly what
is on the wire.

Tests test_01 .. test_21 follow the assignment's required test list in order.
The classes after them cover the extras (limits, timeouts, HTTP/1.0,
concurrency, lingering close, graceful shutdown, fuzzing).

    make integration-test
    CALC_SERVER=build/calc_server python3 tests/integration_test.py [-k pattern]
"""

import os
import random
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SERVER_BINARY = os.environ.get("CALC_SERVER", os.path.join(ROOT, "build", "calc_server"))


# --------------------------------------------------------------------------
# Test infrastructure
# --------------------------------------------------------------------------

class ServerProcess:
    """Runs calc_server on a free port (--port 0) and captures its log."""

    def __init__(self, *extra_args):
        self.log_file = tempfile.NamedTemporaryFile(mode="w+", prefix="calc_server_", suffix=".log", delete=False)
        self.proc = subprocess.Popen(
            [SERVER_BINARY, "--host", "127.0.0.1", "--port", "0", *extra_args],
            stdout=subprocess.DEVNULL,
            stderr=self.log_file,
        )
        self.port = self._wait_for_port()

    def _wait_for_port(self):
        deadline = time.time() + 5
        while time.time() < deadline:
            match = re.search(r"listening on 127\.0\.0\.1:(\d+)", self.log())
            if match:
                return int(match.group(1))
            if self.proc.poll() is not None:
                raise RuntimeError("server exited during startup:\n" + self.log())
            time.sleep(0.02)
        raise RuntimeError("server did not report its port:\n" + self.log())

    def log(self):
        with open(self.log_file.name) as f:
            return f.read()

    def wait_for_log(self, pattern, timeout=3.0):
        """Waits until `pattern` (a regex) appears in the log; returns the match."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            match = re.search(pattern, self.log())
            if match:
                return match
            time.sleep(0.02)
        raise AssertionError(f"log never matched {pattern!r}:\n{self.log()}")

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        """SIGINT, like Ctrl-C. Returns the exit code."""
        if self.alive():
            self.proc.send_signal(signal.SIGINT)
        try:
            return self.proc.wait(timeout=5)
        finally:
            os.unlink(self.log_file.name)


class ConnectionClosed(Exception):
    pass


class Response:
    def __init__(self, status, reason, headers, body, raw, head_only=False):
        self.status, self.reason, self.headers, self.body, self.raw = status, reason, headers, body, raw
        self.head_only = head_only  # response to HEAD: no body on the wire, whatever Content-Length says

    def header(self, name):
        for key, value in self.headers:
            if key.lower() == name.lower():
                return value
        return None

    def __repr__(self):
        return f"<Response {self.status} {self.reason} body={self.body!r}>"


class Client:
    """ONE TCP connection. Reads responses off the byte stream by
    Content-Length, like a real HTTP/1.1 client, so any number of responses
    can come back over the same socket."""

    def __init__(self, port, timeout=5.0, rcvbuf=None):
        self.timeout = timeout
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.settimeout(timeout)
        self.sock.connect(("127.0.0.1", port))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.local_address = self.sock.getsockname()
        self.buffer = b""

    def send(self, data):
        self.sock.sendall(data)

    def request(self, data):
        self.send(data)
        return self.read_response(head_only=data.startswith(b"HEAD "))

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise ConnectionClosed("server closed the connection")
        self.buffer += chunk

    def read_response(self, head_only=False):
        """head_only=True for the response to a HEAD request, which ends at its
        blank line (RFC 9112 §6.3), exactly as a real HTTP client treats it."""
        while b"\r\n\r\n" not in self.buffer:
            self._fill()
        head, _, _ = self.buffer.partition(b"\r\n\r\n")
        lines = head.decode("latin-1").split("\r\n")
        _version, status, reason = lines[0].split(" ", 2)
        headers = []
        for line in lines[1:]:
            name, _, value = line.partition(":")
            headers.append((name, value.strip()))
        length = 0 if head_only else int(next(v for k, v in headers if k.lower() == "content-length"))
        total = len(head) + 4 + length
        while len(self.buffer) < total:
            self._fill()
        raw, self.buffer = self.buffer[:total], self.buffer[total:]
        return Response(int(status), reason, headers, raw[len(head) + 4:].decode("latin-1"), raw, head_only)

    def is_open(self):
        """Non-blocking check: True unless the server has sent FIN or RST."""
        if self.buffer:
            return True
        self.sock.setblocking(False)
        try:
            return self.sock.recv(1, socket.MSG_PEEK) != b""
        except BlockingIOError:
            return True  # nothing pending: no FIN, no RST, connection open
        except ConnectionResetError:
            return False
        finally:
            self.sock.settimeout(self.timeout)

    def wait_for_eof(self, timeout=3.0):
        """True if the server closes cleanly (FIN) without sending anything more."""
        if self.buffer:
            return False
        self.sock.settimeout(timeout)
        try:
            return self.sock.recv(65536) == b""
        except (socket.timeout, ConnectionResetError):
            return False
        finally:
            self.sock.settimeout(self.timeout)

    def nothing_received_within(self, seconds):
        """True if no bytes arrive within `seconds` (used to show the server is still waiting)."""
        self.sock.settimeout(seconds)
        try:
            data = self.sock.recv(65536)
            self.buffer += data
            return False
        except socket.timeout:
            return True
        finally:
            self.sock.settimeout(self.timeout)

    def reset(self):
        """Abortive close: SO_LINGER{on, 0} makes close() send RST instead of FIN."""
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.sock.close()

    def close(self):
        self.sock.close()


def req(method="GET", target="/add?a=2&b=3", headers=(), body=b"", version="HTTP/1.1", host="localhost:8080"):
    """Builds raw request bytes. Adds Content-Length for a body unless given explicitly."""
    lines = [f"{method} {target} {version}"]
    if host is not None:
        lines.append(f"Host: {host}")
    lines += [f"{name}: {value}" for name, value in headers]
    if body and not any(name.lower() == "content-length" for name, _ in headers):
        lines.append(f"Content-Length: {len(body)}")
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1") + body


def connection_stats(log, connection_id):
    """Parses the key=value statistics the server logs when a connection closes."""
    match = re.search(rf"\[conn {connection_id}\] closed .*?\| (.*)", log)
    if not match:
        return None
    return {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", match.group(1))}


class ServerTestCase(unittest.TestCase):
    """One server per test class; every test checks the server survived."""

    server_args = ()

    @classmethod
    def setUpClass(cls):
        cls.server = ServerProcess(*cls.server_args)

    @classmethod
    def tearDownClass(cls):
        exit_code = cls.server.stop()
        if exit_code != 0:
            raise AssertionError(f"server exited with {exit_code} on SIGINT")

    def tearDown(self):
        self.assertTrue(self.server.alive(), "server process died:\n" + self.server.log()[-3000:])

    def client(self, **kwargs):
        c = Client(self.server.port, **kwargs)
        self.addCleanup(c.close)
        return c

    def assertResponse(self, response, status, body=None, connection="keep-alive"):
        self.assertEqual(response.status, status, response)
        if body is not None:
            self.assertEqual(response.body, body, response)
        self.assertEqual(response.header("Connection"), connection, response)
        self.assertEqual(response.header("Content-Type"), "text/plain")
        # Framing check: Content-Length matches the body's size in bytes.
        if not response.head_only:
            self.assertEqual(int(response.header("Content-Length")), len(response.body.encode("latin-1")))

    def assertServerStillWorks(self):
        c = self.client()
        self.assertResponse(c.request(req(target="/mul?a=6&b=7")), 200, "42\n")


# --------------------------------------------------------------------------
# The assignment's required test list, in order
# --------------------------------------------------------------------------

class RequiredBehaviour(ServerTestCase):

    # --- 1-12: calculator semantics and status codes ---------------------

    def test_01_add(self):
        self.assertResponse(self.client().request(req(target="/add?a=2&b=3")), 200, "5\n")

    def test_02_subtract(self):
        self.assertResponse(self.client().request(req(target="/sub?a=10&b=4")), 200, "6\n")

    def test_03_multiply(self):
        self.assertResponse(self.client().request(req(target="/mul?a=6&b=7")), 200, "42\n")

    def test_04_normal_divide(self):
        c = self.client()
        self.assertResponse(c.request(req(target="/div?a=84&b=2")), 200, "42\n")
        self.assertResponse(c.request(req(target="/div?a=7&b=2")), 200, "3\n")    # integer division
        self.assertResponse(c.request(req(target="/div?a=-7&b=2")), 200, "-3\n")  # truncates toward zero

    def test_05_divide_by_zero(self):
        c = self.client()
        self.assertResponse(c.request(req(target="/div?a=1&b=0")), 400, "division by zero\n")
        self.assertTrue(c.is_open(), "a 400 for bad input must not close the connection")

    def test_06_unknown_operation(self):
        response = self.client().request(req(target="/pow?a=2&b=8"))
        self.assertResponse(response, 404)
        self.assertIn("/pow", response.body)

    def test_07_unsupported_method(self):
        c = self.client()
        response = c.request(req(method="POST", target="/add"))
        self.assertResponse(response, 405)
        self.assertEqual(response.header("Allow"), "GET")
        for method in ("PUT", "DELETE", "HEAD", "PATCH"):
            self.assertResponse(c.request(req(method=method, target="/add?a=1&b=2")), 405)

    def test_08_missing_host(self):
        c = self.client()
        response = c.request(req(target="/add?a=2&b=3", host=None))
        self.assertResponse(response, 400)
        self.assertIn("Host", response.body)
        # Framing was fine, so the connection is still usable.
        self.assertResponse(c.request(req()), 200, "5\n")

    def test_09_malformed_request_line(self):
        for raw in (b"GARBAGE\r\n\r\n", b"GET /add\r\n\r\n", b"GET  /add HTTP/1.1\r\nHost: x\r\n\r\n",
                    b"GET add?a=1&b=2 HTTP/1.1\r\nHost: x\r\n\r\n", b"GET /add HTTP/1.1 extra\r\nHost: x\r\n\r\n"):
            with self.subTest(raw=raw):
                c = self.client()
                self.assertResponse(c.request(raw), 400, connection="close")
                self.assertTrue(c.wait_for_eof(), "after a framing error the server must close")

    def test_10_missing_query_parameter(self):
        c = self.client()
        self.assertResponse(c.request(req(target="/add?a=1")), 400, "missing query parameter 'b'\n")
        self.assertResponse(c.request(req(target="/add?b=1")), 400, "missing query parameter 'a'\n")
        self.assertResponse(c.request(req(target="/add")), 400, "missing query parameters 'a' and 'b'\n")
        self.assertResponse(c.request(req(target="/add?a=&b=1")), 400, "query parameter 'a' is empty\n")

    def test_11_invalid_integer(self):
        c = self.client()
        for value in ("abc", "1.5", "1e3", "0x10", "+1", "--1", "%201", "99999999999999999999"):
            with self.subTest(value=value):
                self.assertResponse(c.request(req(target=f"/add?a={value}&b=1")), 400)
        # Overflow of the signed 64-bit result is also rejected, not wrapped.
        self.assertResponse(c.request(req(target="/add?a=9223372036854775807&b=1")), 400)
        self.assertResponse(c.request(req(target="/div?a=-9223372036854775808&b=-1")), 400)

    def test_12_duplicate_query_parameter(self):
        c = self.client()
        self.assertResponse(c.request(req(target="/add?a=1&a=2&b=3")), 400, "duplicate query parameter 'a'\n")
        self.assertResponse(c.request(req(target="/add?a=1&b=2&b=3")), 400, "duplicate query parameter 'b'\n")

    # --- 13-21: TCP connection behaviour ----------------------------------

    def test_13_one_request_per_connection(self):
        # The classic non-persistent pattern still works: connect, one
        # request with "Connection: close", the server closes, repeat.
        ports = set()
        for a in range(3):
            c = self.client()
            ports.add(c.local_address[1])
            response = c.request(req(target=f"/add?a={a}&b=1", headers=[("Connection", "close")]))
            self.assertResponse(response, 200, f"{a + 1}\n", connection="close")
            self.assertTrue(c.wait_for_eof())
        self.assertEqual(len(ports), 3, "each iteration really used a new TCP connection")

    def test_14_multiple_requests_over_one_connection(self):
        # THE key requirement: ONE TCP connection carries every request and
        # response, and is still open afterwards. A dedicated server makes
        # the server log unambiguous.
        server = ServerProcess()
        try:
            c = Client(server.port)
            local_before = c.local_address
            exchanges = [
                (req(target="/add?a=2&b=3"), 200, "5\n"),
                (req(target="/sub?a=10&b=4"), 200, "6\n"),
                (req(target="/mul?a=6&b=7"), 200, "42\n"),
                (req(target="/div?a=1&b=0"), 400, None),
                (req(target="/pow?a=2&b=8"), 404, None),
                (req(method="POST", target="/add", body=b"a=2&b=3"), 405, None),
                (req(target="/add?a=2&b=3", host=None), 400, None),
            ]
            responses = []
            for raw, status, body in exchanges:
                response = c.request(raw)
                self.assertResponse(response, status, body)
                responses.append(response)

            # Client side: same socket object, same local port, never reconnected.
            self.assertEqual(c.sock.getsockname(), local_before)
            # Server side: every response came from the same accepted connection, numbered 1..7.
            self.assertEqual({r.header("X-Connection-Id") for r in responses}, {"1"})
            self.assertEqual([r.header("X-Request-Number") for r in responses], [str(i) for i in range(1, 8)])
            # The server's log shows exactly one accept() for all seven requests.
            log = server.log()
            self.assertEqual(log.count("accepted connection"), 1, log)
            self.assertIn("[conn 1] #7 ", log)
            # The socket is still open, and still works.
            self.assertTrue(c.is_open(), "connection must stay open after the responses")
            self.assertResponse(c.request(req(target="/add?a=40&b=2")), 200, "42\n")
            self.assertEqual(server.log().count("accepted connection"), 1)
            c.close()
        finally:
            self.assertEqual(server.stop(), 0)

    def test_15_multiple_requests_in_one_recv(self):
        server = ServerProcess()
        try:
            c = Client(server.port)
            # One sendall() of ~150 bytes on loopback is one TCP segment, so it
            # reaches the server's single recv() as one chunk holding 3 requests.
            c.send(req(target="/add?a=2&b=3") + req(target="/sub?a=10&b=4") + req(target="/mul?a=6&b=7"))
            self.assertEqual([c.read_response().body for _ in range(3)], ["5\n", "6\n", "42\n"])
            c.close()
            server.wait_for_log(r"\[conn 1\] closed")
            stats = connection_stats(server.log(), 1)
            self.assertEqual(stats["requests"], 3)
            self.assertEqual(stats["recv_calls"], 1, "all three requests were parsed out of a single recv()")
        finally:
            self.assertEqual(server.stop(), 0)

    def test_16_fragmented_request_across_multiple_recv_calls(self):
        server = ServerProcess()
        try:
            c = Client(server.port)
            raw = req(target="/mul?a=6&b=7")
            # Split inside the method, the target, the header name, and the final "\r\n\r\n".
            pieces = [raw[:2], raw[2:9], raw[9:30], raw[30:-3], raw[-3:-1], raw[-1:]]
            for piece in pieces[:-1]:
                c.send(piece)
                time.sleep(0.05)
            self.assertTrue(c.nothing_received_within(0.2), "server must not answer before the request is complete")
            c.send(pieces[-1])
            self.assertResponse(c.read_response(), 200, "42\n")

            # And one byte per segment.
            for byte in req(target="/add?a=20&b=22"):
                c.send(bytes([byte]))
                time.sleep(0.002)
            self.assertResponse(c.read_response(), 200, "42\n")
            c.close()
            server.wait_for_log(r"\[conn 1\] closed")
            stats = connection_stats(server.log(), 1)
            self.assertEqual(stats["requests"], 2)
            self.assertGreaterEqual(stats["recv_calls"], len(pieces) + 10, "requests really arrived in many recv() calls")
        finally:
            self.assertEqual(server.stop(), 0)

    def test_17_pipelined_requests(self):
        c = self.client()
        c.send(req(target="/add?a=2&b=3") + req(target="/sub?a=10&b=4") + req(target="/mul?a=6&b=7"))
        responses = [c.read_response() for _ in range(3)]
        self.assertEqual([r.body for r in responses], ["5\n", "6\n", "42\n"])  # same order as the requests

        # A larger mixed pipeline, including errors and a body, written by a
        # separate thread while this one reads.
        batch, expected = [], []
        for i in range(500):
            kind = i % 5
            if kind == 0:
                batch.append(req(target=f"/add?a={i}&b=0")); expected.append((200, f"{i}\n"))
            elif kind == 1:
                batch.append(req(target=f"/div?a={i}&b=0")); expected.append((400, None))
            elif kind == 2:
                batch.append(req(target=f"/nope?a={i}")); expected.append((404, None))
            elif kind == 3:
                batch.append(req(method="POST", target="/add", body=f"n={i}".encode())); expected.append((405, None))
            else:
                batch.append(req(target=f"/mul?a={i}&b=2")); expected.append((200, f"{2 * i}\n"))
        writer = threading.Thread(target=c.send, args=(b"".join(batch),))
        writer.start()
        for status, body in expected:
            self.assertResponse(c.read_response(), status, body)
        writer.join()
        self.assertTrue(c.is_open())

    def test_18_connection_close(self):
        c = self.client()
        response = c.request(req(target="/add?a=2&b=3", headers=[("Connection", "close")]))
        self.assertResponse(response, 200, "5\n", connection="close")
        self.assertTrue(c.wait_for_eof(), "server must close after a 'Connection: close' request")

        # Requests pipelined after the "close" one are not processed (RFC 9112 §9.6).
        c = self.client()
        c.send(req(target="/add?a=1&b=1") + req(target="/sub?a=10&b=4", headers=[("Connection", "close")])
               + req(target="/mul?a=6&b=7"))
        self.assertResponse(c.read_response(), 200, "2\n")
        self.assertResponse(c.read_response(), 200, "6\n", connection="close")
        self.assertTrue(c.wait_for_eof(), "no response for the request after 'Connection: close'")

    def test_19_malformed_content_length(self):
        for value in ("abc", "-1", "+7", "1.5", "7, 7", ""):
            with self.subTest(content_length=value):
                c = self.client()
                response = c.request(req(method="POST", target="/add", headers=[("Content-Length", value)]))
                self.assertResponse(response, 400, connection="close")
                self.assertTrue(c.wait_for_eof(), "request boundaries are unknowable, so the server must close")
        c = self.client()
        response = c.request(req(method="POST", target="/add",
                                 headers=[("Content-Length", "3"), ("Content-Length", "3")], body=b"abc"))
        self.assertResponse(response, 400, connection="close")
        self.assertServerStillWorks()

    def test_20_client_disconnect(self):
        # (a) connect and leave immediately
        Client(self.server.port).close()
        # (b) half a request, then FIN
        c = Client(self.server.port); c.send(b"GET /add?a=1&b=2 HTT"); c.close()
        # (c) full request, then FIN without reading the response
        c = Client(self.server.port); c.send(req()); c.close()
        # (d) half a request, then RST
        c = Client(self.server.port); c.send(b"GET /add?a=1"); time.sleep(0.05); c.reset()
        # (e) a big pipeline, then RST while the server is still writing responses
        c = Client(self.server.port, rcvbuf=4096)
        c.send(req() * 2000)
        time.sleep(0.1)
        c.reset()
        time.sleep(0.3)
        self.assertTrue(self.server.alive())
        self.assertServerStillWorks()

    def test_21_partial_send_write_handling(self):
        # 50,000 pipelined requests produce about 6 MB of responses: more
        # than any kernel send buffer holds, including Linux's auto-tuned
        # buffers (up to 4 MB). The client does not read for 0.5 s and uses a
        # small receive buffer, so the server's send() calls must hit a full
        # buffer (short writes / EAGAIN) and resume without losing,
        # duplicating or reordering a single byte.
        server = ServerProcess()
        try:
            count = 50000
            c = Client(server.port, rcvbuf=8192, timeout=30)
            payload = b"".join(req(target=f"/add?a={i}&b=0") for i in range(count))
            writer = threading.Thread(target=c.send, args=(payload,))
            writer.start()
            time.sleep(0.5)
            for i in range(count):
                response = c.read_response()
                if response.status != 200 or response.body != f"{i}\n" or response.header("X-Request-Number") != str(i + 1):
                    self.fail(f"response {i} out of order or corrupted: {response!r}")
            writer.join()
            self.assertTrue(c.is_open())
            c.close()
            server.wait_for_log(r"\[conn 1\] closed", timeout=10)
            stats = connection_stats(server.log(), 1)
            self.assertEqual(stats["requests"], count)
            self.assertGreater(stats["short_sends"] + stats["blocked_sends"], 0,
                               "expected the kernel send buffer to fill at least once")
            self.assertGreater(stats["send_calls"], 1)
        finally:
            self.assertEqual(server.stop(), 0)


# --------------------------------------------------------------------------
# Extras: request bodies, limits, HTTP/1.0, timeouts, concurrency, shutdown
# --------------------------------------------------------------------------

class RequestBodies(ServerTestCase):

    def test_body_is_consumed_exactly_and_connection_continues(self):
        c = self.client()
        c.send(req(method="POST", target="/add", body=b"a=2&b=3") + req(target="/mul?a=6&b=7"))
        self.assertResponse(c.read_response(), 405)
        self.assertResponse(c.read_response(), 200, "42\n")

    def test_body_that_looks_like_a_request_is_not_executed(self):
        c = self.client()
        smuggled = req(target="/add?a=1000&b=1000")
        c.send(req(method="POST", target="/add", body=smuggled) + req(target="/sub?a=10&b=4"))
        self.assertResponse(c.read_response(), 405)
        self.assertResponse(c.read_response(), 200, "6\n")  # not "2000": the body was never parsed as a request

    def test_server_waits_for_the_whole_body(self):
        c = self.client()
        c.send(b"POST /add HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n0123")
        self.assertTrue(c.nothing_received_within(0.3), "6 body bytes are still missing")
        c.send(b"456789" + req(target="/add?a=1&b=1"))
        self.assertResponse(c.read_response(), 405)
        self.assertResponse(c.read_response(), 200, "2\n")

    def test_head_response_has_no_body_on_the_wire(self):
        # A response to HEAD ends at its blank line (RFC 9112 §6.3). If the
        # server sent the 405 body anyway, a real client would take those
        # bytes as the start of the next response.
        c = self.client()
        c.send(req(method="HEAD", target="/add?a=2&b=3") + req(target="/mul?a=6&b=7"))
        while b"\r\n\r\n" not in c.buffer:
            c._fill()
        head, _, rest = c.buffer.partition(b"\r\n\r\n")
        self.assertTrue(head.startswith(b"HTTP/1.1 405 Method Not Allowed"), head)
        self.assertIn(b"Content-Length: ", head)  # the size the body would have had
        c.buffer = rest  # no body to skip: the next response starts right here
        self.assertResponse(c.read_response(), 200, "42\n")

    def test_get_with_body_and_zero_length_body(self):
        c = self.client()
        c.send(req(target="/add?a=2&b=3", body=b"ignored") + req(method="POST", target="/add", headers=[("Content-Length", "0")])
               + req(target="/sub?a=10&b=4"))
        self.assertResponse(c.read_response(), 200, "5\n")
        self.assertResponse(c.read_response(), 405)
        self.assertResponse(c.read_response(), 200, "6\n")


class LimitsAndFramingErrors(ServerTestCase):

    def expect_error_then_close(self, raw, status):
        c = self.client()
        c.send(raw)
        self.assertResponse(c.read_response(), status, connection="close")
        self.assertTrue(c.wait_for_eof())

    def test_oversized_head_is_431(self):
        self.expect_error_then_close(req(headers=[("X-Pad", "p" * 9000)]), 431)

    def test_too_many_headers_is_431(self):
        self.expect_error_then_close(req(headers=[(f"X-{i}", "v") for i in range(101)]), 431)

    def test_oversized_body_is_413(self):
        self.expect_error_then_close(req(method="POST", target="/add", headers=[("Content-Length", str(64 * 1024 + 1))]), 413)

    def test_transfer_encoding_is_501(self):
        self.expect_error_then_close(req(method="POST", target="/add", headers=[("Transfer-Encoding", "chunked")]), 501)

    def test_transfer_encoding_with_content_length_is_400(self):
        self.expect_error_then_close(
            req(method="POST", target="/add", headers=[("Transfer-Encoding", "chunked"), ("Content-Length", "3")]), 400)

    def test_unsupported_http_version_is_505(self):
        self.expect_error_then_close(req(version="HTTP/2.0"), 505)

    def test_whitespace_before_header_colon_is_400(self):
        self.expect_error_then_close(b"GET /add?a=1&b=2 HTTP/1.1\r\nHost : x\r\n\r\n", 400)


class PersistenceRules(ServerTestCase):

    def test_hundred_sequential_requests_on_one_connection(self):
        c = self.client()
        for i in range(100):
            response = c.request(req(target=f"/mul?a={i}&b=3"))
            self.assertResponse(response, 200, f"{3 * i}\n")
            self.assertEqual(response.header("X-Request-Number"), str(i + 1))
        self.assertEqual(len({response.header("X-Connection-Id")}), 1)
        self.assertTrue(c.is_open())

    def test_http10_closes_by_default(self):
        c = self.client()
        response = c.request(req(version="HTTP/1.0", host=None))  # HTTP/1.0 does not require Host
        self.assertResponse(response, 200, "5\n", connection="close")
        self.assertTrue(c.wait_for_eof())

    def test_http10_keep_alive_persists(self):
        c = self.client()
        for _ in range(3):
            response = c.request(req(version="HTTP/1.0", host=None, headers=[("Connection", "keep-alive")]))
            self.assertResponse(response, 200, "5\n")
        self.assertTrue(c.is_open())

    def test_half_close_still_gets_all_responses(self):
        c = self.client()
        c.send(req(target="/add?a=2&b=3") + req(target="/sub?a=10&b=4"))
        c.sock.shutdown(socket.SHUT_WR)  # like `printf ... | nc -N`
        self.assertEqual([c.read_response().body for _ in range(2)], ["5\n", "6\n"])
        self.assertTrue(c.wait_for_eof())

    def test_bare_lf_requests_like_nc(self):
        c = self.client()
        self.assertResponse(c.request(b"GET /add?a=2&b=3 HTTP/1.1\nHost: localhost\n\n"), 200, "5\n")

    def test_lingering_close_delivers_final_response(self):
        # "Connection: close" followed by 64 KiB the server will not parse.
        # Closing with unread input would send an RST that might erase the
        # response (RFC 9112 §9.6). The server half-closes and drains instead, so the client
        # reliably gets the response and then a clean FIN.
        for _ in range(5):
            c = self.client()
            c.send(req(target="/mul?a=6&b=7", headers=[("Connection", "close")]) + b"x" * 65536)
            self.assertResponse(c.read_response(), 200, "42\n", connection="close")
            self.assertTrue(c.wait_for_eof(), "expected FIN, not RST")

    def test_concurrent_connections_do_not_block_each_other(self):
        idle = self.client()
        idle.send(b"GET /add?a=1")  # an unfinished request on connection A...
        busy = self.client()
        for _ in range(5):          # ...does not delay connection B
            self.assertResponse(busy.request(req(target="/sub?a=10&b=4")), 200, "6\n")
        idle.send(b"&b=1 HTTP/1.1\r\nHost: x\r\n\r\n")
        self.assertResponse(idle.read_response(), 200, "2\n")
        self.assertNotEqual(idle.local_address, busy.local_address)


class Timeouts(ServerTestCase):
    server_args = ("--idle-timeout", "0.5")

    def test_idle_connection_is_closed(self):
        c = self.client()
        self.assertResponse(c.request(req()), 200, "5\n")
        started = time.time()
        self.assertTrue(c.wait_for_eof(timeout=5))
        self.assertGreaterEqual(time.time() - started, 0.4)

    def test_activity_keeps_connection_alive(self):
        c = self.client()
        for _ in range(4):  # 4 x 0.3 s > 0.5 s timeout, but never idle for 0.5 s
            self.assertResponse(c.request(req()), 200, "5\n")
            time.sleep(0.3)
        self.assertTrue(c.is_open())

    def test_unfinished_request_gets_408(self):
        c = self.client()
        c.send(b"GET /add?a=1&b=2 HTTP/1.1\r\nHost: x\r\n")  # blank line never sent
        self.assertResponse(c.read_response(), 408, connection="close")
        self.assertTrue(c.wait_for_eof())


class ConnectionLimit(ServerTestCase):
    server_args = ("--max-connections", "2")

    def test_excess_connection_waits_in_backlog_until_a_slot_frees(self):
        first, second = self.client(), self.client()
        self.assertResponse(first.request(req()), 200, "5\n")
        self.assertResponse(second.request(req()), 200, "5\n")
        # connect() still succeeds: the kernel completes the handshake and
        # queues the connection in the listen backlog...
        third = self.client()
        third.send(req(target="/mul?a=6&b=7"))
        # ...but the server does not accept() it while at the limit.
        self.assertTrue(third.nothing_received_within(0.3))
        first.close()
        self.assertResponse(third.read_response(), 200, "42\n")


class Robustness(ServerTestCase):
    server_args = ("--quiet",)

    def test_random_garbage_never_crashes_the_server(self):
        rng = random.Random(10086)  # fixed seed: reproducible
        samples = [bytes(rng.randrange(256) for _ in range(rng.randrange(1, 400))) + b"\r\n\r\n" for _ in range(60)]
        samples += [b"\x00" * 50 + b"\r\n\r\n", b"\r\n" * 100 + req(), b"GET /add?a=1&b=2 HTTP/1.1\r\n" + b"X: \x7f\r\n\r\n"]
        for sample in samples:
            c = Client(self.server.port, timeout=3)
            try:
                c.send(sample)
                response = c.read_response()
                self.assertIn(response.status, (200, 400, 404, 405, 413, 431, 501, 505), response)
            except (ConnectionClosed, ConnectionResetError, socket.timeout):
                pass  # an incomplete head just waits; the point is the server survives
            finally:
                c.close()
        self.assertTrue(self.server.alive())
        self.assertServerStillWorks()


class GracefulShutdown(unittest.TestCase):

    def test_sigint_closes_connections_and_exits_zero(self):
        server = ServerProcess()
        c = Client(server.port)
        self.assertEqual(c.request(req()).body, "5\n")
        started = time.time()
        self.assertEqual(server.stop(), 0)
        self.assertLess(time.time() - started, 3)
        self.assertTrue(c.wait_for_eof(), "open connections are closed on shutdown")
        c.close()


if __name__ == "__main__":
    if not os.path.exists(SERVER_BINARY):
        sys.exit(f"server binary not found at {SERVER_BINARY}; run `make` first")
    unittest.main(verbosity=2)
