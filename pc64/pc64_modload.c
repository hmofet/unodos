/* ===========================================================================
 * UnoDOS/pc64 - the runtime .UNO module loader.
 *
 * Apps are no longer compiled into the kernel image: each one ships as a
 * .UNO file (a flattened PE32+ DLL, produced by tools/mkuno.py) and is
 * loaded from storage on first launch - the modern-PC analogue of the C64
 * port loading .PRG apps through its JMP table.  Search order per app,
 * matching the font loader's convention:
 *
 *     APPS\<NAME>.UNO             on every volume   (the USB/dev layout)
 *     EFI\UNODOS\APPS\<NAME>.UNO  on volumes 1..    (an installed system)
 *
 * Loading = read + CRC check + AllocatePages(EfiLoaderCode) + copy + zero
 * the bss tail + rebase (u32 RVA list of u64 cells) + resolve the named
 * import slots against kExports[] below.  Imports are functions only; the
 * module reaches them through `jmp *slot(%rip)` thunks mkuno.py generated,
 * so there is no PE machinery at runtime and no import libraries at build
 * time.  A module with an import this kernel does not export is refused
 * whole (no half-linked apps); build.sh cross-checks the same list at
 * build time.
 * ======================================================================== */
#include "uno_app.h"        /* AppInterface/KernelApi + mac_compat.h Toolbox */
#include "pc64_fs.h"
#include "uefi.h"
#include "e1000.h"
#include "uno_nic.h"
#include "net.h"
#include "tls.h"
#include <string.h>

void *uno_pc64_st(void);    /* uefi_main.c - the EFI system table */

/* ---- debug log: QEMU debugcon (port 0x402) - METAL-UNSAFE, opt-in only ---- */
#ifdef UNO_DBGCON
static inline void mod_outb(unsigned short port, unsigned char v)
{ __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }
static void mdbg(const char *s) { while (*s) mod_outb(0x402, (unsigned char)*s++); }
#else
static void mdbg(const char *s) { (void)s; }
#endif

/* ---- the kernel export table ----------------------------------------------
 * Everything a .UNO module may import by name.  Functions only (mkuno.py
 * thunks cannot express data imports); keep this the union of what the apps
 * actually reference plus the stable Toolbox/libc surface.  build.sh greps
 * the KX() names out of this file to verify every module import resolves. */
#define KX(n) { #n, (void *)&n }
static const struct { const char *name; void *addr; } kExports[] = {
    /* Toolbox geometry + drawing (mac_compat.c) */
    KX(SetRect),   KX(OffsetRect), KX(InsetRect),  KX(PtInRect),
    KX(PaintRect), KX(FrameRect),  KX(InvertRect), KX(PaintOval),
    KX(FrameOval), KX(MoveTo),     KX(LineTo),     KX(PenNormal),
    KX(PenMode),   KX(RGBForeColor),
    /* Toolbox events / time / memory / misc */
    KX(TickCount), KX(GetMouse),   KX(StillDown),  KX(Random),
    KX(NewPtr),    KX(DisposePtr),
    /* libc (pc64_libc.c) */
    KX(memcpy),    KX(memmove),    KX(memset),     KX(memcmp),
    KX(strlen),    KX(strcpy),     KX(strncpy),    KX(strcat),
    KX(strcmp),    KX(strncmp),
    /* the network stack (Network app) */
    KX(e1000_nic), KX(e1000_mac),
    KX(net_init),  KX(net_poll),   KX(net_link),   KX(net_ip),
    KX(net_dhcp_start), KX(net_dhcp_done),
    KX(net_ping),  KX(net_ping_replied),
    KX(net_tcp_connect), KX(net_tcp_state), KX(net_tcp_send),
    KX(net_tcp_recv),    KX(net_tcp_close),
    KX(net_udp_send),    KX(net_udp_recv),
    KX(tls_connect), KX(tls_read),  KX(tls_write), KX(tls_close),
    KX(tls_cipher),  KX(tls_version), KX(tls_last_error), KX(tls_have_rdrand),
};
#define NEXPORT ((int)(sizeof kExports / sizeof kExports[0]))

static void *kexport(const char *name)
{
    int i;
    for (i = 0; i < NEXPORT; i++)
        if (!strcmp(kExports[i].name, name)) return kExports[i].addr;
    return 0;
}

