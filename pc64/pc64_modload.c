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
#include "uno_uuiapp.h"     /* the unoui-class module ABI (flags bit 0) */
#include "pc64_fs.h"
#include "fat.h"
#include "pc64_font.h"
#include "uefi.h"
#include "e1000.h"
#include "e1000e.h"
#include "igb.h"
#include "r8169.h"
#include "uno_nic.h"
#include "net.h"
#include "tls.h"
#include "pc64_http.h"     /* pc64_net_up */
#include "iwlwifi.h"
#include "rtl8152.h"
#include "rtwifi.h"
#include "mrvlwifi.h"
#include <string.h>
#include <stdlib.h>

void uno_pc64_delay_ms(int ms);    /* uefi_main.c */

/* shell services (pc64_uui.c) + fb accessors a unoui-class module may import */
struct unoui_theme;
void pc64_shell_add_window(unoui_window *w);
void pc64_shell_del_window(unoui_window *w);      /* remove_window, <=23 chars */
void pc64_shell_focus_window(unoui_window *w);
void pc64_shell_dirty(void);
int  pc64_shell_workarea_w(void);
int  pc64_shell_workarea_h(void);
const struct unoui_theme *pc64_shell_theme(void);
int  pc64_shell_run_user(int vol, const char *path);
int  pc64_shell_font_mono(void);
void pc64_browser_open_path(const char *path);
int  fb_width(void);
int  fb_height(void);

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
    KX(e1000e_nic), KX(e1000e_mac), KX(igb_nic), KX(igb_mac), KX(r8169_nic), KX(r8169_mac),
    KX(net_init),  KX(net_poll),   KX(net_link),   KX(net_ip),
    KX(net_dhcp_start), KX(net_dhcp_done),
    KX(net_ping),  KX(net_ping_replied),
    KX(net_tcp_connect), KX(net_tcp_state), KX(net_tcp_send),
    KX(net_tcp_recv),    KX(net_tcp_close),
    KX(net_udp_send),    KX(net_udp_recv),
    KX(tls_connect), KX(tls_read),  KX(tls_write), KX(tls_close),
    KX(tls_cipher),  KX(tls_version), KX(tls_last_error), KX(tls_have_rdrand),
    /* net bring-up + DNS + CA-validated TLS: a module (Studio's AI assistant)
     * makes its own HTTPS request through these */
    KX(pc64_net_up), KX(net_dns_query), KX(tls_connect_ca), KX(uno_pc64_delay_ms),
    /* Intel WiFi + Realtek USB-ethernet status (Network app readout) */
    KX(iwl_present), KX(iwl_nic),    KX(iwl_mac),   KX(iwl_status_str),
    KX(rtl8152_nic), KX(rtl8152_mac), KX(rtl8152_status),
    /* Realtek + Marvell PCIe WiFi status */
    KX(rtwifi_present),   KX(rtwifi_nic),   KX(rtwifi_mac),   KX(rtwifi_status_str),
    KX(mrvlwifi_present), KX(mrvlwifi_nic), KX(mrvlwifi_mac), KX(mrvlwifi_status_str),
    /* ---- the unoui-class surface (Studio and friends) ------------------- */
    /* toolkit */
    KX(unoui_window_init), KX(unoui_add_label),  KX(unoui_add_button),
    KX(unoui_add_check),   KX(unoui_add_field),  KX(unoui_add_edit),
    KX(unoui_add_textarea),KX(unoui_add_list),   KX(unoui_add_dropdown),
    KX(unoui_add_tabs),    KX(unoui_add_menubar),KX(unoui_add_sep),
    KX(unoui_add_canvas),  KX(unoui_add_vscroll),KX(unoui_add_group),
    KX(unoui_text_init),   KX(unoui_text_set),   KX(unoui_widget_fill),
    KX(unoui_widget_rect), KX(unoui_content_origin),
    /* framebuffer + fonts */
    KX(fb_fill_rect), KX(fb_hline), KX(fb_vline), KX(fb_text),
    KX(fb_text_w),    KX(fb_text_h), KX(fb_width), KX(fb_height),
    KX(uno_font_draw_styled), KX(uno_font_text_w_styled),
    KX(uno_font_height_px),   KX(uno_font_baseline_px), KX(uno_font_active),
    KX(uno_font_push), KX(uno_font_pop),
    /* filesystem: the simple per-volume surface + the rich FAT one */
    KX(uno_fs_volumes), KX(uno_fs_volume_name), KX(uno_fs_list_begin),
    KX(uno_fs_list_get), KX(uno_fs_read), KX(uno_fs_read_at), KX(uno_fs_size),
    KX(uno_fs_write), KX(uno_fs_writable), KX(uno_fs_kind), KX(uno_fs_fat_index),
    KX(uno_fat_list_ex), KX(uno_fat_read), KX(uno_fat_read_at), KX(uno_fat_size),
    KX(uno_fat_write), KX(uno_fat_delete), KX(uno_fat_mkdir), KX(uno_fat_rename),
    /* memory */
    KX(malloc), KX(free),
    /* shell services */
    KX(pc64_shell_add_window), KX(pc64_shell_del_window),
    KX(pc64_shell_focus_window), KX(pc64_shell_dirty),
    KX(pc64_shell_workarea_w), KX(pc64_shell_workarea_h),
    KX(pc64_shell_theme), KX(pc64_shell_run_user), KX(pc64_shell_font_mono),
    KX(pc64_browser_open_path),
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

