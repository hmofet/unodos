/* ===========================================================================
 * UnoDOS/pc64 - install to a local disk (see installer.h).
 *
 * Runs entirely on UEFI boot-services plumbing the firmware already provides:
 *   - Simple File System for the file-level ESP install (FAT write support is
 *     part of every UEFI FAT driver),
 *   - Block IO for the whole-disk clone,
 *   - runtime SetVariable for the Boot#### / BootOrder entry.
 *
 * Safety model:
 *   - the boot USB (the disk the running image came from) is NEVER a target;
 *     it is excluded by device-path prefix match against the boot partition.
 *   - ESP installs create/overwrite only \EFI\UNODOS\* (plus the removable
 *     fallback \EFI\BOOT\BOOTX64.EFI - only when that file does not exist).
 *   - whole-disk installs are gated by the UI's double-confirm and refuse
 *     undersized or non-512-byte-sector targets.
 * ======================================================================== */
#include "uefi.h"
#include "string.h"
#include "installer.h"
#include "unostorage.h"     /* shared CRC-32 (one copy for the GPT checksum) */

/* uefi_main.c */
void *uno_pc64_st(void);
void *uno_pc64_image_handle(void);
int   uno_pc64_detached(void);

/* pc64_modload.c - the .UNO app-module roster (copied on ESP installs) */
int         uno_mod_count(void);
const char *uno_mod_file(int proc);

/* ---- EFI protocol shapes not in uefi.h (natural layout matches spec) ------ */
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

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    void *SystemTable;
    EFI_HANDLE DeviceHandle;                 /* the partition we booted from   */
    void *FilePath, *Reserved;
    UINT32 LoadOptionsSize; void *LoadOptions;
    void *ImageBase; UINT64 ImageSize;
    UINT32 ImageCodeType, ImageDataType; void *Unload;
} EFI_LOADED_IMAGE;

typedef struct { UINT8 Type, SubType, Length[2]; } DP_NODE;   /* device path  */

/* runtime services, with the variable calls typed (uefi_main's copy has them
 * as void* placeholders; same table, same slots) */
typedef struct {
    UINT8 Hdr[24];
    void *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime;
    void *SetVirtualAddressMap, *ConvertPointer;
    EFI_STATUS (*GetVariable)(CHAR16 *, EFI_GUID *, UINT32 *, UINTN *, void *);
    EFI_STATUS (*GetNextVariableName)(UINTN *, CHAR16 *, EFI_GUID *);
    EFI_STATUS (*SetVariable)(CHAR16 *, EFI_GUID *, UINT32, UINTN, void *);
} INST_RTS;

/* EFI_FILE_PROTOCOL / EFI_SIMPLE_FS live in uefi_main.c; redeclare here (the
 * shapes are fixed by the UEFI spec) */
typedef struct _I_FILE I_FILE;
struct _I_FILE {
    UINT64 Revision;
    EFI_STATUS (*Open)(I_FILE *, I_FILE **, CHAR16 *, UINT64, UINT64);
    EFI_STATUS (*Close)(I_FILE *);
    EFI_STATUS (*Delete)(I_FILE *);
    EFI_STATUS (*Read)(I_FILE *, UINTN *, void *);
    EFI_STATUS (*Write)(I_FILE *, UINTN *, void *);
    EFI_STATUS (*GetPosition)(I_FILE *, UINT64 *);
    EFI_STATUS (*SetPosition)(I_FILE *, UINT64);
    EFI_STATUS (*GetInfo)(I_FILE *, EFI_GUID *, UINTN *, void *);
    EFI_STATUS (*SetInfo)(I_FILE *, EFI_GUID *, UINTN, void *);
    EFI_STATUS (*Flush)(I_FILE *);
};
typedef struct _I_SFS I_SFS;
struct _I_SFS { UINT64 Revision; EFI_STATUS (*OpenVolume)(I_SFS *, I_FILE **); };

#define F_READ   1ull
#define F_WRITE  2ull
#define F_CREATE 0x8000000000000000ull
#define A_DIR    0x10ull