/* ---- app id -> module file ------------------------------------------------ */
static const char *kModFile[APP_NAPPS] = {
    "SYSINFO.UNO",              /* APP_SYSINFO */
    "CLOCK.UNO",                /* APP_CLOCK   */
    "FILES.UNO",                /* APP_FILES   */
    "NOTEPAD.UNO",              /* APP_NOTEPAD */
    "MUSIC.UNO",                /* APP_MUSIC   */
    "DOSTRIS.UNO",              /* APP_DOSTRIS */
    "OUTLAST.UNO",              /* APP_OUTLAST */
    "PACMAN.UNO",               /* APP_PACMAN  */
    "TRACKER.UNO",              /* APP_TRACKER */
    "PAINT.UNO",                /* APP_PAINT   */
    "THEME.UNO",                /* APP_THEME   */
    "SETTINGS.UNO",             /* APP_SETTINGS */
    "NETWORK.UNO",              /* APP_NETWORK */
    "RUNNER.UNO",               /* APP_RUNNER  */
};

int         uno_mod_count(void)      { return APP_NAPPS; }
const char *uno_mod_file(int proc)
{ return (proc >= 0 && proc < APP_NAPPS) ? kModFile[proc] : 0; }

/* ---- the .UNO container (mirrors tools/mkuno.py) -------------------------- */
#define UNO_MOD_MAGIC 0x314F4E55u          /* 'UNO1' */
typedef struct {
    unsigned int       magic;
    unsigned short     abi, flags;
    unsigned int       entry, mem_size, file_size, nreloc;
    unsigned int       imp_rva, imp_count;
    unsigned long long pref_base;
    unsigned int       crc, rsv;
} UnoModHdr;                                /* 48 bytes */

