#!/usr/bin/env python3
"""An INDEPENDENT model of the SAE password-element derivations, used to check
pc64/wifi_sae.c against something other than itself.

Why this file exists.  wifi_sae.c carries a hand-written 256-bit Montgomery
arithmetic layer, because BearSSL exposes point multiplication but not the
coordinate-field operations SAE needs (modular square roots, quadratic-residue
tests, inverses).  A self-consistency test - two peers agreeing on a PMK -
cannot catch a bug in that layer: both peers run the same wrong code and agree
perfectly.  So this reimplements the two derivations straight from the
IEEE 802.11-2020 text, on Python's arbitrary-precision integers, sharing no
line of code with the C, and diffs the results.

Read against 802.11-2020:
  12.4.4.3.3  hunting-and-pecking
  12.4.4.3.2  direct hashing (hash-to-element) with SSWU
  12.4.5.2    PWE = scalar-op(val, PT)
  12.7.1.6.2  the KDF
"""
import hashlib
import hmac
import sys

# ---- NIST P-256 --------------------------------------------------------------
P = 0xffffffff00000001000000000000000000000000ffffffffffffffffffffffff
N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551
A = (-3) % P
B = 0x5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b
Z = (-10) % P                      # the SSWU constant for group 19


def inv(x):
    return pow(x, P - 2, P)


def is_qr(x):
    return x != 0 and pow(x, (P - 1) // 2, P) == 1


def sqrt_p(x):
    # p == 3 (mod 4), so the square root is a single exponentiation.
    return pow(x, (P + 1) // 4, P)


def on_curve(pt):
    x, y = pt
    return (y * y - (x * x * x + A * x + B)) % P == 0


def pt_add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if p1 == p2:
        lam = (3 * x1 * x1 + A) * inv(2 * y1) % P
    else:
        lam = (y2 - y1) * inv(x2 - x1) % P
    x3 = (lam * lam - x1 - x2) % P
    return (x3, (lam * (x1 - x3) - y1) % P)


def pt_mul(k, pt):
    r, q = None, pt
    while k:
        if k & 1:
            r = pt_add(r, q)
        q = pt_add(q, q)
        k >>= 1
    return r


# ---- 802.11 primitives -------------------------------------------------------
def h256(key, msg):
    return hmac.new(key, msg, hashlib.sha256).digest()


def kdf(key, label, ctx, nbits):
    """12.7.1.6.2: HMAC-SHA256(key, i_LE16 || label || context || nbits_LE16)."""
    out, i = b"", 1
    tail = nbits.to_bytes(2, "little")
    while len(out) * 8 < nbits:
        out += h256(key, i.to_bytes(2, "little") + label.encode() + ctx + tail)
        i += 1
    return out[: (nbits + 7) // 8]


def mac_pair(a, b):
    return (a + b) if a >= b else (b + a)


# ---- 12.4.4.3.3 hunting-and-pecking -----------------------------------------
def pwe_hnp(password, mac_a, mac_b):
    key = mac_pair(mac_a, mac_b)
    for counter in range(1, 41):
        seed = h256(key, password.encode() + bytes([counter]))
        value = int.from_bytes(kdf(seed, "SAE Hunting and Pecking",
                                   P.to_bytes(32, "big"), 256), "big")
        if value >= P:
            continue
        gx = (pow(value, 3, P) + A * value + B) % P
        if is_qr(gx):
            y = sqrt_p(gx)
            if (y & 1) != (seed[31] & 1):
                y = P - y
            return (value, y)
    raise RuntimeError("hunting-and-pecking found no candidate in 40 rounds")


# ---- 12.4.4.3.2 hash-to-element ---------------------------------------------
def sswu(u):
    m = (Z * Z * pow(u, 4, P) + Z * u * u) % P
    if m == 0:
        x1 = B * inv(Z * A % P) % P
    else:
        x1 = ((-B) * inv(A)) % P * (1 + inv(m)) % P
    gx1 = (pow(x1, 3, P) + A * x1 + B) % P
    x2 = Z * u * u % P * x1 % P
    gx2 = (pow(x2, 3, P) + A * x2 + B) % P
    if is_qr(gx1):
        v, x = gx1, x1
    else:
        v, x = gx2, x2
    y = sqrt_p(v)
    if (u & 1) != (y & 1):
        y = P - y
    return (x, y)


def pwe_h2e(ssid, password, mac_a, mac_b):
    prk = h256(ssid.encode(), password.encode())           # HKDF-Extract

    def expand(info, length):                              # HKDF-Expand
        t, out, ctr = b"", b"", 1
        while len(out) < length:
            t = h256(prk, t + info.encode() + bytes([ctr]))
            out += t
            ctr += 1
        return out[:length]

    # len = olen(p) + ceil(olen(p)/2) = 48 bytes for group 19
    u1 = int.from_bytes(expand("SAE Hash to Element u1 P1", 48), "big") % P
    u2 = int.from_bytes(expand("SAE Hash to Element u2 P2", 48), "big") % P
    pt = pt_add(sswu(u1), sswu(u2))
    val = int.from_bytes(h256(b"\x00" * 32, mac_pair(mac_a, mac_b)), "big")
    val = val % (N - 1) + 1
    return pt_mul(val, pt)


# ---- the diff ----------------------------------------------------------------
def enc(pt):
    return "04" + f"{pt[0]:064x}" + f"{pt[1]:064x}"


def main(path):
    got = {}
    with open(path) as fh:
        for line in fh:
            k, _, v = line.strip().partition(" ")
            if k:
                got[k] = v

    ssid = "SKYNET"
    pw = "a test passphrase"
    mac_a = bytes.fromhex("021122334455")
    mac_b = bytes.fromhex("02aabbccddee")

    want = {
        "pwe_hnp": enc(pwe_hnp(pw, mac_a, mac_b)),
        "pwe_h2e": enc(pwe_h2e(ssid, pw, mac_a, mac_b)),
        "kdf512": kdf(b"0123456789abcdef", "SAE KCK and PMK",
                      b"0123456789abcdef0123456789abcdef", 512).hex(),
    }

    fails = 0
    for key, expect in want.items():
        actual = got.get(key)
        if actual is None:
            print(f"FAIL {key}: the C harness printed nothing")
            fails += 1
        elif actual != expect:
            print(f"FAIL {key}\n  C      {actual}\n  python {expect}")
            fails += 1
        else:
            print(f"ok   {key}")

    # The two PWEs must also be genuine curve points, checked here with
    # arithmetic that shares nothing with the C.
    for key in ("pwe_hnp", "pwe_h2e"):
        raw = got.get(key, "")
        if len(raw) == 130 and raw.startswith("04"):
            pt = (int(raw[2:66], 16), int(raw[66:], 16))
            if not on_curve(pt):
                print(f"FAIL {key}: the point the C produced is NOT on P-256")
                fails += 1

    if fails:
        print(f"\n{fails} cross-check FAILURE(S)")
        return 1
    print("\nthe C arithmetic agrees with the independent model")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build/sae_vectors.txt"))
