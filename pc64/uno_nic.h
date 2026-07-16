/* uno_nic — the networking service (CONTRACT-ARCH §6/§10 server, Phase 13).
 * Networking is absent by default — a queried capability (NET). A per-link driver
 * publishes this service into the registry (§7). A HEADLESS+NET build (zero display
 * surfaces) is then a "server" by composition, not new mechanism. */
#ifndef UNO_NIC_H
#define UNO_NIC_H
#include <stddef.h>
typedef struct uno_nic {
    void *ctx;
    int (*send)(void *ctx, const void *pkt, int len);   /* >=0 bytes sent */
    int (*recv)(void *ctx, void *pkt, int cap);         /* >0 len, 0 if none */
    int (*link)(void *ctx);                             /* 1 = up */
} uno_nic_t;
uno_nic_t *uno_nic_loopback(void);                       /* host loopback backend */
#endif
