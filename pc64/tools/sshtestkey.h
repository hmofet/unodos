/* ===========================================================================
 * The Ed25519 seed the ssh-c gate authenticates with.
 *
 * THROWAWAY, AND DELIBERATELY IN THE REPO. It authorises exactly one thing: a
 * throwaway sshd the harness starts on loopback, with its own throwaway host
 * key and its own authorized_keys, which it kills again when the scenario
 * ends. It is never installed anywhere else and grants access to nothing.
 *
 * Shared by the guest-side test (unossh.c, under UNO_DEBUG) and the host-side
 * tools/sshkeygen.c that writes the authorized_keys line. ONE definition, so
 * the key the guest signs with and the key the server trusts cannot drift
 * apart - which would fail as a bare "rejected" with nothing pointing at why.
 * ======================================================================== */
#ifndef PC64_SSHTESTKEY_H
#define PC64_SSHTESTKEY_H

static const unsigned char kSshTestSeed[32] = {
    0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60,
    0xba, 0x84, 0x4a, 0xf4, 0x92, 0xec, 0x2c, 0xc4,
    0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32, 0x69, 0x19,
    0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60
};

#endif
