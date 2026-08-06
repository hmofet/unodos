#!/usr/bin/env python3
"""nettest_server - a server that answers the questions the null-NIC gate
cannot: does keep-alive actually reuse a connection, does chunked decoding
work end to end, and does a page paint before its last byte arrives?

It is deliberately hand-rolled rather than http.server: the measurement IS
the connection lifecycle, and a framework that manages connections for you
is a framework that hides the thing under test. Every accept and every
request is logged, so "one connection, four requests" is an observation
rather than an inference.

Reachable from a QEMU guest on user-mode networking at 10.0.2.2:<port>.

    python3 tools/nettest_server.py [port]        # run standalone
"""
import socket, sys, threading, time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8099

# what happened, for the driver to assert on
stats = {"connections": 0, "requests": 0, "paths": []}
_lock = threading.Lock()

PAGE = (b"<html><head>"
        b"<link rel=stylesheet href='/a.css'>"
        b"<link rel=stylesheet href='/b.css'>"
        b"<link rel=stylesheet href='/c.css'>"
        b"</head><body><h1>keepalive</h1>"
        b"<p class=k>four requests, one connection</p></body></html>")

SHEETS = {b"/a.css": b".k{color:#c81e28}",
          b"/b.css": b".k{font-weight:bold}",
          b"/c.css": b"h1{color:#1e5ac8}"}

# a page big enough that it cannot arrive in one go, with a pause in the
# middle - if progressive render works, the top is on screen during it
SLOW_HEAD = b"<html><body><h1>slow page</h1>"
SLOW_BODY = b"".join(b"<p>line %d of the first half</p>" % i for i in range(1, 260))
SLOW_TAIL = b"".join(b"<p>line %d of the second half</p>" % i for i in range(1, 60))
SLOW_TAIL += b"<h2>THE END</h2></body></html>"


def respond(conn, body, ctype=b"text/html", chunked=False, slow=False):
    if chunked:
        head = (b"HTTP/1.1 200 OK\r\nContent-Type: " + ctype +
                b"\r\nTransfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n")
        conn.sendall(head)
        # several chunks, so the decoder is genuinely exercised
        step = max(1, len(body) // 3)
        for i in range(0, len(body), step):
            part = body[i:i + step]
            conn.sendall(b"%x\r\n" % len(part) + part + b"\r\n")
        conn.sendall(b"0\r\n\r\n")
        return
    head = (b"HTTP/1.1 200 OK\r\nContent-Type: " + ctype +
            b"\r\nContent-Length: " + str(len(body)).encode() +
            b"\r\nConnection: keep-alive\r\n\r\n")
    if slow:
        # headers + first half, then a visible pause, then the rest
        conn.sendall(head + SLOW_HEAD + SLOW_BODY)
        time.sleep(2.5)
        conn.sendall(SLOW_TAIL)
        return
    conn.sendall(head + body)


def serve_conn(conn):
    with _lock:
        stats["connections"] += 1
    conn.settimeout(10)
    buf = b""
    try:
        while True:
            while b"\r\n\r\n" not in buf:
                d = conn.recv(4096)
                if not d:
                    return
                buf += d
            head, buf = buf.split(b"\r\n\r\n", 1)
            line = head.split(b"\r\n")[0]
            path = line.split(b" ")[1] if b" " in line else b"/"
            with _lock:
                stats["requests"] += 1
                stats["paths"].append(path.decode("latin1"))
            if path in SHEETS:
                respond(conn, SHEETS[path], b"text/css")
            elif path == b"/chunked":
                respond(conn, b"<html><body><h1>chunked</h1>"
                              b"<p>decoded across three chunks</p></body></html>",
                        chunked=True)
            elif path == b"/slow":
                respond(conn, SLOW_HEAD + SLOW_BODY + SLOW_TAIL, slow=True)
            elif path == b"/stats":
                with _lock:
                    s = ("connections=%d requests=%d paths=%s" %
                         (stats["connections"], stats["requests"],
                          ",".join(stats["paths"]))).encode()
                respond(conn, s, b"text/plain")
            else:
                respond(conn, PAGE)
    except Exception:
        pass
    finally:
        try: conn.close()
        except Exception: pass


def start(port=PORT):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", port))
    srv.listen(8)
    def loop():
        while True:
            try:
                c, _ = srv.accept()
            except OSError:
                return
            threading.Thread(target=serve_conn, args=(c,), daemon=True).start()
    threading.Thread(target=loop, daemon=True).start()
    return srv


if __name__ == "__main__":
    start()
    print("nettest_server on :%d - ctrl-c to stop" % PORT)
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n%s" % stats)
