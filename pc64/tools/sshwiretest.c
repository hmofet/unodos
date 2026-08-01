/* ===========================================================================
 * unossh wire + key-exchange contract test (host).
 *
 * These are the parts of SSH that fail SILENTLY. A wrong mpint, a length in
 * the wrong units, or a scalar handed over in the wrong byte order does not
 * raise an error - it produces an exchange hash the server computes
 * differently, and the connection dies several messages later with "invalid
 * signature", pointing nowhere near the mistake. So they are pinned here,
 * where a failure names itself, rather than discovered against a live sshd.
 *
 * The X25519 vectors matter most. BearSSL's contract is asymmetric - point
 * little-endian, scalar big-endian, clamped internally - which is not what
 * anyone would guess, and getting it backwards yields a perfectly well-formed
 * shared secret that simply is not the same one the peer computed.
 *
 *   sh tools/sshwiretest.sh
 * ======================================================================== */
#include "../unossh.h"
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(name, cond) do {                                              \
    if (cond) printf("  ok   %s\n", name);                                  \
    else { printf("  FAIL %s\n", name); fails++; }                          \
} while (0)

static int unhex(unsigned char *out, int cap, const char *h)
{
    int n = 0;
    while (h[0] && h[1] && n < cap) {
        int hi = (h[0] <= '9') ? h[0] - '0' : (h[0] | 32) - 'a' + 10;
        int lo = (h[1] <= '9') ? h[1] - '0' : (h[1] | 32) - 'a' + 10;
        out[n++] = (unsigned char)((hi << 4) | lo);
        h += 2;
    }
    return n;
}

