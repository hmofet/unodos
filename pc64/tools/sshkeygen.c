/* ===========================================================================
 * Print the authorized_keys line for the ssh-c gate's throwaway test key.
 *
 * Uses OUR ed25519.c to derive the public key, which makes this more than a
 * convenience: if OpenSSH accepts a key this produced and then verifies a
 * signature our client made with the matching seed, the implementation is
 * interoperable at both ends of the same key, not merely self-consistent.
 *
 *   cc -Ibearssl/inc -o build/sshkeygen tools/sshkeygen.c ed25519.c \
 *      bearssl/src/hash/sha2big.c bearssl/src/codec/{dec,enc}64be.c
 * ======================================================================== */
#include "../ed25519.h"
#include "sshtestkey.h"
#include <stdio.h>

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64(const unsigned char *d, int n)
{
    int i;
    for (i = 0; i < n; i += 3) {
        unsigned v = (unsigned)d[i] << 16;
        int have = n - i;
        if (have > 1) v |= (unsigned)d[i + 1] << 8;
        if (have > 2) v |= (unsigned)d[i + 2];
        putchar(kB64[(v >> 18) & 63]);
        putchar(kB64[(v >> 12) & 63]);
        putchar(have > 1 ? kB64[(v >> 6) & 63] : '=');
        putchar(have > 2 ? kB64[v & 63] : '=');
    }
}

int main(void)
{
    unsigned char pk[32], blob[64];
    int n = 0, i;
    static const char ty[] = "ssh-ed25519";

    ed25519_pubkey(pk, kSshTestSeed);

    /* the SSH public key blob: string "ssh-ed25519", string key */
    blob[n++] = 0; blob[n++] = 0; blob[n++] = 0; blob[n++] = 11;
    for (i = 0; i < 11; i++) blob[n++] = (unsigned char)ty[i];
    blob[n++] = 0; blob[n++] = 0; blob[n++] = 0; blob[n++] = 32;
    for (i = 0; i < 32; i++) blob[n++] = pk[i];

    printf("ssh-ed25519 ");
    b64(blob, n);
    printf(" unodos-ssh-c-gate\n");
    return 0;
}
