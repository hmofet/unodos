/* ===========================================================================
 * UnoDOS/pc64 - block-device registry + the firmware-sector fallback backend.
 *
 * The native storage stack (fat.c) sees one flat list of 512 B/s disks.
 * Native controller drivers (ahci.c) register first; then this file wraps any
 * REMAINING firmware EFI Block IO whole-disk handles (skipping disks a native
 * driver already claimed, matched by the controller's PCI device/function in
 * the handle's device path) so NVMe/eMMC/USB disks are still scanned and
 * FAT-mounted by OUR filesystem code - the firmware only moves sectors.
 * ======================================================================== */
#include "uefi.h"
#include "string.h"
#include "blkdev.h"

void *uno_pc64_st(void);             /* uefi_main.c */
int   uno_pc64_detached(void);       /* 1 once ExitBootServices has run (M3) */

#define MAXBLK 12
static uno_bdev g_dev[MAXBLK];
static int      g_ndev;
static int      g_done;

int       uno_blk_count(void)  { return g_ndev; }
uno_bdev *uno_blk_get(int i)   { return (i >= 0 && i < g_ndev) ? &g_dev[i] : 0; }

int uno_blk_register(const uno_bdev *dev)
{
    if (g_ndev >= MAXBLK) return 0;
    g_dev[g_ndev] = *dev;
    g_ndev++;
    return 1;
}

/* ---- EFI Block IO fallback (spec-fixed shapes, same as installer.c) ------- */
typedef struct {
    UINT32 MediaId;
    UINT8  RemovableMedia, MediaPresent, LogicalPartition, ReadOnly, WriteCaching;
    UINT32 BlockSize;
    UINT32 IoAlign;
    UINT64 LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef struct _EFI_BLOCK_IO EFI_BLOCK_IO;
struct _EFI_BLOCK_IO {
    UINT64 Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    EFI_STATUS (*Reset)(EFI_BLOCK_IO *, EFI_BOOLEAN);
    EFI_STATUS (*ReadBlocks)(EFI_BLOCK_IO *, UINT32, UINT64, UINTN, void *);
    EFI_STATUS (*WriteBlocks)(EFI_BLOCK_IO *, UINT32, UINT64, UINTN, void *);
    EFI_STATUS (*FlushBlocks)(EFI_BLOCK_IO *);
};

typedef struct { UINT8 Type, SubType, Length[2]; } DP_NODE;

static EFI_GUID gBlkGuid = { 0x964e5b21, 0x6459, 0x11d2, { 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b } };
static EFI_GUID gDpGuid  = { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b } };

/* first PCI node (type 1 / subtype 1) in a device path -> device, function */
static int dp_pci(DP_NODE *dp, int *dev, int *fn)
{
    int guard = 0;
    while (dp && !(dp->Type == 0x7F && dp->SubType == 0xFF)) {
        int l = (int)dp->Length[0] | ((int)dp->Length[1] << 8);
        if (l < 4 || ++guard > 64) return 0;
        if (dp->Type == 1 && dp->SubType == 1 && l >= 6) {
            const unsigned char *b = (const unsigned char *)dp;
            *fn = b[4]; *dev = b[5];
            return 1;
        }
        dp = (DP_NODE *)((unsigned char *)dp + l);
    }
    return 0;
}

static int fw_read(uno_bdev *d, unsigned long long lba, unsigned int n, void *buf)
{
    EFI_BLOCK_IO *b = (EFI_BLOCK_IO *)d->ctx;
    return b->ReadBlocks(b, b->Media->MediaId, lba, (UINTN)n * 512, buf) == EFI_SUCCESS;
}
static int fw_write(uno_bdev *d, unsigned long long lba, unsigned int n, const void *buf)
{
    EFI_BLOCK_IO *b = (EFI_BLOCK_IO *)d->ctx;
    if (b->Media->ReadOnly) return 0;
    return b->WriteBlocks(b, b->Media->MediaId, lba, (UINTN)n * 512, (void *)buf) == EFI_SUCCESS;
}

static void fw_scan(void)
{
    EFI_SYSTEM_TABLE *ST = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    EFI_BOOT_SERVICES *BS;
    EFI_HANDLE *hs = 0; UINTN n = 0, i;
    int fwidx = 0;

    if (!ST) return;
    BS = ST->BootServices;
    if (BS->LocateHandleBuffer(2, &gBlkGuid, 0, &n, &hs) != EFI_SUCCESS || !hs)
        return;
    for (i = 0; i < n && g_ndev < MAXBLK; i++) {
        EFI_BLOCK_IO *b = 0; void *dpv = 0;
        int pdev = -1, pfn = -1, k, claimed = 0;
        uno_bdev d;
        if (BS->HandleProtocol(hs[i], &gBlkGuid, (void **)&b) != EFI_SUCCESS || !b)
            continue;
        if (!b->Media || b->Media->LogicalPartition || !b->Media->MediaPresent)
            continue;                          /* whole disks only            */
        if (b->Media->BlockSize != 512)
            continue;                          /* fat.c speaks 512 B sectors  */
        if (BS->HandleProtocol(hs[i], &gDpGuid, &dpv) == EFI_SUCCESS && dpv)
            dp_pci((DP_NODE *)dpv, &pdev, &pfn);
        /* skip disks behind a controller a NATIVE driver already claimed */
        for (k = 0; k < g_ndev; k++)
            if (g_dev[k].native && pdev >= 0 &&
                g_dev[k].pci_dev == pdev && g_dev[k].pci_fn == pfn)
                claimed = 1;
        if (claimed)
            continue;
        memset(&d, 0, sizeof d);
        d.native  = 0;
        d.sectors = b->Media->LastBlock + 1;
        d.name[0]='f'; d.name[1]='w'; d.name[2]=(char)('0'+fwidx); d.name[3]=0;
        d.pci_dev = pdev; d.pci_fn = pfn;
        d.ctx     = b;
        d.read    = fw_read;
        d.write   = fw_write;
        if (uno_blk_register(&d)) fwidx++;
    }
    BS->FreePool(hs);
}

void uno_blk_init(void)
{
    if (g_done) return;
    g_done = 1;
    /* ONE driver per controller.  While boot services are live the FIRMWARE
     * owns the AHCI/NVMe controllers, so we must not reprogram their ports
     * (doing so breaks the firmware's own Block IO - it corrupted an installer
     * clone mid-write).  Attached: go through the firmware's Block IO for the
     * sector transport (our FAT code still does all partition + filesystem
     * work).  Detached (post-ExitBootServices, M3): the native drivers are the
     * only ones on the bus, and firmware Block IO is gone. */
    if (uno_pc64_detached())
        uno_ahci_init();             /* native controller ownership (no fw)    */
    else
        fw_scan();                   /* firmware moves sectors; we own the FS  */
}
