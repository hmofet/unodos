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
#include "pyhost.h"     /* Python-runtime + Python-app module tiers */
#include "unoauto.h"    /* mod.load / mod.unload tap points (no-op in prod) */
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
#include "fb.h"             /* the full framebuffer surface (Python bindings) */
#include "unosound.h"       /* uno_seq_* audio */
#include "uno3d.h"          /* u3d_* 3D */
#include "unoscript.h"      /* the production scripting surface: usc_ + unosec_ */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

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
const char *pc64_shell_py_error(void);
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
#ifdef UNO_DEBUG
/* the unoautomate DRIVE surface exported below (unoauto_* come from
 * unoauto.h; the rest are debug-only accessors with no public header) */
long unoauto_deadline_left(void);        /* 23-char alias (unoauto.c) */
void uno_pc64_inject_key(int scan, int uni, int ctrl);
void uno_pc64_inject_pointer(int x, int y, int btn);
int  pc64_shell_app_count(void);
int  pc64_shell_launch(int a);
void pc64_shell_close_top(void);
int  pc64_shell_win_count(void);
const char *pc64_shell_win_title(int i);
int  pc64_shell_win_focused(int i);
void uno_pc64_shutdown(void);
unsigned long long uno_dbg_uptime_ms(void);
/* unoautomate remote channel (unoauto_remote.c) - Python-visible link ops */
int  unoauto_remote_active(void);
int  unoauto_remote_send(const char *type, const char *text);
int  unoauto_remote_recv(char *buf, int cap);
void unoauto_remote_stop(void);
#endif
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
    KX(net_init),  KX(net_poll),   KX(net_link),   KX(net_ip),   KX(net_gw),
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
    KX(fb_fill_rect), KX(fb_hline), KX(fb_vline), KX(fb_blit), KX(fb_text),
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
    KX(pc64_browser_open_path), KX(pc64_shell_py_error),
    /* ---- Python runtime (PYRT.UNO) surface ------------------------------- *
     * Appended at the tail so a concurrent kExports edit (Wi-Fi) merges
     * cleanly.  Exposes the full audio/3D/framebuffer/filesystem platform to
     * any module (not just Python), plus the libc/libm the interpreter links. */
    /* unosound */
    KX(uno_seq_init), KX(uno_seq_beep), KX(uno_seq_play), KX(uno_seq_stop),
    KX(uno_seq_playing), KX(uno_seq_tick), KX(uno_seq_backend),
    /* uno3d */
    KX(u3d_init), KX(u3d_shutdown), KX(u3d_begin), KX(u3d_end), KX(u3d_present),
    KX(u3d_perspective), KX(u3d_load_identity), KX(u3d_translate), KX(u3d_scale),
    KX(u3d_rotate_x), KX(u3d_rotate_y), KX(u3d_rotate_z), KX(u3d_triangles),
    KX(u3d_last_tris),
    /* framebuffer remainder */
    KX(fb_clear), KX(fb_pixel), KX(fb_frame_rect), KX(fb_invert_rect),
    KX(fb_blend_rect), KX(fb_grad_v), KX(fb_round_rect), KX(fb_set_clip),
    KX(fb_reset_clip), KX(fb_big_text), KX(fb_glyph),
    /* unoui widget gaps */
    KX(unoui_add_radio), KX(unoui_add_progress), KX(unoui_add_slider),
    KX(unoui_add_spinner), KX(unoui_add_hscroll), KX(unoui_add_icon),
    /* libc the interpreter links */
    KX(realloc), KX(calloc), KX(memchr), KX(strstr), KX(strrchr), KX(strspn),
    KX(strchr), KX(qsort), KX(abort), KX(atoi), KX(llabs),
    KX(snprintf), KX(vsnprintf),
    KX(strtol), KX(strtoul), KX(strtoll), KX(strtoull),
    /* single-precision libm */
    KX(sinf), KX(cosf), KX(tanf), KX(asinf), KX(acosf), KX(atanf), KX(atan2f),
    KX(sinhf), KX(coshf), KX(tanhf), KX(expf), KX(exp2f), KX(expm1f),
    KX(logf), KX(log2f), KX(log10f), KX(log1pf), KX(powf), KX(sqrtf), KX(cbrtf),
    KX(floorf), KX(ceilf), KX(truncf), KX(roundf), KX(fabsf), KX(fmodf),
    KX(copysignf), KX(ldexpf), KX(frexpf), KX(modff), KX(nearbyintf), KX(rintf),
    /* ---- unoscript: the PRODUCTION scripting surface ----------------------
     * PYRT.UNO's `unoscript` module (upy_port/mod_unoscript.c) binds these;
     * they ship in every build (unlike the debug-only DRIVE symbols below).
     * The unosec_* seam is adjudicated by unosecure; the usc_* calls are the
     * capability-gated surface ops. */
    KX(unoscript_available), KX(unoscript_cap_name), KX(unoscript_cap_tier),
    KX(unosec_current_user), KX(unosec_present), KX(unosec_request),
    KX(usc_ui_pointer), KX(usc_ui_key), KX(usc_ui_screen_text),
    KX(usc_ui_clipboard_get), KX(usc_ui_clipboard_set),
    KX(usc_app_count), KX(usc_app_launch), KX(usc_app_close_top),
    KX(usc_proc_list), KX(usc_mem_read), KX(usc_mem_write),
    KX(usc_io_in), KX(usc_io_out), KX(usc_power),