static unsigned int mod_crc32(const unsigned char *p, long n)
{
    unsigned int c = 0xFFFFFFFFu; long i; int k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

#define MODBUF_MAX (1L << 20)
static unsigned char gModBuf[MODBUF_MAX];

static long mod_read(const char *file, unsigned char *buf, long max)
{
    int nv = uno_fs_volumes(), v;
    char p[64];
    strcpy(p, "APPS\\"); strcat(p, file);
    for (v = 0; v < nv; v++)
        { long n = uno_fs_read(v, p, buf, max); if (n > 0) return n; }
    strcpy(p, "EFI\\UNODOS\\APPS\\"); strcat(p, file);
    for (v = 1; v < nv; v++)
        { long n = uno_fs_read(v, p, buf, max); if (n > 0) return n; }
    return -1;
}

/* EFI page allocation, typed EfiLoaderCode so the image is executable even
 * under firmware NX policies (AllocatePages is a void* slot in uefi.h).
 * Once detached (M3) AllocatePages is gone - a static bump arena takes over
 * (module images are ~40 KB each; 1.5 MB covers the whole roster twice). */
typedef EFI_STATUS (*EFI_ALLOC_PAGES)(UINTN Type, UINTN MemType, UINTN Pages,
                                      unsigned long long *Memory);
typedef EFI_STATUS (*EFI_FREE_PAGES)(unsigned long long Memory, UINTN Pages);
#define EFI_LOADER_CODE 1

int uno_pc64_detached(void);                       /* uefi_main.c (M3) */

/* The post-detach arena is RESERVED while boot services are still live (an
 * EfiLoaderCode allocation stays ours - and stays executable - after EBS;
 * a .bss array might sit in pages the firmware's image protection marked
 * NX).  try_detach() calls the reserve right before ExitBootServices. */
#define MOD_ARENA_PAGES 384u                       /* 1.5 MB */
static unsigned char *gModArena;
static unsigned long  gModArenaUsed;

void uno_modload_reserve(void)
{
    EFI_SYSTEM_TABLE *ST = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    unsigned long long mem = 0;
    if (gModArena || !ST) return;
    if (((EFI_ALLOC_PAGES)ST->BootServices->AllocatePages)
            (0, EFI_LOADER_CODE, MOD_ARENA_PAGES, &mem) == EFI_SUCCESS)
        gModArena = (unsigned char *)(unsigned long long)mem;
}

static unsigned char *mod_alloc(unsigned long np)
{
    EFI_SYSTEM_TABLE *ST = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    unsigned long long mem = 0;
    if (uno_pc64_detached() || !ST) {
        unsigned long bytes = np << 12;
        if (!gModArena || gModArenaUsed + bytes > (MOD_ARENA_PAGES << 12))
            return 0;
        { unsigned char *p = gModArena + gModArenaUsed;
          gModArenaUsed += bytes; return p; }
    }
    if (((EFI_ALLOC_PAGES)ST->BootServices->AllocatePages)
            (0 /*AnyPages*/, EFI_LOADER_CODE, np, &mem) != EFI_SUCCESS)
        return 0;
    return (unsigned char *)(unsigned long long)mem;
}
static void mod_free(unsigned char *base, unsigned long np)
{
    EFI_SYSTEM_TABLE *ST = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    if (!base) return;
    if (gModArena && base >= gModArena &&
        base < gModArena + (MOD_ARENA_PAGES << 12)) {
        /* bump arena: only the most recent allocation can be returned */
        if (base + (np << 12) == gModArena + gModArenaUsed)
            gModArenaUsed -= np << 12;
        return;
    }
    if (ST && !uno_pc64_detached())
        ((EFI_FREE_PAGES)ST->BootServices->FreePages)
            ((unsigned long long)base, np);
}

static UnoAppEntry mod_load(const char *file)
{
    const UnoModHdr *h = (const UnoModHdr *)gModBuf;
    unsigned char *base; unsigned long np;
    const unsigned int *rel; unsigned int i;
    long n = mod_read(file, gModBuf, MODBUF_MAX);

    mdbg("modload: "); mdbg(file); mdbg("\n");
    if (n < (long)sizeof *h)               { mdbg("modload: not found\n"); return 0; }
    if (n >= MODBUF_MAX)                   { mdbg("modload: too big\n");   return 0; }
    if (h->magic != UNO_MOD_MAGIC)         { mdbg("modload: bad magic\n"); return 0; }
    if (h->abi != UNO_ABI_VERSION)         { mdbg("modload: bad abi\n");   return 0; }
    if ((long)sizeof *h + h->file_size + 4ll * h->nreloc != n)
                                           { mdbg("modload: bad size\n");  return 0; }
    if (h->entry >= h->mem_size || h->file_size > h->mem_size)
                                           { mdbg("modload: bad hdr\n");   return 0; }
    if (mod_crc32(gModBuf + sizeof *h, n - (long)sizeof *h) != h->crc)
                                           { mdbg("modload: bad crc\n");   return 0; }

    np = (h->mem_size + 4095u) >> 12;
    base = mod_alloc(np);
    if (!base)                             { mdbg("modload: alloc fail\n"); return 0; }
    memcpy(base, gModBuf + sizeof *h, h->file_size);
    memset(base + h->file_size, 0, h->mem_size - h->file_size);

    /* rebase: each listed RVA is a u64 cell holding a pref_base address */
    rel = (const unsigned int *)(gModBuf + sizeof *h + h->file_size);
    for (i = 0; i < h->nreloc; i++) {
        if (rel[i] + 8u > h->mem_size) { mod_free(base, np); return 0; }
        *(unsigned long long *)(base + rel[i]) +=
            (unsigned long long)base - h->pref_base;
    }

    /* resolve the named import slots: {char name[24]; u64 addr;} records */
    for (i = 0; i < h->imp_count; i++) {
        char *rec = (char *)base + h->imp_rva + 32u * i;
        void *fn;
        if (h->imp_rva + 32u * (i + 1) > h->mem_size) { mod_free(base, np); return 0; }
        rec[23] = 0;
        fn = kexport(rec);
        if (!fn) {
            mdbg("modload: unresolved import "); mdbg(rec); mdbg("\n");
            mod_free(base, np);
            return 0;
        }
        *(unsigned long long *)(rec + 24) = (unsigned long long)fn;
    }

    mdbg("modload: ok\n");
    return (UnoAppEntry)(base + h->entry);
}

/* ---- the kernel-facing hook (cached per app id) --------------------------- */
static UnoAppEntry gEntry[APP_NAPPS];
static char        gTried[APP_NAPPS];

UnoAppEntry uno_load_module(short proc)
{
    if (proc < 0 || proc >= APP_NAPPS || !kModFile[proc]) return 0;
    if (!gTried[proc]) {
        gTried[proc] = 1;
        gEntry[proc] = mod_load(kModFile[proc]);
    }
    return gEntry[proc];
}
