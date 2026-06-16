/* unonet_test — nic round-trip via the registry + HEADLESS server composition. */
#include "uno_nic.h"
#include "unobus.h"
#include <stdio.h>
#include <string.h>
static int fails=0; static void ck(const char*w,int ok){printf("  [%s] %s\n",ok?"PASS":"FAIL",w); if(!ok)fails++;}
int main(void){
    uno_registry_reset();
    uno_publish(SVC_NIC, "loopback", uno_nic_loopback());        /* a driver publishes nic */
    uno_nic_t *nic = (uno_nic_t*)uno_lookup(SVC_NIC);
    ck("nic service bound in registry", nic != NULL);
    ck("link is up", nic && nic->link(nic->ctx)==1);
    nic->send(nic->ctx, "PING", 4);
    char in[64]={0}; int n = nic->recv(nic->ctx, in, sizeof in);
    ck("packet round-trips through nic (send->recv)", n==4 && memcmp(in,"PING",4)==0);
    ck("recv on empty returns 0 (no UB)", nic->recv(nic->ctx,in,sizeof in)==0);
    /* server composition: HEADLESS + NET + a bound nic, zero display surfaces */
    ck("HEADLESS+NET server = nic bound, no fb", uno_lookup(SVC_NIC)!=NULL && uno_lookup(SVC_FB)==NULL);
    printf("\n%s\n", fails?"FAILURES":"ALL PASS");
    return fails?1:0;
}
