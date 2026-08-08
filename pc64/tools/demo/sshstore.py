#!/usr/bin/env python3
"""Author a pc64 `SSHSTORE.DAT` on the HOST, so the demo can show the SSH app
connecting to a real machine.

WHY THIS EXISTS. `unossh` keeps its keys, saved sessions and known hosts in one
container file (pc64/unossh_store.c). Nothing that a driver can reach populates
it:

  - `sshapp_ui.c` only LISTS sessions and keys and connects to the selected one.
    There is no "add session" and no "import key" control anywhere in the app.
  - `unossh_cmd.c` implements a complete `ssh` verb (keygen / sessadd / run
    ...), and its own header comment says unoautomate "lands a weak stub and a
    four-line dispatch clause once" - but that clause was never landed:
    `grep ssh unoauto_remote.c` is empty. So the verb is unreachable over URC.
  - the only callers of `ssh_key_import` / `ssh_sess_set` in the whole tree are
    the SPECTEST suites in unossh.c, which seed a fixed 10.0.2.2:2222 session.

So the store is authored here and staged like any other asset. This file
replicates exactly what `ssh_key_add` + `ssh_sess_set` + `store_save` write, and
nothing else - see unossh_store.c:

    key_ent  { char name[32]; u8 salt[16], ct[32], mac[32], pub[32]; int used, guarded; }
    sess_ent { char name[32], host[64], user[32], key[32]; int port, used; }
    host_ent { char host[64]; u8 fp[32]; int used; }
    ssh_store{ char magic[8]="UNOSSH01"; int version=1;
               key_ent keys[8]; sess_ent sess[16]; host_ent hosts[32]; }

and the at-rest protection of the 32-byte ed25519 seed:

    kd      = PBKDF2-HMAC-SHA256(passphrase, salt, 200000, 64)
    ct      = seed XOR AES-256-CTR keystream(kd[0:32], iv = 12 zero bytes, cc=0)
    mac     = HMAC-SHA256(kd[32:64], salt || ct)

`store_load()` accepts the file ONLY if it is exactly sizeof(ssh_store) bytes
with the right magic and version; anything else is silently replaced by an empty
store. That is the failure mode to watch for - it presents as "no saved
sessions" in the app, not as an error.

AES is implemented here rather than imported: two blocks of keystream is all
this needs, and a demo driver that runs on three different hosts should not
depend on a crypto package being installed on any of them.

    python3 sshstore.py KEYFILE OUT.DAT --name devbuntu \\
        --host 192.168.2.100 --user arin [--port 22]
"""
import hashlib, hmac, struct, sys

# ---- struct geometry (must match unossh.h / unossh_store.c) ---------------
NAMELEN, HOSTLEN = 32, 64
MAXKEYS, MAXSESS, MAXHOSTS = 8, 16, 32
KEY_ENT, SESS_ENT, HOST_ENT = 152, 168, 100
STORE_SIZE = 8 + 4 + MAXKEYS * KEY_ENT + MAXSESS * SESS_ENT + MAXHOSTS * HOST_ENT
PBKDF2_ITERS = 200000


# ---- AES-256 block encryption (encrypt only; two blocks of CTR keystream) --
_SBOX = None