#define MODBUF_MAX (2L << 20)          /* 2 MB: Studio carries a compiler */
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
#define MOD_ARENA_PAGES 1024u                      /* 4 MB (Studio-sized) */
static unsigned char *gModArena;
static unsigned long  gModArenaUsed;

/* The Studio build-run loop reloads the user's app over and over; a bump
 * arena would leak a slot per rebuild once detached.  A fixed carve-out
 * gives the user app a stable home instead: every reload lands in the same
 * pages, forever. */
#define USER_SLOT_PAGES 128u                       /* 512 KB per user app */
static unsigned char *gUserSlot;

void uno_modload_reserve(void)
{
    EFI_SYSTEM_TABLE *ST = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    unsigned long long mem = 0;
    if (gModArena || !ST) return;
    if (((EFI_ALLOC_PAGES)ST->BootServices->AllocatePages)
            (0, EFI_LOADER_CODE, MOD_ARENA_PAGES + USER_SLOT_PAGES, &mem)
        == EFI_SUCCESS) {
        gModArena = (unsigned char *)(unsigned long long)mem;
        gUserSlot = gModArena + ((unsigned long)MOD_ARENA_PAGES << 12);
    }
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

/* instantiate the module image sitting in gModBuf[0..n).  `at` picks the
 * placement: 0 = the ordinary path (AllocatePages / bump arena), else a
 * fixed region of `atpages` pages (the user slot).  On success *flags_out
 * (if given) carries UnoModHdr.flags. */
static void *mod_instantiate(long n, unsigned short *flags_out,
                             unsigned char *at, unsigned long atpages,
                             unsigned char **base_out, unsigned long *np_out)
{
    const UnoModHdr *h = (const UnoModHdr *)gModBuf;
    unsigned char *base; unsigned long np;
    const unsigned int *rel; unsigned int i;

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
    if (at) {
        if (np > atpages)                  { mdbg("modload: slot full\n"); return 0; }
        base = at;
    } else {
        base = mod_alloc(np);
        if (!base)                         { mdbg("modload: alloc fail\n"); return 0; }
    }
    memcpy(base, gModBuf + sizeof *h, h->file_size);
    memset(base + h->file_size, 0, h->mem_size - h->file_size);

    /* rebase: each listed RVA is a u64 cell holding a pref_base address */
    rel = (const unsigned int *)(gModBuf + sizeof *h + h->file_size);
    for (i = 0; i < h->nreloc; i++) {
        if (rel[i] + 8u > h->mem_size) { if (!at) mod_free(base, np); return 0; }
        *(unsigned long long *)(base + rel[i]) +=
            (unsigned long long)base - h->pref_base;
    }

    /* resolve the named import slots: {char name[24]; u64 addr;} records */
    for (i = 0; i < h->imp_count; i++) {
        char *rec = (char *)base + h->imp_rva + 32u * i;
        void *fn;
        if (h->imp_rva + 32u * (i + 1) > h->mem_size) { if (!at) mod_free(base, np); return 0; }
        rec[23] = 0;
        fn = kexport(rec);
        if (!fn) {
            mdbg("modload: unresolved import "); mdbg(rec); mdbg("\n");
            if (!at) mod_free(base, np);
            return 0;
        }
        *(unsigned long long *)(rec + 24) = (unsigned long long)fn;
    }

    if (flags_out) *flags_out = h->flags;
    if (base_out)  *base_out = base;
    if (np_out)    *np_out = np;
    mdbg("modload: ok\n");
    return base + h->entry;
}

static UnoAppEntry mod_load(const char *file)
{
    unsigned short flags = 0;
    void *e;
    long n = mod_read(file, gModBuf, MODBUF_MAX);
    mdbg("modload: "); mdbg(file); mdbg("\n");
    e = mod_instantiate(n, &flags, 0, 0, 0, 0);
    if (e && (flags & UNO_MODF_UUI)) {
        /* a unoui-class module in the classic roster would be called with
         * the wrong entry signature - refuse it here */
        mdbg("modload: uui module in classic slot\n");
        return 0;
    }
    return (UnoAppEntry)e;
}

/* ---- unoui-class modules (Studio) ----------------------------------------- */
int uno_mod_present(const char *file)
{
    int nv = uno_fs_volumes(), v;
    char p[64];
    strcpy(p, "APPS\\"); strcat(p, file);
    for (v = 0; v < nv; v++)
        if (uno_fs_size(v, p) >= 48) return 1;
    strcpy(p, "EFI\\UNODOS\\APPS\\"); strcat(p, file);
    for (v = 1; v < nv; v++)
        if (uno_fs_size(v, p) >= 48) return 1;
    return 0;
}

UnoUuiEntry uno_mod_load_uui(const char *file)
{
    unsigned short flags = 0;
    void *e;
    long n = mod_read(file, gModBuf, MODBUF_MAX);
    mdbg("modload(uui): "); mdbg(file); mdbg("\n");
    e = mod_instantiate(n, &flags, 0, 0, 0, 0);
    if (e && !(flags & UNO_MODF_UUI)) { mdbg("modload: not a uui module\n"); return 0; }
    return (UnoUuiEntry)e;
}

/* ---- the user-app slot (Studio's build-run loop) --------------------------
 * Loads a just-built module from an explicit volume + path.  Attached, each
 * load gets fresh pages and unload returns them; detached, every load lands
 * in the fixed 512 KB carve-out, so rebuilds never eat the arena. */
static unsigned char *gUserBase;
static unsigned long  gUserNp;

void uno_mod_unload_user(void);

UnoAppEntry uno_mod_load_user(int vol, const char *path)
{
    unsigned short flags = 0;
    void *e;
    long n = uno_fs_read(vol, path, gModBuf, MODBUF_MAX);
    mdbg("modload(user): "); mdbg(path); mdbg("\n");
    if (uno_pc64_detached()) {
        if (!gUserSlot) return 0;
        e = mod_instantiate(n, &flags, gUserSlot, USER_SLOT_PAGES,
                            &gUserBase, &gUserNp);
    } else
        e = mod_instantiate(n, &flags, 0, 0, &gUserBase, &gUserNp);
    if (e && (flags & UNO_MODF_UUI)) {
        /* user apps are classic-tier only (v1) */
        mdbg("modload: user app must be classic tier\n");
        uno_mod_unload_user();
        return 0;
    }
    if (!e) { gUserBase = 0; gUserNp = 0; }
    return (UnoAppEntry)e;
}

void uno_mod_unload_user(void)
{
    if (!gUserBase) return;
    if (!(gUserSlot && gUserBase == gUserSlot))   /* slot loads stay resident */
        mod_free(gUserBase, gUserNp);
    gUserBase = 0; gUserNp = 0;
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
