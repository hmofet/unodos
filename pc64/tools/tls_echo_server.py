#!/usr/bin/env python3
"""A listening TLS echo server for the tls_conn host gate.

tls_test/tls_server.py speaks over stdin/stdout because QEMU's SLIRP
`guestfwd=...-cmd:` runs one process per guest connection. The host gate has no
QEMU and needs the opposite shape: ONE process holding SEVERAL connections open
at once, which is the entire property under test - a server that serialised its
clients could not tell a concurrent client from a sequential one.

Threaded, one thread per connection, echoing until the peer goes away. Prints
`listening <port>` on stdout once the socket is bound, so the gate can wait for
readiness instead of sleeping and hoping.

    tls_echo_server.py <cert> <key> [port]      (port 0 = pick one, and say so)
"""
import socket, ssl, sys, threading


def serve_one(conn, addr):
    try:
        while True:
            b = conn.recv(4096)
            if not b:
                break
            conn.sendall(b)
    except (OSError, ssl.SSLError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: tls_echo_server.py <cert> <key> [port]")
    cert, key = sys.argv[1], sys.argv[2]
    port = int(sys.argv[3]) if len(sys.argv) > 3 else 0

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(cert, key)
    # The client pins a P-256 key and offers BearSSL's full client profile; the
    # gate is about the transport, not about negotiating something exotic.
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.maximum_version = ssl.TLSVersion.TLSv1_2

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(16)
    print("listening %d" % srv.getsockname()[1], flush=True)

    while True:
        try:
            raw, addr = srv.accept()
        except OSError:
            break
        # Wrapping inside the worker matters: the handshake itself is what the
        # gate runs concurrently, so accepting must never block on one peer's.
        def run(raw=raw, addr=addr):
            try:
                conn = ctx.wrap_socket(raw, server_side=True)
            except (OSError, ssl.SSLError):
                try:
                    raw.close()
                except OSError:
                    pass
                return
            serve_one(conn, addr)
        threading.Thread(target=run, daemon=True).start()


if __name__ == "__main__":
    main()
