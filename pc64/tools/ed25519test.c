/* ===========================================================================
 * Ed25519 contract test (host) - RFC 8032 section 7.1.
 *
 * This is the gate phase ssh-a does not land without. Ed25519 is the one piece
 * of crypto the SSH client needs that BearSSL does not carry, everything
 * downstream depends on it, and a signature scheme that is subtly wrong fails
 * in the least useful way possible: it produces plausible bytes that no other
 * implementation accepts.
 *
 * Built WITH the OS's sanitizer set (see ed25519test.sh). A host harness
 * compiled without them is testing different code from the one the debug OS
 * runs - the lesson the UnoAmp EQ defect cost a day to learn, where correct
 * arithmetic and DEFINED arithmetic turned out to be different properties.
 *
 *   sh tools/ed25519test.sh
 * ======================================================================== */
#include "../ed25519.h"
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

struct vec { const char *sk, *pk, *msg, *sig; };

/* RFC 8032 section 7.1, TEST 1, 2, 3 and "TEST SHA(abc)" */
static const struct vec kVec[] = {
{ "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
  "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
  "",
  "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e0652249015"
  "55fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
{ "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
  "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
  "72",
  "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
  "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
{ "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
  "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
  "af82",
  "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
  "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
{ "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
  "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
  "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
  "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
  "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b589"
  "09351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704" },
};
#define NVEC ((int)(sizeof kVec / sizeof kVec[0]))

int main(void)
{
    unsigned char sk[32], pk[32], want_pk[32], sig[64], want_sig[64];
    unsigned char msg[128];
    int i, k, mlen;
    char name[64];

    printf("curve invariants\n");
    CHECK("selftest (sqrt(-1), base point, 2B == B+B)", ed25519_selftest());

    printf("RFC 8032 section 7.1 vectors\n");
    for (i = 0; i < NVEC; i++) {
        unhex(sk, 32, kVec[i].sk);
        unhex(want_pk, 32, kVec[i].pk);
        mlen = unhex(msg, (int)sizeof msg, kVec[i].msg);
        unhex(want_sig, 64, kVec[i].sig);

        ed25519_pubkey(pk, sk);
        sprintf(name, "vector %d: public key", i + 1);
        CHECK(name, memcmp(pk, want_pk, 32) == 0);

        ed25519_sign(sig, msg, mlen, want_pk, sk);
        sprintf(name, "vector %d: signature (%d-byte message)", i + 1, mlen);
        CHECK(name, memcmp(sig, want_sig, 64) == 0);

        sprintf(name, "vector %d: verifies", i + 1);
        CHECK(name, ed25519_verify(want_sig, msg, mlen, want_pk) == 1);
    }

    printf("rejection\n");
    unhex(sk, 32, kVec[2].sk);
    unhex(want_pk, 32, kVec[2].pk);
    mlen = unhex(msg, (int)sizeof msg, kVec[2].msg);
    unhex(want_sig, 64, kVec[2].sig);

    for (k = 0; k < 64; k++) {                 /* every byte of the signature */
        memcpy(sig, want_sig, 64);
        sig[k] ^= 0x01;
        if (ed25519_verify(sig, msg, mlen, want_pk) != 0) break;
    }
    CHECK("flipping any single signature bit is rejected", k == 64);

    memcpy(sig, want_sig, 64);
    CHECK("a flipped message bit is rejected",
          (msg[0] ^= 0x20, ed25519_verify(sig, msg, mlen, want_pk)) == 0);
    msg[0] ^= 0x20;

    unhex(pk, 32, kVec[1].pk);
    CHECK("the wrong public key is rejected",
          ed25519_verify(sig, msg, mlen, pk) == 0);

    {   /* S >= L must be refused rather than quietly reduced */
        int j;
        memcpy(sig, want_sig, 64);
        for (j = 32; j < 64; j++) sig[j] = 0xFF;
        CHECK("a non-canonical S is rejected",
              ed25519_verify(sig, msg, mlen, want_pk) == 0);
    }
    {   /* a public key that is not on the curve */
        unsigned char bad[32];
        memset(bad, 0xFF, 32);
        CHECK("a public key off the curve is rejected",
              ed25519_verify(want_sig, msg, mlen, bad) == 0);
    }

    printf("round trip over message lengths\n");
    {
        int bad = 0;
        unsigned char seed[32], p[32], s[64], buf[200];
        for (i = 0; i < 32; i++) seed[i] = (unsigned char)(i * 7 + 3);
        ed25519_pubkey(p, seed);
        for (mlen = 0; mlen <= 130; mlen += 13) {
            for (i = 0; i < mlen; i++) buf[i] = (unsigned char)(i ^ mlen);
            ed25519_sign(s, buf, mlen, p, seed);
            if (!ed25519_verify(s, buf, mlen, p)) bad = 1;
            if (mlen && ed25519_verify(s, buf, mlen - 1, p)) bad = 1;
        }
        CHECK("sign/verify agree at every length, and length matters", !bad);
    }

    printf("\ned25519test: %s\n", fails ? "FAILURES" : "all checks passed");
    return fails ? 1 : 0;
}