#ifdef UNO_DEBUG
    /* ---- unoautomate DRIVE surface (debug builds only) --------------------
     * The `unoauto` Python module (upy_port/mod_unoauto.c) binds these; the
     * prod PYRT compiles that module to stubs, so a prod PYRT.UNO never
     * imports them and the prod export table stays clean. */
    KX(unoauto_log), KX(unoauto_probe), KX(unoauto_deadline_left),
    KX(uno_pc64_inject_key), KX(uno_pc64_inject_pointer),
    KX(pc64_shell_app_count), KX(pc64_shell_launch), KX(pc64_shell_close_top),
    KX(pc64_shell_win_count), KX(pc64_shell_win_title), KX(pc64_shell_win_focused),
    KX(uno_pc64_shutdown), KX(uno_dbg_uptime_ms),
    KX(unoauto_remote_active), KX(unoauto_remote_send),
    KX(unoauto_remote_recv),   KX(unoauto_remote_stop),
#endif
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
    if ((unsigned long long)sizeof *h + (unsigned long long)h->file_size +
        4ull * (unsigned long long)h->nreloc != (unsigned long long)n)
                                           { mdbg("modload: bad size\n");  return 0; }
    /* mem_size is attacker-controlled (the CRC authenticates nothing - a crafter
     * recomputes it). Cap it: without this, mem_size near 0xFFFFFFFF wrapped the
     * page-count math to ~0 (a tiny/zero alloc) and then memset(.., mem_size-..)
     * wrote ~4 GB out of bounds. 64 MB is far above any real module (Studio is
     * 256 KB). The np math below is also done in 64-bit as belt-and-suspenders. */
    if (h->entry >= h->mem_size || h->file_size > h->mem_size ||
        h->mem_size > (64u << 20))
                                           { mdbg("modload: bad hdr\n");   return 0; }
    if (mod_crc32(gModBuf + sizeof *h, n - (long)sizeof *h) != h->crc)
                                           { mdbg("modload: bad crc\n");   return 0; }

    np = (unsigned long)(((unsigned long long)h->mem_size + 4095ull) >> 12);
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
        /* 64-bit compare: rel[i]+8u in 32-bit wrapped (rel[i]=0xFFFFFFF8 passed
         * the check) and gave an 8-byte write at an attacker offset. */
        if ((unsigned long long)rel[i] + 8ull > h->mem_size) { if (!at) mod_free(base, np); return 0; }
        *(unsigned long long *)(base + rel[i]) +=
            (unsigned long long)base - h->pref_base;
    }

    /* resolve the named import slots: {char name[24]; u64 addr;} records */
    for (i = 0; i < h->imp_count; i++) {
        char *rec = (char *)base + h->imp_rva + 32u * i;
        void *fn;
        if ((unsigned long long)h->imp_rva + 32ull * (i + 1) > h->mem_size) { if (!at) mod_free(base, np); return 0; }
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
    if (e && !(flags & UNO_MODF_UUI)) { mdbg("modload: not a uui module\n"); e = 0; }
    { UnoAutoModEv ev; ev.file = file; ev.ok = e != 0;
      unoauto_hook_fire("mod.load", &ev); }
    return (UnoUuiEntry)e;
}

