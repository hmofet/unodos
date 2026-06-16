#include "uno_nic.h"
#include <string.h>
#define CAP 16
#define MTU 256
static struct { unsigned char buf[CAP][MTU]; int len[CAP]; int head, tail; } LB;
static int lb_send(void *c, const void *p, int n) { (void)c;
    if (n > MTU) n = MTU;
    int nx = (LB.tail + 1) % CAP; if (nx == LB.head) return -1;   /* full */
    memcpy(LB.buf[LB.tail], p, n); LB.len[LB.tail] = n; LB.tail = nx; return n; }
static int lb_recv(void *c, void *p, int cap) { (void)c;
    if (LB.head == LB.tail) return 0;
    int n = LB.len[LB.head]; if (n > cap) n = cap;
    memcpy(p, LB.buf[LB.head], n); LB.head = (LB.head + 1) % CAP; return n; }
static int lb_link(void *c) { (void)c; return 1; }
static uno_nic_t LBNIC = { NULL, lb_send, lb_recv, lb_link };
uno_nic_t *uno_nic_loopback(void) { LB.head = LB.tail = 0; return &LBNIC; }
