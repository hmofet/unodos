# pc64 TLS test fixtures

Throwaway test material for verifying the pc64 BearSSL TLS client in QEMU.
**Not secret, not for any real endpoint** — `server.key`/`server.crt` are a
self-signed P-256 pair for a localhost echo server the harness runs.

- `server.key` / `server.crt` — the EC (P-256) key + self-signed cert.
- `gen.sh` — regenerates them and prints the 65-byte uncompressed public
  point. If you regenerate, paste the new `POINT65` into `PINNED_EC_Q` in
  `../tls.c` (the client pins that exact key — that IS the trust anchor).
- `tls_server.py` — a per-connection TLS echo server that speaks over
  stdin/stdout, so QEMU SLIRP `guestfwd=...-cmd:` can run it per guest
  connection. Driven by `../nettest.py`.

The client trusts the server by **pinned public key** (`br_x509_knownkey`),
so no CA store and no system clock are needed.
