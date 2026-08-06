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

# What happened, for the driver to assert on.
#   connections   accepts since the last reset
#   live/peak     connections open AT ONCE - the measurement that tells a
#                 parallel fetcher from a fast serial one, which a plain
#                 accept count cannot
#   t_first/t_last  when the first request arrived and the last response left,
#                 so the driver can compare the page's wall clock against the
#                 sum of its parts without trusting its own sleeps
#   keepalive_asked  did the CLIENT ask to persist? The old client sent
#                 "Connection: close" and this server answered keep-alive
#                 anyway, so reuse looked like it worked here and could not
#                 have worked against any server that honours the header.
stats = {"connections": 0, "requests": 0, "paths": [],
         "live": 0, "peak": 0, "t_first": 0.0, "t_last": 0.0,
         "keepalive_asked": 0, "close_asked": 0}
# Knobs the driver turns to make a property measurable.
#   sheet  per-stylesheet latency. A page's subresources all answering
#          instantly cannot distinguish parallel from serial at QEMU
#          timescales; a second each makes 3-in-a-row and 3-at-once obvious.
#   idle   how long a connection is held open with no request on it. The
#          default is long, so "did the CLIENT reuse its pool" is not really
#          "did the SERVER hang up first" - the old 10 s made a 15 s gap
#          between two page loads look like a client that never pools. Turn
#          it DOWN to test the opposite: recovery from a pool of connections
#          the server dropped, which is what every real server eventually does.
delays = {"sheet": 0.0, "idle": 60.0}
_lock = threading.Lock()


_open = set()          # every connection currently held, for drop_all()


def reset():
    stats.update(connections=0, requests=0, paths=[], live=0, peak=0,
                 t_first=0.0, t_last=0.0, keepalive_asked=0, close_asked=0)


def drop_all():
    """Hang up on every connection we are holding, the way a real server
    eventually does to an idle one. An IDLE TIMEOUT cannot be used for this:
    it is armed when a connection is accepted, so turning it down afterwards
    leaves the already-pooled connections on the old, long one - the test
    then passes without ever having dropped anything. This is unambiguous."""
    with _lock:
        conns = list(_open)
        _open.clear()
    for c in conns:
        try:
            c.close()
        except OSError:
            pass
    return len(conns)


def span():
    """Seconds from the first request in to the last response out, measured
    HERE. The driver's own timing includes its screenshot sleeps."""
    return max(0.0, stats["t_last"] - stats["t_first"])

PAGE = (b"<html><head>"
        b"<link rel=stylesheet href='/a.css'>"
        b"<link rel=stylesheet href='/b.css'>"
        b"<link rel=stylesheet href='/c.css'>"
        b"</head><body><h1>keepalive</h1>"
        b"<p class=k>four requests, one connection</p></body></html>")

SHEETS = {b"/a.css": b".k{color:#c81e28}",
          b"/b.css": b".k{font-weight:bold}",
          b"/c.css": b"h1{color:#1e5ac8}"}

# /big - google.com's SHAPE, which is the shape that broke the browser: a very
# large <head> and every visible byte after it. The padding lives inside
# <style> on purpose, because a renderer that leaked it as text would light up
# the content area and the test would pass on a build that truncates. The only
# renderable bytes in this page start at ~70 KB, past both of the caps that
# used to apply (the transport's 48 KB and the tab's 32 KB).
BIG_PAD = b"".join(b"/* filler line %05d - not renderable, only bulk */\n" % i
                   for i in range(1, 1500))
BIG = (b"<html><head><title>big</title><style>\n" + BIG_PAD +
       b"</style></head><body><h1>a page whose body starts late</h1>" +
       b"".join(b"<p>visible line %d, and every one of these is past 64 KB</p>" % i
               for i in range(1, 12)) +
       b"</body></html>")

# /huge - bigger than the transport will ever hold (RAW_MAX = 1 MB), so the cap
# is genuinely reached. A truncated page must still RENDER what arrived, must
# SAY it was truncated, and must not be pooled: its framing never completed, so
# reusing that connection would read this page's tail as the next page's body.
HUGE = (b"<html><body><h1>a page past the cap</h1>" +
        b"".join(b"<p>huge line %06d, filler to run past one megabyte</p>" % i
                 for i in range(1, 24000)) +
        b"<h2>THE TAIL, WHICH CANNOT ARRIVE</h2></body></html>")

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
        stats["live"] += 1
        if stats["live"] > stats["peak"]:
            stats["peak"] = stats["live"]
        _open.add(conn)
    conn.settimeout(delays["idle"])
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
            low = head.lower()
            with _lock:
                stats["requests"] += 1
                stats["paths"].append(path.decode("latin1"))
                if not stats["t_first"]:
                    stats["t_first"] = time.time()
                if b"connection: keep-alive" in low:
                    stats["keepalive_asked"] += 1
                if b"connection: close" in low:
                    stats["close_asked"] += 1
            if path in SHEETS:
                if delays["sheet"]:
                    time.sleep(delays["sheet"])
                respond(conn, SHEETS[path], b"text/css")
            elif path == b"/big":
                respond(conn, BIG)
            elif path == b"/huge":
                respond(conn, HUGE)
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
            with _lock:
                stats["t_last"] = time.time()
    except Exception:
        pass
    finally:
        with _lock:
            stats["live"] -= 1
            _open.discard(conn)
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