static EFI_GUID gSfs   = { 0x964e5b22, 0x6459, 0x11d2, { 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b } };
static EFI_GUID gBlk   = { 0x964e5b21, 0x6459, 0x11d2, { 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b } };
static EFI_GUID gDp    = { 0x09576e91, 0x6d3f, 0x11d2, { 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b } };
static EFI_GUID gLoaded= { 0x5b1b31a1, 0x9562, 0x11d2, { 0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b } };
static EFI_GUID gFsInfo= { 0x09576e93, 0x6d3f, 0x11d2, { 0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b } };
static EFI_GUID gGlobal= { 0x8be4df61, 0x93ca, 0x11d2, { 0xaa,0x0d,0x00,0xe0,0x98,0x03,0x2b,0x8c } };

/* ---- state ---------------------------------------------------------------- */
#define MAXT 12
typedef struct {
    int kind, usable;
    EFI_HANDLE   h;                          /* SFS handle (ESP targets)       */
    EFI_BLOCK_IO *bio;                       /* whole-disk targets             */
    char desc[72];
} target;

static EFI_SYSTEM_TABLE  *iST;
static EFI_BOOT_SERVICES *iBS;
static EFI_HANDLE  g_boot_part;              /* partition we booted from       */
static DP_NODE    *g_boot_dp;                /* its device path (may be NULL)  */
static target      g_t[MAXT];
static int         g_nt;
static char        g_err[96] = "";
static unsigned char g_buf[1u << 20];        /* 1 MiB copy buffer              */
static unsigned char g_gpt[512 * 33];        /* src LBA1 header + 32 entry sectors */
static UINT64      g_src_need;               /* sectors the clone needs        */

const char *uno_inst_error(void) { return g_err; }
static void err(const char *m) { int i; for (i = 0; m[i] && i < 95; i++) g_err[i] = m[i]; g_err[i] = 0; }

/* ---- small helpers -------------------------------------------------------- */
static void w16(CHAR16 *d, const char *s, int max)
{ int i; for (i = 0; i < max - 1 && s[i]; i++) d[i] = (unsigned char)s[i]; d[i] = 0; }

