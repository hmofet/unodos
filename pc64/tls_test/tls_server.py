#!/usr/bin/env python3
"""A per-connection TLS echo server that speaks over stdin/stdout.

QEMU SLIRP `guestfwd=tcp:...-cmd:...` runs this once per guest connection with
fd 0/1 wired to the forwarded TCP stream. We drive a TLS server handshake over
that bidirectional stream using an ssl MemoryBIO, then echo decrypted
application bytes back (re-encrypted). Verifies the UnoDOS/pc64 BearSSL client
end-to-end.

    tls_server.py server.crt server.key
"""
import os, ssl, sys

CERT, KEY = sys.argv[1], sys.argv[2]

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(CERT, KEY)
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
ctx.maximum_version = ssl.TLSVersion.TLSv1_2   # match the BearSSL client profile

inbio, outbio = ssl.MemoryBIO(), ssl.MemoryBIO()
obj = ctx.wrap_bio(inbio, outbio, server_side=True)


def flush():
    d = outbio.read()
    if d:
        os.write(1, d)


def feed():
    d = os.read(0, 16384)
    if not d:
        sys.exit(0)
    inbio.write(d)


# handshake
while True:
    try:
        obj.do_handshake()
        break
    except ssl.SSLWantReadError:
        flush()
        feed()
flush()

# echo application data
while True:
    try:
        data = obj.read(16384)
        if not data:
            break
        obj.write(data)
        flush()
    except ssl.SSLWantReadError:
        flush()
        feed()
    except (ssl.SSLError, OSError):
        break