/* ---- the Python runtime + Python-app containers (PYRT.UNO) ----------------
 * PYRT.UNO is native code flagged UNO_MODF_PY whose entry returns a PyHost*.
 * A Python app is a UNO_MODF_PYAPP container carrying source bytes - not code,
 * so it is never instantiated; the shell hands the payload to PYRT. */
PyHostEntry uno_mod_load_pyrt(void)
{
    unsigned short flags = 0;
    void *e;
    long n = mod_read("PYRT.UNO", gModBuf, MODBUF_MAX);
    mdbg("modload(pyrt)\n");
    e = mod_instantiate(n, &flags, 0, 0, 0, 0);
    if (e && !(flags & UNO_MODF_PY)) { mdbg("modload: not a pyrt module\n"); return 0; }
    return (PyHostEntry)e;
}

/* the first 48 bytes of a module's header hold its flags; 0 if absent/bad */
unsigned short uno_mod_peek_flags(int vol, const char *path)
{
    unsigned char hdr[48];
    long n = uno_fs_read(vol, path, hdr, sizeof hdr);
    const UnoModHdr *h = (const UnoModHdr *)hdr;
    if (n < (long)sizeof hdr || h->magic != UNO_MOD_MAGIC) return 0;
    return h->flags;
}

/* read a PYAPP container from an explicit volume+path; return a pointer to its
 * source payload inside gModBuf (transient - PYRT compiles it immediately). */
int uno_mod_load_pyapp(int vol, const char *path, const unsigned char **src, int *len)
{
    const UnoModHdr *h = (const UnoModHdr *)gModBuf;
    long n = uno_fs_read(vol, path, gModBuf, MODBUF_MAX);
#ifdef UNO_DEBUG
    /* unoautomate door (debug only): a RAW .py source file - plain text can
     * never carry the UNO magic, so its whole content is the payload.  Lets
     * an operator drop AUTOMATE.PY on a stick with no container step. */
    if (n > 0 && n >= (long)sizeof *h && h->magic != UNO_MOD_MAGIC)
    { *src = gModBuf; *len = (int)n; return 0; }
#endif
    if (n < (long)sizeof *h)                   { mdbg("pyapp: not found\n"); return -1; }
    if (h->magic != UNO_MOD_MAGIC)             { mdbg("pyapp: bad magic\n"); return -1; }
    if (h->abi != UNO_ABI_VERSION)             { mdbg("pyapp: bad abi\n");   return -1; }
    if (!(h->flags & UNO_MODF_PYAPP))          { mdbg("pyapp: not a py app\n"); return -1; }
    /* S-MOD-12: 64-bit compare. `(long)sizeof*h + h->file_size` is 32-bit on
     * this LLP64 target, so a crafted file_size near UINT_MAX wrapped, passed
     * this check, then the CRC loop read h->file_size bytes OOB. */
    if ((unsigned long long)sizeof *h + (unsigned long long)h->file_size > (unsigned long long)n)
                                               { mdbg("pyapp: bad size\n");  return -1; }
    if (mod_crc32(gModBuf + sizeof *h, h->file_size) != h->crc)
                                               { mdbg("pyapp: bad crc\n");   return -1; }
    *src = gModBuf + sizeof *h;
    *len = (int)h->file_size;
    return 0;
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
    { UnoAutoModEv ev; ev.file = path; ev.ok = e != 0;
      unoauto_hook_fire("mod.load", &ev); }
    return (UnoAppEntry)e;
}

void uno_mod_unload_user(void)
{
    if (!gUserBase) return;
    if (!(gUserSlot && gUserBase == gUserSlot))   /* slot loads stay resident */
        mod_free(gUserBase, gUserNp);
    gUserBase = 0; gUserNp = 0;
    { UnoAutoModEv ev; ev.file = "(user)"; ev.ok = 1;
      unoauto_hook_fire("mod.unload", &ev); }
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