def _init_tables():
    global _SBOX
    if _SBOX is not None:
        return
    p = q = 1
    sbox = [0] * 256
    while True:                                  # generate the S-box from GF(2^8)
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) \
              ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        sbox[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    sbox[0] = 0x63
    _SBOX = sbox


def _xtime(a):
    a <<= 1
    return (a ^ 0x1B) & 0xFF if a & 0x100 else a


def _key_expand(key):
    """AES-256: 32-byte key -> 15 round keys of 16 bytes."""
    _init_tables()
    nk, nr = 8, 14
    w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
    rcon = 1
    for i in range(nk, 4 * (nr + 1)):
        t = list(w[i - 1])
        if i % nk == 0:
            t = t[1:] + t[:1]
            t = [_SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = _xtime(rcon)
        elif i % nk == 4:
            t = [_SBOX[b] for b in t]
        w.append([w[i - nk][j] ^ t[j] for j in range(4)])
    return [bytes(b for word in w[4 * r:4 * r + 4] for b in word)
            for r in range(nr + 1)]


def _aes_encrypt_block(rk, block):
    _init_tables()
    s = [b ^ k for b, k in zip(block, rk[0])]
    for rnd in range(1, len(rk)):
        s = [_SBOX[b] for b in s]
        # ShiftRows. The state is COLUMN-major - byte i is row i%4, column i//4 -
        # so row r rotates left by r: s'[r][c] = s[r][(c+r) % 4].
        t = list(s)
        for c in range(4):
            for r in range(4):
                t[4 * c + r] = s[4 * ((c + r) % 4) + r]
        s = t
        if rnd != len(rk) - 1:                   # MixColumns
            t = list(s)
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                t[4 * c + 0] = _xtime(a[0]) ^ (_xtime(a[1]) ^ a[1]) ^ a[2] ^ a[3]
                t[4 * c + 1] = a[0] ^ _xtime(a[1]) ^ (_xtime(a[2]) ^ a[2]) ^ a[3]
                t[4 * c + 2] = a[0] ^ a[1] ^ _xtime(a[2]) ^ (_xtime(a[3]) ^ a[3])
                t[4 * c + 3] = (_xtime(a[0]) ^ a[0]) ^ a[1] ^ a[2] ^ _xtime(a[3])
            s = t
        s = [b ^ k for b, k in zip(s, rk[rnd])]
    return bytes(s)


def aes256_ctr(key, iv12, cc, data):
    """BearSSL's br_aes_ct64_ctr_run: counter block = iv(12) || BE32(cc)."""
    rk = _key_expand(key)
    out = bytearray()
    for off in range(0, len(data), 16):
        ks = _aes_encrypt_block(rk, iv12 + struct.pack(">I", cc))
        cc += 1
        chunk = data[off:off + 16]
        out += bytes(a ^ b for a, b in zip(chunk, ks))
    return bytes(out)


# ---- the OpenSSH private-key file ----------------------------------------
def parse_openssh_ed25519(text):
    """(seed32, pub32) from an UNENCRYPTED openssh-key-v1 ed25519 file.

    Same walk as ssh_key_import (unossh_store.c) - and the same refusal: an
    encrypted key has ciphername != "none" and there is no bcrypt_pbkdf here or
    on the device, which is exactly why the demo key has no passphrase."""
    import base64
    lines = [l.strip() for l in text.splitlines()]
    body = "".join(l for l in lines if l and not l.startswith("-----"))
    raw = base64.b64decode(body)
    if raw[:15] != b"openssh-key-v1\0":
        raise ValueError("not an openssh-key-v1 file")
    p = 15

    def s():
        nonlocal p
        (n,) = struct.unpack(">I", raw[p:p + 4])
        p += 4
        v = raw[p:p + n]
        p += n
        return v

    cipher = s()
    if cipher != b"none":
        raise ValueError("the key is encrypted (%s); import needs bcrypt_pbkdf, "
                         "which neither this tool nor the device has" %
                         cipher.decode())
    s()                                          # kdfname
    s()                                          # kdfopts
    p += 4                                       # key count
    s()                                          # public blob
    priv = s()
    q = 8                                        # two checkints
    (n,) = struct.unpack(">I", priv[q:q + 4]); q += 4
    if priv[q:q + n] != b"ssh-ed25519":
        raise ValueError("not an ed25519 key")
    q += n
    (n,) = struct.unpack(">I", priv[q:q + 4]); q += 4
    pub = priv[q:q + n]; q += n
    (n,) = struct.unpack(">I", priv[q:q + 4]); q += 4
    if n != 64:
        raise ValueError("private field is %d bytes, expected 64" % n)
    blob = priv[q:q + 64]
    return blob[:32], blob[32:64]                # seed || pub


# ---- the store ------------------------------------------------------------
def _fixed(s, cap):
    b = s.encode("ascii") if isinstance(s, str) else s
    if len(b) >= cap:
        raise ValueError("%r does not fit in %d bytes" % (s, cap))
    return b + b"\0" * (cap - len(b))


def key_entry(name, seed, pub, salt, passphrase=""):
    kd = hashlib.pbkdf2_hmac("sha256", passphrase.encode(), salt,
                             PBKDF2_ITERS, 64)
    ct = aes256_ctr(kd[:32], b"\0" * 12, 0, seed)
    mac = hmac.new(kd[32:], salt + ct, hashlib.sha256).digest()
    e = _fixed(name, NAMELEN) + salt + ct + mac + pub \
        + struct.pack("<ii", 1, 1 if passphrase else 0)
    assert len(e) == KEY_ENT, len(e)
    return e


def sess_entry(name, host, port, user, key):
    e = _fixed(name, NAMELEN) + _fixed(host, HOSTLEN) + _fixed(user, NAMELEN) \
        + _fixed(key, NAMELEN) + struct.pack("<ii", port, 1)
    assert len(e) == SESS_ENT, len(e)
    return e


def build(keyfile_text, key_name, sess_name, host, port, user, salt=None):
    """The whole container: one key, one session, no known hosts.

    Leaving known-hosts EMPTY is deliberate. The app's policy is
    trust-on-first-use: an unknown host is recorded on sight and reported as
    "New host, key recorded", where a MISMATCH refuses in red. A store that
    claimed to already know the host would be asserting a fingerprint this tool
    has not verified."""
    seed, pub = parse_openssh_ed25519(keyfile_text)
    if salt is None:
        import os
        salt = os.urandom(16)
    keys = key_entry(key_name, seed, pub, salt)
    keys += b"\0" * (KEY_ENT * (MAXKEYS - 1))
    sess = sess_entry(sess_name, host, port, user, key_name)
    sess += b"\0" * (SESS_ENT * (MAXSESS - 1))
    blob = b"UNOSSH01" + struct.pack("<i", 1) + keys + sess \
        + b"\0" * (HOST_ENT * MAXHOSTS)
    assert len(blob) == STORE_SIZE, (len(blob), STORE_SIZE)
    return blob


def _selftest():
    """AES-256 against FIPS-197 C.3, and the container against its own geometry."""
    rk = _key_expand(bytes(range(32)))
    ct = _aes_encrypt_block(rk, bytes.fromhex("00112233445566778899aabbccddeeff"))
    assert ct.hex() == "8ea2b7ca516745bfeafc49904b496089", ct.hex()
    assert STORE_SIZE == 7116, STORE_SIZE
    print("sshstore selftest: AES-256 FIPS-197 C.3 ok, store is %d bytes"
          % STORE_SIZE)


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("keyfile", nargs="?")
    ap.add_argument("out", nargs="?")
    ap.add_argument("--name", default="devbuntu", help="saved-session name")
    ap.add_argument("--keyname", default="demo")
    ap.add_argument("--host", default="192.168.2.100")
    ap.add_argument("--port", type=int, default=22)
    ap.add_argument("--user", default="arin")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        _selftest()
        return 0
    if not (a.keyfile and a.out):
        ap.error("KEYFILE and OUT are required")
    with open(a.keyfile) as f:
        blob = build(f.read(), a.keyname, a.name, a.host, a.port, a.user)
    with open(a.out, "wb") as f:
        f.write(blob)
    print("%s: %d bytes, session %r -> %s@%s:%d key %r"
          % (a.out, len(blob), a.name, a.user, a.host, a.port, a.keyname))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