static char *aps(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *apu(char *p, unsigned long long v)
{ char t[24]; int k = 0; if (!v) t[k++] = '0'; while (v) { t[k++] = (char)('0' + v % 10); v /= 10; }
  while (k) *p++ = t[--k]; return p; }
static char *ap_size(char *p, UINT64 bytes)                 /* "7.5 GB" style */
{
    UINT64 mb = bytes >> 20;
    if (mb >= 10240) { p = apu(p, mb >> 10); p = aps(p, " GB"); }
    else if (mb >= 1024) { p = apu(p, mb >> 10); *p++ = '.'; p = apu(p, ((mb & 1023) * 10) >> 10); p = aps(p, " GB"); }
    else { p = apu(p, mb); p = aps(p, " MB"); }
    return p;
}

/* the GPT header/array checksum - now the one shared reflected CRC-32 in
 * unostorage (was a private copy here; byte-identical result). */
static UINT32 crc32(const void *data, UINTN len)
{ return (UINT32)unostorage_crc32(data, (unsigned long)len); }

static UINT32 rd32(const unsigned char *p) { return (UINT32)p[0] | ((UINT32)p[1]<<8) | ((UINT32)p[2]<<16) | ((UINT32)p[3]<<24); }
static UINT64 rd64(const unsigned char *p) { return (UINT64)rd32(p) | ((UINT64)rd32(p+4) << 32); }
static void   wr32(unsigned char *p, UINT32 v) { p[0]=(unsigned char)v; p[1]=(unsigned char)(v>>8); p[2]=(unsigned char)(v>>16); p[3]=(unsigned char)(v>>24); }
static void   wr64(unsigned char *p, UINT64 v) { wr32(p, (UINT32)v); wr32(p+4, (UINT32)(v>>32)); }

/* ---- device paths --------------------------------------------------------- */
static DP_NODE *dp_of(EFI_HANDLE h)
{
    void *dp = 0;
    if (!h || iBS->HandleProtocol(h, &gDp, &dp) != EFI_SUCCESS) return 0;
    return (DP_NODE *)dp;
}
static int dp_len(const DP_NODE *n) { return (int)n->Length[0] | ((int)n->Length[1] << 8); }
static int dp_size(const DP_NODE *dp)                        /* incl. end node */
{
    int sz = 0;
    while (!(dp->Type == 0x7F && dp->SubType == 0xFF)) {
        int l = dp_len(dp);
        if (l < 4 || sz > 4096) return sz + 4;               /* corrupt: stop  */
        sz += l; dp = (const DP_NODE *)((const unsigned char *)dp + l);
    }
    return sz + 4;
}
/* is `pre` (a whole-disk path) a prefix of `full` (a partition path)? */
static int dp_prefix(const DP_NODE *pre, const DP_NODE *full)
{
    int psz = dp_size(pre) - 4, fsz = dp_size(full) - 4;
    if (!psz || psz > fsz) return 0;
    return memcmp(pre, full, (size_t)psz) == 0;
}

/* ---- bind ----------------------------------------------------------------- */
static int bind(void)
{
    EFI_LOADED_IMAGE *li = 0;
    if (iST) return 1;
    iST = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    if (!iST) { err("no system table"); return 0; }
    iBS = iST->BootServices;
    if (uno_pc64_image_handle() &&
        iBS->HandleProtocol((EFI_HANDLE)uno_pc64_image_handle(), &gLoaded,
                            (void **)&li) == EFI_SUCCESS && li)
        g_boot_part = li->DeviceHandle;
    g_boot_dp = dp_of(g_boot_part);
    return 1;
}

/* ---- source GPT (for the whole-disk clone) -------------------------------- */
static DP_NODE *g_src_dp;                    /* boot USB whole-disk devpath    */

static EFI_BLOCK_IO *src_disk(void)
{
    /* the whole-disk Block IO whose path is a prefix of the boot partition's */
    EFI_HANDLE *hs = 0; UINTN n = 0, i; EFI_BLOCK_IO *found = 0;
    if (!g_boot_dp) return 0;
    if (iBS->LocateHandleBuffer(2, &gBlk, 0, &n, &hs) != EFI_SUCCESS || !hs) return 0;
    for (i = 0; i < n && !found; i++) {
        EFI_BLOCK_IO *b = 0; DP_NODE *dp;
        if (iBS->HandleProtocol(hs[i], &gBlk, (void **)&b) != EFI_SUCCESS || !b) continue;
        if (!b->Media || b->Media->LogicalPartition || !b->Media->MediaPresent) continue;
        dp = dp_of(hs[i]);
        if (dp && dp_prefix(dp, g_boot_dp)) { found = b; g_src_dp = dp; }
    }
    iBS->FreePool(hs);
    return found;
}

static int read_src_gpt(EFI_BLOCK_IO *src)
{
    UINT64 elba; UINT32 num, esz; UINTN i;
    if (!src || src->Media->BlockSize != 512) { err("source is not 512 B/sector"); return 0; }
    if (src->ReadBlocks(src, src->Media->MediaId, 1, 512, g_gpt) != EFI_SUCCESS)
        { err("read source GPT failed"); return 0; }
    if (memcmp(g_gpt, "EFI PART", 8) != 0) { err("no GPT on the boot disk"); return 0; }
    elba = rd64(g_gpt + 72); num = rd32(g_gpt + 80); esz = rd32(g_gpt + 84);
    /* num/esz come from the boot medium's GPT (attacker-controlled). The 32-bit
     * product num*esz wrapped past the sizeof guard, giving an OOB read in the
     * entry loop below. Bound esz to the spec range (128..512, multiple of 8) and
     * do the size math in 64-bit. */
    if (esz < 128 || esz > 512 || (esz & 7)) { err("bad GPT entry size"); return 0; }
    if ((UINT64)num * esz > (UINT64)(sizeof g_gpt - 512)) { err("GPT entry table too big"); return 0; }
    if (src->ReadBlocks(src, src->Media->MediaId, elba,
            (UINTN)(((UINT64)num * esz + 511) & ~511ull), g_gpt + 512)
            != EFI_SUCCESS) { err("read GPT entries failed"); return 0; }
    /* the clone extent = end of the last used partition */
    g_src_need = 0;
    for (i = 0; i < num; i++) {
        const unsigned char *e = g_gpt + 512 + i * esz;
        static const unsigned char zero[16] = { 0 };
        if (memcmp(e, zero, 16) == 0) continue;              /* unused entry   */
        { UINT64 last = rd64(e + 40); if (last + 1 > g_src_need) g_src_need = last + 1; }
    }
    if (!g_src_need) { err("source GPT has no partitions"); return 0; }
    return 1;
}

/* ---- scan ----------------------------------------------------------------- */
static void desc_esp(target *t)
{
    I_SFS *sfs = 0; I_FILE *root = 0, *d = 0; char *p = t->desc;
    static unsigned char fi[512];
    UINT64 vsz = 0; char label[20]; int has_efi = 0;
    label[0] = 0;
    if (iBS->HandleProtocol(t->h, &gSfs, (void **)&sfs) == EFI_SUCCESS &&
        sfs->OpenVolume(sfs, &root) == EFI_SUCCESS && root) {
        UINTN sz = sizeof fi; CHAR16 wn[8];
        if (root->GetInfo(root, &gFsInfo, &sz, fi) == EFI_SUCCESS && sz >= 40) {
            const CHAR16 *wl = (const CHAR16 *)(fi + 36); int i;
            vsz = rd64(fi + 16);                              /* VolumeSize    */
            for (i = 0; i < 19 && wl[i]; i++) label[i] = (wl[i] > 31 && wl[i] < 127) ? (char)wl[i] : '?';
            label[i] = 0;
        }
        w16(wn, "EFI", 8);
        if (root->Open(root, &d, wn, F_READ, 0) == EFI_SUCCESS && d) { has_efi = 1; d->Close(d); }
        root->Close(root);
    }
    p = aps(p, "Volume ");
    if (label[0]) { *p++ = '"'; p = aps(p, label); p = aps(p, "\" "); }
    p = ap_size(p, vsz);
    p = aps(p, has_efi ? "  ESP (has \\EFI)" : "  FAT");
    p = aps(p, "  [keeps data]");
    *p = 0;
    t->usable = 1;
}

static void desc_disk(target *t)
{
    char *p = t->desc;
    UINT64 bytes = (t->bio->Media->LastBlock + 1) * (UINT64)t->bio->Media->BlockSize;
    p = aps(p, "Disk ");
    p = ap_size(p, bytes);
    p = aps(p, t->bio->Media->RemovableMedia ? "  removable" : "  fixed");
    t->usable = 1;
    if (t->bio->Media->BlockSize != 512)            { p = aps(p, "  [not 512B/s]"); t->usable = 0; }
    else if (t->bio->Media->ReadOnly)               { p = aps(p, "  [read-only]");  t->usable = 0; }
    else if (g_src_need && t->bio->Media->LastBlock < g_src_need + 33)
                                                    { p = aps(p, "  [too small]");  t->usable = 0; }
    else p = aps(p, "  [ERASES ALL]");
    *p = 0;
}

int uno_inst_scan(void)
{
    EFI_HANDLE *hs = 0; UINTN n = 0, i;
    EFI_BLOCK_IO *src;
    g_nt = 0; g_err[0] = 0;
    if (uno_pc64_detached()) {              /* fw SFS/Block IO/handles are gone */
        err("Install needs the firmware - reboot to install");
        return 0;
    }
    if (!bind()) return 0;

    src = src_disk();                       /* also powers the size check      */
    if (src) read_src_gpt(src);             /* soft-fail: ESP installs still OK */
    g_err[0] = 0;                           /* scan itself hasn't failed       */

    /* ESP / FAT volume targets (file-level, non-destructive) */
    if (iBS->LocateHandleBuffer(2, &gSfs, 0, &n, &hs) == EFI_SUCCESS && hs) {
        for (i = 0; i < n && g_nt < MAXT; i++) {
            DP_NODE *dp;
            if (hs[i] == g_boot_part) continue;              /* the boot USB   */
            dp = dp_of(hs[i]);
            if (dp && g_src_dp && dp_prefix(g_src_dp, dp))
                continue;                    /* another partition of the USB   */
            g_t[g_nt].kind = UNO_INST_ESP;
            g_t[g_nt].h = hs[i];
            g_t[g_nt].bio = 0;
            desc_esp(&g_t[g_nt]);
            g_nt++;
        }
        iBS->FreePool(hs);
    }

    /* whole-disk targets (destructive clone) */
    hs = 0; n = 0;
    if (iBS->LocateHandleBuffer(2, &gBlk, 0, &n, &hs) == EFI_SUCCESS && hs) {
        for (i = 0; i < n && g_nt < MAXT; i++) {
            EFI_BLOCK_IO *b = 0; DP_NODE *dp;
            if (iBS->HandleProtocol(hs[i], &gBlk, (void **)&b) != EFI_SUCCESS || !b) continue;
            if (!b->Media || b->Media->LogicalPartition || !b->Media->MediaPresent) continue;
            dp = dp_of(hs[i]);
            if (dp && g_boot_dp && dp_prefix(dp, g_boot_dp)) continue;   /* boot USB */
            if (b->Media->LastBlock < 2048) continue;        /* tiny/virtual    */
            g_t[g_nt].kind = UNO_INST_DISK;
            g_t[g_nt].h = hs[i];
            g_t[g_nt].bio = b;
            desc_disk(&g_t[g_nt]);
            g_nt++;
        }
        iBS->FreePool(hs);
    }
    return g_nt;
}

const char *uno_inst_desc(int i)  { return (i >= 0 && i < g_nt) ? g_t[i].desc : ""; }
int uno_inst_kind(int i)          { return (i >= 0 && i < g_nt) ? g_t[i].kind : -1; }
int uno_inst_usable(int i)        { return (i >= 0 && i < g_nt) ? g_t[i].usable : 0; }

/* ---- file-level ESP install ----------------------------------------------- */
static I_FILE *vol_root(EFI_HANDLE h)
{
    I_SFS *sfs = 0; I_FILE *root = 0;
    if (iBS->HandleProtocol(h, &gSfs, (void **)&sfs) != EFI_SUCCESS) return 0;
    if (sfs->OpenVolume(sfs, &root) != EFI_SUCCESS) return 0;
    return root;
}

static I_FILE *open_path(I_FILE *root, const char *path, UINT64 mode, UINT64 attr)
{
    CHAR16 wn[96]; I_FILE *f = 0;
    w16(wn, path, 96);
    if (root->Open(root, &f, wn, mode, attr) != EFI_SUCCESS) return 0;
    return f;
}

static int mkdir_p(I_FILE *root, const char *path)   /* single level */
{
    I_FILE *d = open_path(root, path, F_READ | F_WRITE | F_CREATE, A_DIR);
    if (!d) return 0;
    d->Close(d);
    return 1;
}

/* copy src_root:src_path -> dst_root:dst_path (overwrite).  0 = src missing
 * (soft), -1 = write error (hard), 1 = copied. */
static int copy_file(I_FILE *sroot, const char *src_path,
                     I_FILE *droot, const char *dst_path)
{
    I_FILE *s, *d;
    s = open_path(sroot, src_path, F_READ, 0);
    if (!s) return 0;
    /* delete any existing file so the copy can't land on a longer stale one */
    d = open_path(droot, dst_path, F_READ | F_WRITE, 0);
    if (d) d->Delete(d);                                    /* Delete closes  */
    d = open_path(droot, dst_path, F_READ | F_WRITE | F_CREATE, 0);
    if (!d) { s->Close(s); return -1; }
    for (;;) {
        UINTN rn = sizeof g_buf, wn;
        if (s->Read(s, &rn, g_buf) != EFI_SUCCESS || rn == 0) break;
        wn = rn;
        if (d->Write(d, &wn, g_buf) != EFI_SUCCESS || wn != rn) {
            d->Close(d); s->Close(s); return -1;
        }
    }
    d->Flush(d); d->Close(d); s->Close(s);
    return 1;
}

static int file_exists(I_FILE *root, const char *path)
{
    I_FILE *f = open_path(root, path, F_READ, 0);
    if (!f) return 0;
    f->Close(f);
    return 1;
}

/* ---- boot variables ------------------------------------------------------- */
static INST_RTS *rts(void) { return (INST_RTS *)iST->RuntimeServices; }

static void bootname(CHAR16 *wn, int idx)
{
    static const char hx[] = "0123456789ABCDEF";
    wn[0]='B'; wn[1]='o'; wn[2]='o'; wn[3]='t';
    wn[4]=(CHAR16)(unsigned char)hx[(idx >> 12) & 15];
    wn[5]=(CHAR16)(unsigned char)hx[(idx >> 8) & 15];
    wn[6]=(CHAR16)(unsigned char)hx[(idx >> 4) & 15];
    wn[7]=(CHAR16)(unsigned char)hx[idx & 15];
    wn[8]=0;
}

/* find a Boot#### slot: reuse one whose description is "UnoDOS", else first
 * free.  Returns the index, or -1. */
static int boot_slot(void)
{
    static unsigned char v[512];
    CHAR16 wn[9]; int idx, freeslot = -1;
    for (idx = 0; idx < 0x100; idx++) {
        UINTN sz = sizeof v; EFI_STATUS s;
        bootname(wn, idx);
        s = rts()->GetVariable(wn, &gGlobal, 0, &sz, v);
        if (s == EFI_SUCCESS && sz > 6 + 14) {
            const CHAR16 *d = (const CHAR16 *)(v + 6);       /* description   */
            if (d[0]=='U'&&d[1]=='n'&&d[2]=='o'&&d[3]=='D'&&d[4]=='O'&&d[5]=='S'&&d[6]==0)
                return idx;                                  /* reuse ours    */
        } else if (freeslot < 0) {
            freeslot = idx;                                  /* first hole    */
        }
    }
    return freeslot;
}

/* Build + write Boot#### pointing at \EFI\UNODOS\BOOTX64.EFI via the given
 * device path prefix (partition path, or a hand-built HD() node), then splice
 * it into BootOrder. */
static int write_boot_entry(const DP_NODE *prefix, int prefix_sz, int make_default)
{
    static unsigned char lo[1024];
    static const char *fp = "\\EFI\\UNODOS\\BOOTX64.EFI";
    unsigned char *p = lo;
    CHAR16 wn[16]; int i, idx, fplen, fnode;
    UINTN losz;

    idx = boot_slot();
    if (idx < 0) { err("no free Boot#### slot"); return 0; }

    fplen = 0; while (fp[fplen]) fplen++;
    fnode = 4 + 2 * (fplen + 1);                             /* File() node    */

    /* S-INST-07: the prefix is a firmware-supplied device path of unbounded
     * length; the fixed 1024-byte lo[] plus everything we append must fit, or
     * the memcpy below overflows into whatever follows in .bss. Total = 4
     * (attrs) + 2 (len) + 14 (desc "UnoDOS\0\0") + prefix + fnode + 4 (end). */
    /* unsigned compare so a pathological prefix_sz can't wrap the sum negative
     * and slip past the bound (review: low-sev but free to close) */
    if (prefix_sz < 0 ||
        (size_t)(4 + 2 + 14 + fnode + 4) + (size_t)prefix_sz > sizeof lo) {
        err("boot device path too long for the entry buffer");
        return 0;
    }

    wr32(p, 1); p += 4;                                      /* ACTIVE         */
    p[0] = (unsigned char)(prefix_sz + fnode + 4);           /* FilePathListLength */
    p[1] = (unsigned char)((prefix_sz + fnode + 4) >> 8);
    p += 2;
    { static const char *d = "UnoDOS"; for (i = 0; d[i]; i++) { *p++ = (unsigned char)d[i]; *p++ = 0; } *p++ = 0; *p++ = 0; }
    memcpy(p, prefix, (size_t)prefix_sz); p += prefix_sz;
    *p++ = 4; *p++ = 4;                                      /* media / file   */
    *p++ = (unsigned char)fnode; *p++ = (unsigned char)(fnode >> 8);
    for (i = 0; i <= fplen; i++) { *p++ = (unsigned char)fp[i]; *p++ = 0; }
    *p++ = 0x7F; *p++ = 0xFF; *p++ = 4; *p++ = 0;            /* end node       */
    losz = (UINTN)(p - lo);

    bootname(wn, idx);
    if (rts()->SetVariable(wn, &gGlobal, 0x7 /*NV|BS|RT*/, losz, lo) != EFI_SUCCESS) {
        err("SetVariable Boot#### failed");
        return 0;
    }

    /* BootOrder: drop existing occurrences of idx, then prepend/append */
    {
        static UINT16 bo[130], nbo[130];
        UINTN sz = sizeof(UINT16) * 128; int n = 0, m = 0;
        w16(wn, "BootOrder", 16);
        if (rts()->GetVariable(wn, &gGlobal, 0, &sz, bo) == EFI_SUCCESS)
            n = (int)(sz / 2);
        if (make_default) nbo[m++] = (UINT16)idx;
        for (i = 0; i < n && m < 128; i++)
            if (bo[i] != (UINT16)idx) nbo[m++] = bo[i];
        if (!make_default && m < 128) nbo[m++] = (UINT16)idx;
        if (rts()->SetVariable(wn, &gGlobal, 0x7, (UINTN)m * 2, nbo) != EFI_SUCCESS) {
            err("SetVariable BootOrder failed");
            return 0;
        }
    }
    return 1;
}

/* ---- install: ESP (non-destructive) --------------------------------------- */
static int install_esp(target *t, int make_default,
                       void (*progress)(int, const char *))
{
    static const char *aux[] = { "CHICAGO.TTF", "SANS.TTF", "MONO.TTF", "UBUNTU.TTF",
                                 "HELLO.MD", "PAGE.HTML" };
    char dst[64]; int i; I_FILE *sroot, *droot;
    DP_NODE *dp;

    sroot = vol_root(g_boot_part);
    if (!sroot) { err("cannot open the boot volume"); return 0; }
    droot = vol_root(t->h);
    if (!droot) { sroot->Close(sroot); err("cannot open the target volume"); return 0; }

    if (progress) progress(5, "Creating \\EFI\\UNODOS");
    if (!mkdir_p(droot, "EFI") || !mkdir_p(droot, "EFI\\UNODOS")) {
        err("mkdir \\EFI\\UNODOS failed (volume read-only?)");
        goto fail;
    }

    if (progress) progress(15, "Copying BOOTX64.EFI");
    if (copy_file(sroot, "EFI\\BOOT\\BOOTX64.EFI", droot, "EFI\\UNODOS\\BOOTX64.EFI") != 1) {
        err("copying BOOTX64.EFI failed");
        goto fail;
    }

    for (i = 0; i < (int)(sizeof aux / sizeof aux[0]); i++) {
        char *p = aps(dst, "EFI\\UNODOS\\"); p = aps(p, aux[i]); *p = 0;
        if (progress) progress(25 + i * 10, aux[i]);
        if (copy_file(sroot, aux[i], droot, dst) < 0) {      /* missing = soft */
            err("copying a support file failed");
            goto fail;
        }
    }

    /* the .UNO app modules: source is APPS\ on a dev/USB volume, or
     * EFI\UNODOS\APPS\ when installing from an already-installed system */
    if (progress) progress(78, "App modules");
    if (!mkdir_p(droot, "EFI\\UNODOS\\APPS")) {
        err("mkdir \\EFI\\UNODOS\\APPS failed");
        goto fail;
    }
    for (i = 0; i < uno_mod_count(); i++) {
        const char *f = uno_mod_file(i);
        char src[64], *p;
        int r;
        if (!f) continue;
        p = aps(src, "APPS\\"); p = aps(p, f); *p = 0;
        p = aps(dst, "EFI\\UNODOS\\APPS\\"); p = aps(p, f); *p = 0;
        r = copy_file(sroot, src, droot, dst);
        if (r == 0) {                                        /* try installed layout */
            p = aps(src, "EFI\\UNODOS\\APPS\\"); p = aps(p, f); *p = 0;
            r = copy_file(sroot, src, droot, dst);
        }
        if (r < 0) {                                         /* missing = soft */
            err("copying an app module failed");
            goto fail;
        }
    }

    /* removable-media fallback: only where no other OS owns it */
    if (!file_exists(droot, "EFI\\BOOT\\BOOTX64.EFI")) {
        if (progress) progress(88, "Fallback \\EFI\\BOOT");
        if (mkdir_p(droot, "EFI\\BOOT"))
            copy_file(sroot, "EFI\\BOOT\\BOOTX64.EFI", droot, "EFI\\BOOT\\BOOTX64.EFI");
    }

    sroot->Close(sroot); droot->Close(droot);

    if (progress) progress(94, "Boot entry");
    dp = dp_of(t->h);
    if (!dp) { err("target has no device path"); return 0; }
    if (!write_boot_entry(dp, dp_size(dp) - 4, make_default)) return 0;
    if (progress) progress(100, "Done");
    return 1;

fail:
    sroot->Close(sroot); droot->Close(droot);
    return 0;
}

/* ---- install: whole-disk clone (destructive) ------------------------------ */
static int install_disk(target *t, int make_default,
                        void (*progress)(int, const char *))
{
    EFI_BLOCK_IO *src = src_disk(), *dst = t->bio;
    UINT64 lba, dlast; UINT32 num, esz, ecrc; UINTN esec;
    static unsigned char hdr[512];

    if (!src) { err("cannot find the boot disk"); return 0; }
    if (!read_src_gpt(src)) return 0;
    dlast = dst->Media->LastBlock;
    if (dst->Media->BlockSize != 512) { err("target is not 512 B/sector"); return 0; }
    if (dlast < g_src_need + 33) { err("target disk is too small"); return 0; }

    /* 1. raw copy LBA 0 .. last used partition sector */
    for (lba = 0; lba < g_src_need; ) {
        UINTN nsec = sizeof g_buf / 512;
        if (lba + nsec > g_src_need) nsec = (UINTN)(g_src_need - lba);
        if (src->ReadBlocks(src, src->Media->MediaId, lba, nsec * 512, g_buf) != EFI_SUCCESS)
            { err("read from the boot disk failed"); return 0; }
        if (dst->WriteBlocks(dst, dst->Media->MediaId, lba, nsec * 512, g_buf) != EFI_SUCCESS)
            { err("write to the target disk failed"); return 0; }
        lba += nsec;
        if (progress) progress(3 + (int)(lba * 87 / g_src_need), "Copying system");
    }

    /* 2. backup GPT at the target's real end + patched primary */
    if (progress) progress(92, "Writing GPT");
    num = rd32(g_gpt + 80); esz = rd32(g_gpt + 84);
    esec = ((UINTN)num * esz + 511) / 512;
    ecrc = crc32(g_gpt + 512, (UINTN)num * esz);

    /* backup entries end at dlast-1; backup header at dlast */
    if (dst->WriteBlocks(dst, dst->Media->MediaId, dlast - esec, esec * 512, g_gpt + 512)
            != EFI_SUCCESS) { err("write backup GPT entries failed"); return 0; }

    memcpy(hdr, g_gpt, 512);
    wr64(hdr + 24, dlast);                    /* MyLBA                        */
    wr64(hdr + 32, 1);                        /* AlternateLBA -> primary      */
    wr64(hdr + 48, dlast - esec - 1);         /* LastUsableLBA                */
    wr64(hdr + 72, dlast - esec);             /* PartitionEntryLBA            */
    wr32(hdr + 88, ecrc);
    wr32(hdr + 16, 0); wr32(hdr + 16, crc32(hdr, rd32(hdr + 12)));
    if (dst->WriteBlocks(dst, dst->Media->MediaId, dlast, 512, hdr) != EFI_SUCCESS)
        { err("write backup GPT header failed"); return 0; }

    memcpy(hdr, g_gpt, 512);
    wr64(hdr + 32, dlast);                    /* AlternateLBA -> backup       */
    wr64(hdr + 48, dlast - esec - 1);         /* LastUsableLBA                */
    wr32(hdr + 88, ecrc);
    wr32(hdr + 16, 0); wr32(hdr + 16, crc32(hdr, rd32(hdr + 12)));
    if (dst->WriteBlocks(dst, dst->Media->MediaId, 1, 512, hdr) != EFI_SUCCESS)
        { err("write primary GPT header failed"); return 0; }
    dst->FlushBlocks(dst);

    /* 3. boot entry via a hand-built HD() short-form path (the firmware has
     *    not re-read the new partition table yet, so there is no partition
     *    handle to take a device path from) */
    if (progress) progress(96, "Boot entry");
    {
        unsigned char hd[42];
        const unsigned char *e1 = g_gpt + 512;               /* partition 1    */
        hd[0] = 4; hd[1] = 1; hd[2] = 42; hd[3] = 0;         /* Media/HD, len  */
        wr32(hd + 4, 1);                                     /* partition #    */
        memcpy(hd + 8,  e1 + 32, 8);                         /* start LBA      */
        { UINT64 sz = rd64(e1 + 40) - rd64(e1 + 32) + 1; wr64(hd + 16, sz); }
        memcpy(hd + 24, e1 + 16, 16);                        /* unique GUID    */
        hd[40] = 2;                                          /* MBRType: GPT   */
        hd[41] = 2;                                          /* SigType: GUID  */
        if (!write_boot_entry((const DP_NODE *)hd, 42, make_default)) return 0;
    }
    if (progress) progress(100, "Done");
    return 1;
}

int uno_inst_install(int i, int make_default,
                     void (*progress)(int, const char *))
{
    g_err[0] = 0;
    if (uno_pc64_detached()) { err("Install needs the firmware - reboot to install"); return 0; }
    if (!bind()) return 0;
    if (i < 0 || i >= g_nt) { err("no target selected"); return 0; }
    if (!g_t[i].usable) { err("target not usable (see its listing)"); return 0; }
    return g_t[i].kind == UNO_INST_ESP
        ? install_esp(&g_t[i], make_default, progress)
        : install_disk(&g_t[i], make_default, progress);
}