int main(void)
{
    unsigned char buf[256];
    ssh_buf b;
    ssh_rd r;

    printf("strings\n");
    ssh_buf_init(&b, buf, (int)sizeof buf);
    ssh_put_cstr(&b, "ssh-ed25519");
    CHECK("a string is a big-endian length then the bytes",
          b.len == 15 && memcmp(buf, "\0\0\0\13ssh-ed25519", 15) == 0);

    ssh_buf_init(&b, buf, (int)sizeof buf);
    ssh_put_u32(&b, 0x01020304u);
    CHECK("uint32 is big-endian",
          b.len == 4 && buf[0] == 1 && buf[1] == 2 && buf[2] == 3 && buf[3] == 4);

    ssh_buf_init(&b, buf, 4);
    ssh_put_cstr(&b, "too long for this buffer");
    CHECK("a writer that runs out of room says so", b.err == 1);

    printf("mpint - the encoding that silently breaks an exchange hash\n");
    {
        unsigned char v[8];
        ssh_buf_init(&b, buf, (int)sizeof buf);
        v[0] = 0x00; v[1] = 0x00;
        ssh_put_mpint(&b, v, 2);
        CHECK("zero is the empty string", b.len == 4 && memcmp(buf, "\0\0\0\0", 4) == 0);

        ssh_buf_init(&b, buf, (int)sizeof buf);
        v[0] = 0x00; v[1] = 0x09; v[2] = 0xa3;
        ssh_put_mpint(&b, v, 3);
        CHECK("leading zero bytes are not part of the value",
              b.len == 6 && buf[3] == 2 && buf[4] == 0x09 && buf[5] == 0xa3);

        ssh_buf_init(&b, buf, (int)sizeof buf);
        v[0] = 0x80; v[1] = 0x00;
        ssh_put_mpint(&b, v, 2);
        CHECK("a positive value with the top bit set gains a zero byte",
              b.len == 7 && buf[3] == 3 && buf[4] == 0x00 &&
              buf[5] == 0x80 && buf[6] == 0x00);

        ssh_buf_init(&b, buf, (int)sizeof buf);
        v[0] = 0x7f; v[1] = 0xff;
        ssh_put_mpint(&b, v, 2);
        CHECK("a positive value with the top bit clear does not",
              b.len == 6 && buf[3] == 2 && buf[4] == 0x7f);
    }

    printf("reader\n");
    ssh_buf_init(&b, buf, (int)sizeof buf);
    ssh_put_u8(&b, 21);
    ssh_put_cstr(&b, "curve25519-sha256");
    ssh_put_u32(&b, 4242u);
    ssh_rd_init(&r, buf, b.len);
    {
        unsigned t = ssh_get_u8(&r);
        int n = 0;
        const unsigned char *s = ssh_get_str(&r, &n);
        unsigned tail = ssh_get_u32(&r);
        CHECK("round trips what the writer wrote",
              t == 21 && n == 17 && s && memcmp(s, "curve25519-sha256", 17) == 0
              && tail == 4242u && !r.err);
        CHECK("and is then exhausted", ssh_rd_left(&r) == 0);
    }
    ssh_rd_init(&r, buf, 6);                  /* truncated mid-string */
    { int n; ssh_get_u8(&r); ssh_get_str(&r, &n);
      CHECK("a truncated read poisons the reader", r.err == 1);
      CHECK("and every later get returns zero", ssh_get_u32(&r) == 0); }
    {   /* a length field is 32 bits on the wire; our buffers are not */
        unsigned char evil[8];
        int n;
        evil[0] = 0xFF; evil[1] = 0xFF; evil[2] = 0xFF; evil[3] = 0xFF;
        ssh_rd_init(&r, evil, 8);
        ssh_get_str(&r, &n);
        CHECK("an absurd length is refused, not trusted", r.err == 1);
    }

    printf("packet padding (RFC 4253 section 6)\n");
    {
        int bad = 0, p, total, n;
        for (n = 0; n < 300; n++) {
            p = ssh_pad_len(n, 16);
            total = 5 + n + p;
            if (p < 4 || p > 255) bad = 1;
            if (total % 16) bad = 1;
            if (total < 16) bad = 1;
        }
        CHECK("16-byte blocks: 4..255 pad, whole blocks, never under 16", !bad);
        bad = 0;
        for (n = 0; n < 300; n++) {
            p = ssh_pad_len(n, 8);
            total = 5 + n + p;
            if (p < 4 || p > 255 || (total % 8) || total < 16) bad = 1;
        }
        CHECK("8-byte blocks likewise", !bad);
    }

    printf("X25519 - RFC 7748\n");
    {
        unsigned char apriv[32], apub[32], bpriv[32], bpub[32], want[32];
        unsigned char got[32], got2[32];
        unhex(apriv, 32, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        unhex(apub,  32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
        unhex(bpriv, 32, "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
        unhex(bpub,  32, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
        unhex(want,  32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");

        CHECK("section 6.1: alice's public key",
              ssh_x25519_base(got, apriv) && memcmp(got, apub, 32) == 0);
        CHECK("section 6.1: bob's public key",
              ssh_x25519_base(got, bpriv) && memcmp(got, bpub, 32) == 0);
        CHECK("section 6.1: the shared secret, both ways",
              ssh_x25519(got, apriv, bpub) && ssh_x25519(got2, bpriv, apub) &&
              memcmp(got, want, 32) == 0 && memcmp(got2, want, 32) == 0);

        unhex(apriv, 32, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
        unhex(bpub,  32, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
        unhex(want,  32, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
        CHECK("section 5.2: a non-base point multiplies correctly",
              ssh_x25519(got, apriv, bpub) && memcmp(got, want, 32) == 0);
    }

    printf("exchange hash and key derivation\n");
    {
        unsigned char h[32], h2[32], k[32], sid[32], key[80], key2[80];
        ssh_exch e;
        int i;
        for (i = 0; i < 32; i++) { k[i] = (unsigned char)(i + 1); sid[i] = (unsigned char)(0x40 + i); }
        e.v_c = "SSH-2.0-UnoDOS_1.0"; e.v_s = "SSH-2.0-OpenSSH_9.6";
        e.i_c = k; e.i_c_len = 20; e.i_s = sid; e.i_s_len = 24;
        e.k_s = k; e.k_s_len = 32;
        e.q_c = k; e.q_s = sid; e.k = k; e.k_len = 32;
        ssh_exchange_hash(h, &e);
        ssh_exchange_hash(h2, &e);
        CHECK("the exchange hash is deterministic", memcmp(h, h2, 32) == 0);
        e.v_s = "SSH-2.0-OpenSSH_9.7";
        ssh_exchange_hash(h2, &e);
        CHECK("and depends on every field", memcmp(h, h2, 32) != 0);

        ssh_derive_key(key, 32, 'C', k, 32, h, sid);
        ssh_derive_key(key2, 32, 'D', k, 32, h, sid);
        CHECK("different letters give different keys", memcmp(key, key2, 32) != 0);

        ssh_derive_key(key2, 80, 'C', k, 32, h, sid);
        CHECK("a longer key extends the first block rather than replacing it",
              memcmp(key, key2, 32) == 0);
        {   /* the extension really is K2 = HASH(K||H||K1), not a repeat */
            int same = 1;
            for (i = 32; i < 64; i++) if (key2[i] != key2[i - 32]) same = 0;
            CHECK("and the extension is fresh material", !same);
        }
    }

    printf("\nsshwiretest: %s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
