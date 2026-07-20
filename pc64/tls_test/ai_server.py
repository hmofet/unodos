#!/usr/bin/env python3
"""A fake AI provider endpoint over TLS, for testing Studio's assistant offline.

Speaks the same MemoryBIO TLS handshake as tls_server.py (reuses the pinned
throwaway cert), reads one HTTP request, and replies with a canned provider
JSON body.  Studio's AI client reaches it with an AI.CFG that overrides the
host and uses the pinned-key TLS path:

    provider=openai
    key_openai=test
    host=10.0.2.100:9443
    insecure=1

Run under QEMU SLIRP, one process per guest connection:

    guestfwd=tcp:10.0.2.100:9443-cmd:python3 tls_test/ai_server.py \
        tls_test/server.crt tls_test/server.key [openai|anthropic|gemini]

The reply shape matches the provider so studio_json.c's extractor path
(choices.0.message.content / content.0.text / candidates.0...text) resolves.
This verifies the transport + request build + response parse end to end
without a real key or the internet.
"""
import os, ssl, sys, json

CERT, KEY = sys.argv[1], sys.argv[2]
PROVIDER = sys.argv[3] if len(sys.argv) > 3 else "openai"

REPLY = ("Here is a UnoC bounce:\n```c\n#include \"UNO.H\"\n"
         "const AppInterface *uno_app_main(const KernelApi *k){gK=k;return &kIface;}\n```")

if PROVIDER == "anthropic":
    body = {"id": "msg_test", "type": "message",
            "content": [{"type": "text", "text": REPLY}], "model": "test"}
elif PROVIDER == "gemini":
    body = {"candidates": [{"content": {"parts": [{"text": REPLY}], "role": "model"}}]}
else:
    body = {"id": "cmpl", "choices": [{"index": 0,
            "message": {"role": "assistant", "content": REPLY}}]}

payload = json.dumps(body).encode()
resp = (b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        b"Content-Length: " + str(len(payload)).encode() +
        b"\r\nConnection: close\r\n\r\n" + payload)

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(CERT, KEY)
ctx.minimum_version = ssl.TLSVersion.TLSv1_2
ctx.maximum_version = ssl.TLSVersion.TLSv1_2

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


while True:                                   # handshake
    try:
        obj.do_handshake(); break
    except ssl.SSLWantReadError:
        flush(); feed()
flush()

# read the request (until blank line), then send the canned response
req = b""
while b"\r\n\r\n" not in req:
    try:
        d = obj.read(16384)
        if not d:
            break
        req += d
    except ssl.SSLWantReadError:
        flush(); feed()
    except (ssl.SSLError, OSError):
        break

try:
    obj.write(resp)
    flush()
except (ssl.SSLError, OSError):
    pass
