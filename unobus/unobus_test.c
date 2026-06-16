/* unobus_test — enumerate -> bind -> register, then USE a registry-bound service,
 * and show the FDS detect-pin path yields the same registry outcome as a bus walk. */
#include "unobus.h"
#include "unofs.h"          /* reuse the real `block` service interface */
#include <stdio.h>
#include <string.h>

/* a RAM-backed block device a driver publishes (proves the bound service works) */
static uint8_t g_disk[8 * 512];
static int  ram_read(void *c, uint32_t lba, uint32_t n, void *b) { (void)c; memcpy(b, g_disk + lba*512, n*512); return 0; }
static int  ram_write(void *c, uint32_t lba, uint32_t n, const void *b){ (void)c; memcpy(g_disk + lba*512, b, n*512); return 0; }
static uint32_t ram_ss(void *c){ (void)c; return 512; }
static uint32_t ram_sc(void *c){ (void)c; return 8; }
static uno_block_t g_block = { NULL, ram_read, ram_write, ram_ss, ram_sc };

static void *bind_block(const uno_node_t *n) { (void)n; return &g_block; }      /* SCSI/IDE/FDS driver */
static int g_kbd; static void *bind_input(const uno_node_t *n) { (void)n; return &g_kbd; }

static int fails = 0;
static void check(const char *w, int ok){ printf("  [%s] %s\n", ok?"PASS":"FAIL", w); if(!ok) fails++; }

int main(void) {
    strcpy((char*)g_disk + 3*512, "HELLO-FROM-LBA-3");

    /* --- bus-rich path: enumerate a PCI-ish bus, bind drivers, fill the registry --- */
    uno_registry_reset();
    uno_node_t nodes[] = {
        { "pci", 0x10221234, SVC_BLOCK, "ide-disk" },
        { "usb", 0x046d0001, SVC_INPUT, "usb-kbd"  },
    };
    uno_driver_t drivers[] = {
        { SVC_BLOCK, "ide", bind_block },
        { SVC_INPUT, "hid", bind_input },
    };
    int bound = uno_bus_bind(nodes, 2, drivers, 2);
    check("enumerate->bind published 2 services", bound == 2);
    check("registry block provider = ide", uno_provider(SVC_BLOCK) && !strcmp(uno_provider(SVC_BLOCK), "ide"));
    check("registry input present", uno_lookup(SVC_INPUT) != NULL);
    check("no nic bound (capability absent)", uno_lookup(SVC_NIC) == NULL);

    /* use the registry-bound block service to read a sector */
    uno_block_t *blk = (uno_block_t *)uno_lookup(SVC_BLOCK);
    char sec[512] = {0};
    blk->read(blk->ctx, 3, 1, sec);
    check("registry-bound block service reads LBA 3", strncmp(sec, "HELLO-FROM-LBA-3", 16) == 0);

    /* --- scale-down: the FDS 'detect pin' is the SAME binding question --- */
    uno_registry_reset();
    int fds_present = 1;                                  /* answered by a soldered pin, not a bus walk */
    if (fds_present) {
        uno_node_t fds = { "fds-detect", 0, SVC_BLOCK, "famicom-disk" };
        uno_driver_t fdd[] = { { SVC_BLOCK, "fds", bind_block } };
        uno_bus_bind(&fds, 1, fdd, 1);
    }
    check("FDS detect-pin fills the same block slot as a bus walk", uno_lookup(SVC_BLOCK) != NULL);

    printf("\n%s\n", fails ? "FAILURES" : "ALL PASS");
    return fails ? 1 : 0;
}
