#!/usr/bin/env python3
"""uno_manifest - author signed capability manifests for UnoDOS automation apps.

An automation app runs with no human to answer a per-op consent prompt, so a
trusted one ships a signed `<app>.MFT` manifest next to its `.UNO`, declaring the
capabilities it needs.  At launch UnoDOS verifies the signature against a key in
its trust store (see `keygen` + TRUST.MFK below) and grants the declared caps for
the app's lifetime - no prompts.  Format (see pc64/UNOSECURE.md SPEC 5):

    UNOSEC-MANIFEST v1
    name: <app name>
    caps: <comma-separated cap names, e.g. proc.enum,fs.sys>
    key:  <trusted key id>
    sig:  <hex HMAC-SHA256 over the body above, up to and incl. the \\n before sig>

Usage:
    # 1. make a signing key + the TRUST.MFK line to enroll it on the machine
    python uno_manifest.py keygen --key-id acme > acme.trust
    #    -> prints  "acme <64 hex>"  (append to TRUST.MFK on the boot volume)
    #       and, on stderr, the secret hex to keep for signing.

    # 2. sign a manifest for an app, writing MYBOT.MFT beside MYBOT.UNO
    python uno_manifest.py sign --name mybot --caps proc.enum,fs.sys \\
        --key-id acme --secret <64 hex> -o MYBOT.MFT

Caps are the names from the API reference (proc.enum, fs.user, fs.sys, io.read,
mem.read, ...).  A kiosk (deny) policy still refuses everything, and an untrusted
signature grants nothing - the manifest is a convenience for trusted apps, never
a way around the gate.
"""
import argparse, hashlib, hmac, os, secrets, sys


def manifest_body(name, caps, key_id):
    # canonical, LF-terminated; the signed region ends with the '\n' before sig.
    return ("UNOSEC-MANIFEST v1\n"
            "name: %s\n"
            "caps: %s\n"
            "key: %s\n" % (name, caps, key_id))


def sign(body, secret):
    return hmac.new(secret, body.encode(), hashlib.sha256).hexdigest()


def cmd_keygen(a):
    secret = secrets.token_bytes(32)
    # stdout: the TRUST.MFK line to enroll (id + public-ish key id mapping).
    print("%s %s" % (a.key_id, secret.hex()))
    # stderr: the secret to sign with (same 32 bytes - HMAC is symmetric).
    sys.stderr.write("secret (keep safe, use with `sign --secret`): %s\n" % secret.hex())
    return 0


def cmd_sign(a):
    try:
        secret = bytes.fromhex(a.secret)
    except ValueError:
        sys.stderr.write("error: --secret must be hex\n"); return 2
    if len(secret) != 32:
        sys.stderr.write("error: --secret must be 32 bytes (64 hex chars)\n"); return 2
    body = manifest_body(a.name, a.caps, a.key_id)
    manifest = body + "sig: %s\n" % sign(body, secret)
    if a.out:
        with open(a.out, "w", newline="\n") as f:
            f.write(manifest)
        sys.stderr.write("wrote %s (%d bytes)\n" % (a.out, len(manifest)))
    else:
        sys.stdout.write(manifest)
    return 0


def main():
    p = argparse.ArgumentParser(description="author signed UnoDOS capability manifests")
    sub = p.add_subparsers(dest="cmd", required=True)

    k = sub.add_parser("keygen", help="generate a signing key + its TRUST.MFK line")
    k.add_argument("--key-id", required=True, help="short id for the key (e.g. your name)")
    k.set_defaults(fn=cmd_keygen)

    s = sub.add_parser("sign", help="sign a manifest for an app")
    s.add_argument("--name", required=True, help="app name")
    s.add_argument("--caps", required=True, help="comma-separated cap names")
    s.add_argument("--key-id", required=True, help="the trusted key id (matches TRUST.MFK)")
    s.add_argument("--secret", required=True, help="the 32-byte signing key, hex")
    s.add_argument("-o", "--out", help="write here (e.g. MYBOT.MFT); else stdout")
    s.set_defaults(fn=cmd_sign)

    a = p.parse_args()
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
