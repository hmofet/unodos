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
#include "uno_appdesc.h"    /* what a .UNO says about itself (desc_rva)   */
#include "../unojs/unojs.h" /* the extension host engine UnoCode embeds   */
#include "pyhost.h"     /* Python-runtime + Python-app module tiers */
#include "unoauto.h"    /* mod.load / mod.unload tap points (no-op in prod) */
#include "unolog.h"
#include "unovirt_mgr.h"     /* appliances: the manager surface VMGR.UNO uses */
#include "pc64_fs.h"
#include "uno_binds.h"    /* uno.bind_* / uno.pref_* exports */
#include "fat.h"
#include "pc64_font.h"
#include "uefi.h"
#include "bootinfo.h"   /* BIOS boot: the E820 map is the only allocator */
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
#include "snd_pcm.h"        /* sampled effects (uno_snd_sfx_*) */
#include "snd_mus.h"        /* a game's own score, from memory */
#include "uno3d.h"          /* u3d_* 3D */
#include "unoscript.h"      /* the production scripting surface: usc_ + unosec_ */
#include "unoui_anim.h"     /* the shared tween clock the shell pumps */
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
void pc64_shell_fullscreen(unoui_window *w);   /* UnoShow's slide show mode */
int  pc64_shell_is_fullscreen(void);
const struct unoui_theme *pc64_shell_theme(void);
int  pc64_shell_run_user(int vol, const char *path);
const char *pc64_shell_py_error(void);
int  pc64_shell_font_mono(void);
/* the shell's native file dialog, if it has one; pc64's answers 0 */
int  pc64_shell_pick(int want_folder, int *vol, char *dir, int dcap,
                     char *name, int ncap);
unoui_anim *uno_pc64_anim(void);               /* the clock the shell pumps */
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
/* unodevices - the introspection surface PYRT's uno.devices()/uno.pci() call */
int devmgr_list_str(char *, int);
int devmgr_count(void);
int devmgr_info(int, unsigned int *, int);
const char *devmgr_driver_name(int);
/* the unoautomate DRIVE surface exported below (unoauto_* come from unoauto.h;
 * the rest are shell/kernel accessors with no public header).  Production since
 * 2026-08-03 - unoautomate ships, gated by privilege (unoauto_gate.h). */
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
    KX(tls_entropy_source), KX(tls_entropy_name),
    /* net bring-up + DNS + CA-validated TLS: a module (Studio's AI assistant)
     * makes its own HTTPS request through these */
    KX(pc64_net_up), KX(net_dns_query), KX(tls_connect_ca), KX(uno_pc64_delay_ms),
    /* tls.h's HANDLE API, which is the non-blocking one.  Everything above it
     * is the legacy module-scoped surface, and it BLOCKS - which is why the
     * one module using it (Studio's assistant) freezes the desk for the length
     * of a request.  UnoCode's uc_net.h seam is built on these instead, so the
     * editor can pump a request from its frame loop and keep drawing.
     * Exported together because a caller needs all of them: open, advance,
     * both directions, the error, and the way out. */
    KX(tls_open),  KX(tls_poll),  KX(tls_send),  KX(tls_recv),  KX(tls_free),
    KX(tls_conn_error), KX(tls_open_error),
    /* Intel WiFi + Realtek USB-ethernet status (Network app readout) */
    KX(iwl_present), KX(iwl_nic),    KX(iwl_mac),   KX(iwl_status_str),
    /* scan + join for the Network app's "pick an SSID, type the password" UI */
    KX(iwl_scan_aps), KX(iwl_join_ssid),
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
    /* scrolling lists: set_sel for widgets, the geometry for canvas apps */
    KX(unoui_list_set_sel), KX(unoui_list_draw),  KX(unoui_list_rows),
    KX(unoui_list_maxtop),  KX(unoui_list_index_at), KX(unoui_list_reveal),
    KX(unoui_list_bar),
    /* appended: the shared animation clock. uno_pc64_anim() hands back the
     * context the SHELL pumps, so a module animates against the same clock as
     * everything else instead of counting its own frames. */
    KX(uno_pc64_anim),
    KX(unoui_tween_start),  KX(unoui_tween_to),
    KX(unoui_anim_value),   KX(unoui_anim_progress), KX(unoui_anim_done),
    KX(unoui_anim_live),    KX(unoui_anim_active),
    KX(unoui_anim_cancel),  KX(unoui_anim_finish),   KX(unoui_anim_free),
    KX(unoui_ease),         KX(unoui_anim_lerp),     KX(unoui_anim_now),
    KX(unoui_seq_init),     KX(unoui_seq_add),       KX(unoui_seq_start),
    KX(unoui_seq_trigger),  KX(unoui_seq_stop),
    KX(unoui_seq_done),     KX(unoui_seq_waiting),
    /* framebuffer + fonts */
    KX(fb_fill_rect), KX(fb_hline), KX(fb_vline), KX(fb_blit), KX(fb_text),
    KX(fb_text_w),    KX(fb_text_h), KX(fb_width), KX(fb_height),
    KX(uno_font_draw_styled), KX(uno_font_text_w_styled), KX(uno_font_draw_mono),
    KX(uno_font_height_px),   KX(uno_font_baseline_px), KX(uno_font_active),
    KX(uno_font_push), KX(uno_font_pop),
    KX(uno_font_count), KX(uno_font_name),   /* UnoWord's Font combo */
    /* filesystem: the simple per-volume surface + the rich FAT one */
    KX(uno_fs_volumes), KX(uno_fs_volume_name), KX(uno_fs_list_begin),
    KX(uno_fs_list_get), KX(uno_fs_read), KX(uno_fs_read_at), KX(uno_fs_size),
    KX(uno_fs_write), KX(uno_fs_writable), KX(uno_fs_kind), KX(uno_fs_fat_index),
    KX(devmgr_list_str), KX(devmgr_count), KX(devmgr_info), KX(devmgr_driver_name),
    KX(uno_fs_mkdir),
    /* appended for UOWORD (the Office file dialog browses volumes) */
    KX(uno_fs_isdir),
    KX(uno_fat_list_ex), KX(uno_fat_read), KX(uno_fat_read_at), KX(uno_fat_size),
    KX(uno_fat_write), KX(uno_fat_delete), KX(uno_fat_mkdir), KX(uno_fat_rename),
    /* memory */
    KX(malloc), KX(free),
    /* shell services */
    KX(pc64_shell_add_window), KX(pc64_shell_del_window),
    KX(pc64_shell_focus_window), KX(pc64_shell_dirty),
    KX(pc64_shell_workarea_w), KX(pc64_shell_workarea_h),
    KX(pc64_shell_fullscreen), KX(pc64_shell_is_fullscreen),
    KX(pc64_shell_theme), KX(pc64_shell_run_user), KX(pc64_shell_font_mono),
    KX(pc64_shell_pick),
    KX(pc64_browser_open_path), KX(pc64_shell_py_error),
    /* ---- Python runtime (PYRT.UNO) surface ------------------------------- *
     * Appended at the tail so a concurrent kExports edit (Wi-Fi) merges
     * cleanly.  Exposes the full audio/3D/framebuffer/filesystem platform to
     * any module (not just Python), plus the libc/libm the interpreter links. */
    /* unosound */
    KX(uno_seq_init), KX(uno_seq_beep), KX(uno_seq_play), KX(uno_seq_stop),
    KX(uno_seq_playing), KX(uno_seq_tick), KX(uno_seq_backend),
    /* sampled audio: the WAD's own effects and score, for a game that has
       more to say than one square-wave note (snd_pcm.h, snd_mus.h) */
    KX(uno_snd_active),
    KX(uno_snd_sfx_load), KX(uno_snd_sfx_play), KX(uno_snd_sfx_stop_all),
    KX(uno_snd_sfx_playing),
    KX(uno_snd_mus_play), KX(uno_snd_mus_stop), KX(uno_snd_mus_playing),
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
    /* the side-effect-free live check.  mod_unoauto.c gates every call on it,
     * and it is the right primitive for any module asking "may I?" without
     * triggering a consent sheet - unlike unosec_request, which escalates. */
    KX(unosec_require),
    KX(usc_ui_pointer), KX(usc_ui_key), KX(usc_ui_screen_text),
    KX(usc_ui_clipboard_get), KX(usc_ui_clipboard_set),
    KX(usc_app_count), KX(usc_app_launch), KX(usc_app_close_top), KX(usc_app_message),
    KX(usc_fs_read), KX(usc_fs_write),
    KX(usc_proc_list), KX(usc_mem_read), KX(usc_mem_write),
    KX(usc_io_in), KX(usc_io_out), KX(usc_power),
    KX(usc_hook_add), KX(usc_hook_remove),
#ifdef UNO_DEBUG
    KX(unoscript_e2e_selftest),      /* authenticated end-to-end self-test (u.e2e) */
    KX(unoscript_mtest), /* manifest-caps self-test (u.mtest)          */
#endif
    /* ---- unoautomate DRIVE surface (every build) --------------------------
     * The `unoauto` Python module (upy_port/mod_unoauto.c) binds these.  It used
     * to compile to stubs in production so the prod export table stayed clean;
     * since 2026-08-03 unoautomate ships in production and the module is real in
     * both builds, capability-gated per call (automate.observe/drive/system).
     * One export table, one code path, both builds test it. */
    KX(unoauto_log), KX(unoauto_probe), KX(unoauto_deadline_left),
    KX(uno_pc64_inject_key), KX(uno_pc64_inject_pointer),
    KX(pc64_shell_app_count), KX(pc64_shell_launch), KX(pc64_shell_close_top),
    KX(pc64_shell_win_count), KX(pc64_shell_win_title), KX(pc64_shell_win_focused),
    KX(uno_pc64_shutdown), KX(uno_dbg_uptime_ms),
    KX(unoauto_remote_active), KX(unoauto_remote_send),
    KX(unoauto_remote_recv),   KX(unoauto_remote_stop),
    /* unolog (pc64/UNOLOG.md): the system log, for the uno.log* Python
     * bindings and for LOGVIEW.UNO. The read side is exported as well as the
     * write side - a viewer that could only append would be a strange thing. */
    /* unovirt: what the appliance manager (APPS\VMGR.UNO) needs. The
     * hypervisor itself is not exported and must not be - a module gets
     * the manager's surface, not a vCPU. */
    KX(uno_vm_count), KX(uno_vm_get), KX(uno_vm_add), KX(uno_vm_set),
    KX(uno_vm_del), KX(uno_vm_save), KX(uno_vm_start), KX(uno_vm_stop),
    KX(uno_vm_running), KX(uno_vm_status),
    KX(uno_vm_con_lines), KX(uno_vm_con_line), KX(uno_vm_con_seq),
    KX(uno_vm_con_key), KX(uno_vm_con_clear),
    KX(unolog), KX(unolog_flush), KX(unolog_format),
    KX(unolog_level), KX(unolog_set_level),
    KX(unolog_remote_level), KX(unolog_set_remote_level),
    KX(unolog_set_remote), KX(unolog_remote_host), KX(unolog_remote_port),
    KX(unolog_set_listen), KX(unolog_listening), KX(unolog_save_cfg),
    KX(unolog_first), KX(unolog_next), KX(unolog_get),
    KX(unolog_dropped), KX(unolog_sent), KX(unolog_received),
    KX(unolog_sev_name), KX(unolog_fac_name),
    /* held-keys level for PYRT's uno.keys_down() (Duum movement) */
    KX(uno_pc64_keys_held),
    /* key bindings + app preferences, for PYRT's uno.bind_* / uno.pref_*
     * (Duum's Controls screen and its FPS toggle) */
    KX(uno_bind_name), KX(uno_bind_set), KX(uno_bind_reset),
    KX(uno_bind_keyid), KX(uno_pref_get), KX(uno_pref_set),
    /* ---- UnoCode (APPS\UNOCODE.UNO) surface -----------------------------
     * Appended at the tail, per AGENTS.md section 2: this table is a shared
     * choke-point and every entry here is an ADDITION - nothing above moves.
     *
     * Two filesystem calls the editor needs and no earlier module did: the
     * subdirectory listing (the Explorer tree and the EXT\ scan) and the
     * state-volume pick (where UNOCODE\SETTINGS.JSN belongs - the answer
     * pc64_fs.c already owns, rather than a fourth private copy of the
     * heuristic).  Then unojs, the extension host's interpreter. */
    KX(uno_fs_list_dir), KX(uno_fs_pref_vol),
    /* unojs: the EMBEDDING surface only (unojs/unojs.h).  No parser, lexer or
     * VM internals - a module gets the same API pc64/webjs.c uses, which is
     * what keeps unojs free to change underneath both of them. */
    KX(ujs_new), KX(ujs_free), KX(ujs_eval), KX(ujs_resume),
    KX(ujs_exception), KX(ujs_clear_exception), KX(ujs_describe),
    KX(ujs_scope_open), KX(ujs_scope_close), KX(ujs_root), KX(ujs_unroot),
    KX(ujs_undefined), KX(ujs_null), KX(ujs_bool), KX(ujs_number),
    KX(ujs_string), KX(ujs_object_new), KX(ujs_array_new),
    KX(ujs_typeof), KX(ujs_is_undefined), KX(ujs_is_null), KX(ujs_is_number),
    KX(ujs_is_string), KX(ujs_is_object), KX(ujs_is_array), KX(ujs_is_function),
    KX(ujs_to_number), KX(ujs_to_bool), KX(ujs_string_bytes), KX(ujs_to_string),
    KX(ujs_get), KX(ujs_set), KX(ujs_get_index), KX(ujs_set_index),
    KX(ujs_has), KX(ujs_delete), KX(ujs_array_length), KX(ujs_array_push),
    KX(ujs_call), KX(ujs_function_new), KX(ujs_host_new), KX(ujs_host_user),
    KX(ujs_set_fn), KX(ujs_set_accessor), KX(ujs_global),
    KX(ujs_throw), KX(ujs_throw_error),
    KX(ujs_fuel_used), KX(ujs_fuel_reset), KX(ujs_gc), KX(ujs_heap_used),
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
    unsigned int       crc;
    /* was `rsv`, always written 0 and never read.  Now the RVA of the app
     * descriptor block inside the image (uno_appdesc.h); 0 = no descriptor,
     * which is what every module built before 2026-08-07 says. */
    unsigned int       desc_rva;
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
    unsigned long long want =
        ((unsigned long long)(MOD_ARENA_PAGES + USER_SLOT_PAGES)) << 12;
    if (gModArena) return;

    /* BIOS boot: no AllocatePages, so the arena comes out of the E820 map.
     *
     * THIS IS NOT COSMETIC. Without an arena mod_alloc() returns 0 for every
     * module, and every loadable app - Files, Notepad, Photos, Dostris, the
     * Browser, Studio - fails to launch. The desktop draws either way, which is
     * exactly what makes it easy to call a BIOS boot "working" while half the
     * system is unreachable.
     *
     * uno_bios_find_ram() takes the TOP of the highest usable run below 4 GiB
     * (all the loader's page tables map), which keeps the arena clear of the
     * kernel and its heap at the bottom of high memory. Nothing else allocates
     * physical pages on this path, so a single carve-out needs no bookkeeping
     * beyond the bump pointer that is already here. */
    if (!ST) {
        const uno_bootinfo *bi = uno_pc64_bootinfo();
        unsigned long long base = uno_bios_find_ram(bi, want);
        if (!base) return;                 /* mod_alloc keeps failing, visibly */
        gModArena = (unsigned char *)(unsigned long long)base;
        gUserSlot = gModArena + ((unsigned long)MOD_ARENA_PAGES << 12);
        return;
    }

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
    unsigned char *base = 0; unsigned long np = 0;
    void *e;
    long n = mod_read(file, gModBuf, MODBUF_MAX);
    mdbg("modload: "); mdbg(file); mdbg("\n");
    e = mod_instantiate(n, &flags, 0, 0, &base, &np);
    if (e && (flags & UNO_MODF_UUI)) {
        /* a unoui-class module in the classic roster would be called with
         * the wrong entry signature - refuse it here, and free its pages: a
         * wrong-tier .UNO retried per launch would otherwise exhaust the 4 MB
         * post-detach arena. */
        mdbg("modload: uui module in classic slot\n");
        mod_free(base, np);
        return 0;
    }
    return (UnoAppEntry)e;
}

/* ---- loadable DRIVERS (unodevices phase 4) --------------------------------
 * A driver lives at \DRIVERS\<NAME>.UNO on a specific volume - specific,
 * because devmgr scans volumes itself and a driver found on volume 2 must be
 * loaded from volume 2, not from whichever volume happens to answer first.
 * That is the one difference from mod_read()'s search-every-volume behaviour,
 * which is right for the fixed app roster and wrong here.
 *
 * `vol` is a fat.c volume index, NOT a uno_fs one, and the distinction is not
 * cosmetic: uno_fs_* is a MAP over the RAM disk, the FAT volumes and the
 * firmware filesystem, so its volume 0 is the RAM disk while fat.c's volume 0
 * is the first real FAT partition. Listing with one namespace and reading with
 * the other reads the wrong disk, silently - which is exactly what the first
 * version of this did, and what the phase-4 gate caught.
 *
 * UNO_MODF_DRV (0x0008) is checked because the entry SIGNATURE differs: a
 * driver entry takes a services pointer and returns a module descriptor, so
 * calling an ordinary app through it would pass a pointer where the app
 * expects nothing and interpret its return value as a struct address. The flag
 * is the only thing standing between "someone copied an app into \DRIVERS\"
 * and a jump through a bad pointer. */
void *uno_mod_load_drv(int vol, const char *file)
{
    unsigned short flags = 0;
    unsigned char *base = 0; unsigned long np = 0;
    void *e;
    char p[80];
    long n;
    if (!file || !*file) return 0;
    strcpy(p, "DRIVERS\\"); strcat(p, file);
    n = uno_fat_read(vol, p, gModBuf, MODBUF_MAX);
    if (n <= 0) return 0;
    mdbg("modload(drv): "); mdbg(file); mdbg("\n");
    e = mod_instantiate(n, &flags, 0, 0, &base, &np);
    if (e && !(flags & 0x0008)) { mdbg("modload: not a driver module\n"); mod_free(base, np); e = 0; }
    return e;
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

/* Where APPS\<file> actually is.  uno_mod_present answers yes/no; this answers
 * "which volume and under which of the two layouts", which is what the registry
 * needs so a later read does not search all over again. */
int uno_mod_find(const char *file, int *vol_out, char *path_out, int max)
{
    int nv = uno_fs_volumes(), v, pass;
    char p[64];
    for (pass = 0; pass < 2; pass++) {
        strcpy(p, pass ? "EFI\\UNODOS\\APPS\\" : "APPS\\");
        strcat(p, file);
        for (v = pass; v < nv; v++)          /* the ESP layout skips volume 0 */
            if (uno_fs_size(v, p) >= 48) {
                if (vol_out) *vol_out = v;
                if (path_out && max > 0) {
                    int i = 0;
                    while (p[i] && i < max - 1) { path_out[i] = p[i]; i++; }
                    path_out[i] = 0;
                }
                return 1;
            }
    }
    return 0;
}

/* ---- the app descriptor: metadata WITHOUT loading the module ---------------
 * Two reads, no arena, no relocation, nothing executed.  See uno_appdesc.h for
 * why that constraint drives the whole format. */

static void d_setstr(char *dst, int cap, const char *src, int n)
{
    int i = 0;
    while (i < n && i < cap - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* lowercase and keep [a-z0-9._-] only: how an id is derived from a filename
 * stem, and how a hand-written `id:` is sanitised.  `n` bounds the source. */
static void d_ident(char *dst, int cap, const char *src, int n)
{
    int i = 0, j = 0;
    while (j < n && src[j] && i < cap - 1) {
        char c = src[j++];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') dst[i++] = c;
    }
    dst[i] = 0;
}

/* "APPS\\VMGR.UNO" -> "VMGR" */
static void d_stem(char *dst, int cap, const char *path)
{
    const char *b = path, *p = path, *dot = 0;
    int i = 0;
    for (; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    for (p = b; *p; p++) if (*p == '.') dot = p;
    for (p = b; *p && (!dot || p < dot) && i < cap - 1; p++) dst[i++] = *p;
    dst[i] = 0;
}

static const char *kCatName[UAC_NCAT] =
    { "system", "net", "tools", "media", "games", "other" };

/* one "key: value" line; `k`/`v` point into the body, lengths are exact */
static void desc_apply(UnoAppDesc *d, const char *k, int kn,
                       const char *v, int vn)
{
    int i;
    if (kn == 2 && !memcmp(k, "id", 2))          d_ident(d->id, sizeof d->id, v, vn);
    else if (kn == 4 && !memcmp(k, "name", 4))   d_setstr(d->name, sizeof d->name, v, vn);
    else if (kn == 5 && !memcmp(k, "short", 5))  d_setstr(d->shortnm, sizeof d->shortnm, v, vn);
    else if (kn == 4 && !memcmp(k, "icon", 4))   d_setstr(d->icon, sizeof d->icon, v, vn);
    else if (kn == 3 && !memcmp(k, "cat", 3)) {
        for (i = 0; i < UAC_NCAT; i++) {
            int n = 0; while (kCatName[i][n]) n++;
            if (n == vn && !memcmp(v, kCatName[i], (unsigned)n)) { d->cat = (unsigned char)i; break; }
        }
    } else if (kn == 4 && !memcmp(k, "rank", 4)) {
        int r = 0;
        for (i = 0; i < vn && v[i] >= '0' && v[i] <= '9'; i++) r = r * 10 + (v[i] - '0');
        if (r > 255) r = 255;
        d->rank = (unsigned char)r;
    } else if (kn == 5 && !memcmp(k, "flags", 5)) {
        int s = 0;
        for (i = 0; i <= vn; i++) {
            if (i == vn || v[i] == ',') {
                int n = i - s; const char *f = v + s;
                if (n == 9 && !memcmp(f, "singleton", 9)) d->flags |= UAF_SINGLETON;
                else if (n == 6 && !memcmp(f, "hidden", 6))    d->flags |= UAF_HIDDEN;
                else if (n == 4 && !memcmp(f, "game", 4))      d->flags |= UAF_GAME;
                else if (n == 9 && !memcmp(f, "nosession", 9)) d->flags |= UAF_NOSESSION;
                s = i + 1;
                while (s < vn && v[s] == ' ') s++;
            }
        }
    } else if (kn == 3 && !memcmp(k, "min", 3)) {
        int w = 0, h = 0;
        for (i = 0; i < vn && v[i] >= '0' && v[i] <= '9'; i++) w = w * 10 + (v[i] - '0');
        if (i < vn && (v[i] == 'x' || v[i] == 'X')) {
            for (i++; i < vn && v[i] >= '0' && v[i] <= '9'; i++) h = h * 10 + (v[i] - '0');
            if (w > 0 && w < 8192 && h > 0 && h < 8192)
                { d->pref_w = (short)w; d->pref_h = (short)h; }
        }
    }
    /* every other key, including `needs:`, is deliberately ignored here: the
     * enforced capability grant is the signed .MFT, and an unknown key must
     * never fail a parse or the format stops being extensible. */
}

static void desc_parse(UnoAppDesc *d, const char *body, int n)
{
    int i = 0;
    while (i < n) {
        int ks = i, ke, vs, ve;
        while (i < n && body[i] != '\n') i++;
        ve = i; if (ve > ks && body[ve - 1] == '\r') ve--;
        for (ke = ks; ke < ve && body[ke] != ':'; ke++) { }
        if (ke < ve) {
            vs = ke + 1;
            while (vs < ve && (body[vs] == ' ' || body[vs] == '\t')) vs++;
            while (ve > vs && (body[ve - 1] == ' ' || body[ve - 1] == '\t')) ve--;
            if (ke > ks) desc_apply(d, body + ks, ke - ks, body + vs, ve - vs);
        }
        i++;                                     /* step over the newline */
    }
}

/* 0 = descriptor read, 1 = a module with no descriptor (defaults filled in),
 * -1 = not a module at all.  Never allocates and never runs module code. */
int uno_mod_desc_read(int vol, const char *path, UnoAppDesc *out)
{
    unsigned char hdr[48], blk[UNO_APPDESC_MAX];
    const UnoModHdr *h = (const UnoModHdr *)hdr;
    const UnoAppDescHdr *dh = (const UnoAppDescHdr *)blk;
    char stem[16];
    long n;
    int len;

    if (!out) return -1;
    n = uno_fs_read(vol, path, hdr, (long)sizeof hdr);
    if (n < (long)sizeof hdr || h->magic != UNO_MOD_MAGIC ||
        h->abi != UNO_ABI_VERSION) return -1;

    /* defaults first, so every later failure degrades instead of refusing */
    memset(out, 0, sizeof *out);
    out->cat = UAC_OTHER;
    out->rank = 100;
    out->tier = h->flags;
    d_stem(stem, sizeof stem, path);
    d_ident(out->id, sizeof out->id, stem, (int)sizeof stem);
    /* FAT gives us "VMGR"; title-case it so a module with no descriptor still
     * reads as a name rather than as shouting. */
    d_setstr(out->name, sizeof out->name, stem, (int)sizeof stem);
    { int i; for (i = 1; out->name[i]; i++)
        if (out->name[i] >= 'A' && out->name[i] <= 'Z')
            out->name[i] = (char)(out->name[i] - 'A' + 'a'); }
    d_setstr(out->shortnm, sizeof out->shortnm, out->name, (int)sizeof out->name);

    /* A PYAPP's block sits AFTER the source rather than inside the image, so
     * desc_rva == file_size there and the in-image bound would reject it. The
     * payload is Python that PYRT compiles verbatim; nothing may be spliced
     * into it, and file_size has to keep covering only the source or the crc
     * would not match. The block validates itself below (magic, version,
     * length), so letting it live past the source costs no checking. */
    if (!h->desc_rva) return 1;
    if (h->desc_rva >= h->file_size && !(h->flags & UNO_MODF_PYAPP)) return 1;
    n = uno_fs_read_at(vol, path, (long)(sizeof hdr + h->desc_rva),
                       blk, (long)sizeof blk);
    if (n < (long)sizeof *dh) return 1;
    if (dh->magic != UNO_APPDESC_MAGIC || dh->ver != UNO_APPDESC_VER) return 1;
    len = dh->len;
    if (len <= (int)sizeof *dh || len > (int)sizeof blk || len > (int)n) return 1;

    out->shortnm[0] = 0;                 /* so `short:` can be seen to be set */
    desc_parse(out, (const char *)blk + sizeof *dh, len - (int)sizeof *dh);
    if (!out->id[0])                     /* an `id:` of pure punctuation      */
        d_ident(out->id, sizeof out->id, stem, (int)sizeof stem);
    if (!out->shortnm[0])                /* `short:` defaults to `name:`      */
        d_setstr(out->shortnm, sizeof out->shortnm, out->name,
                 (int)sizeof out->name);
    out->has_desc = 1;
    return 0;
}

/* ---- the scan: every .UNO on the system, described, none of them loaded -----
 * Walks APPS\ on every volume and then EFI\UNODOS\APPS\ on volumes 1.., the
 * same two layouts uno_mod_find already searches, and de-duplicates by FILENAME
 * so an installed copy and a stick copy count once (the first found wins, which
 * matches which one uno_mod_find/mod_read would go on to load).
 *
 * `*over` (nullable) is set when a directory held more entries than fitted:
 * the caller reports that rather than quietly showing a short list, because a
 * truncated app list looks exactly like an app that failed to install.
 *
 * Costs two sector reads per module. Nothing here allocates and nothing here
 * runs module code - see uno_appdesc.h for why that is the whole point. */
int uno_mod_scan(UnoAppDesc *out, char (*file)[16], signed char *vol,
                 int maxn, int *over)
{
    int nv = uno_fs_volumes(), v, pass, n = 0, i, j;
    char names[48][16];
    if (over) *over = 0;
    if (!out || !file || !vol || maxn <= 0) return 0;
    for (pass = 0; pass < 2; pass++) {
        const char *dir = pass ? "EFI\\UNODOS\\APPS" : "APPS";
        for (v = pass; v < nv; v++) {
            int total = uno_fs_list_dir(v, dir, names, 48);
            if (total > 48) { if (over) *over = 1; total = 48; }
            for (i = 0; i < total; i++) {
                char path[64];
                UnoAppDesc d;
                int k = 0; const char *p;
                /* .UNO only - APPS\ also holds .MFT manifests and .PY sources */
                for (j = 0; names[i][j]; j++) { }
                if (j < 5 || names[i][j-4] != '.' || names[i][j-3] != 'U' ||
                    names[i][j-2] != 'N' || names[i][j-1] != 'O') continue;
                for (j = 0; j < n; j++) if (!strcmp(file[j], names[i])) break;
                if (j < n) continue;                    /* already seen this file */
                if (n >= maxn) { if (over) *over = 1; return n; }
                for (p = dir; *p && k < 63; ) path[k++] = *p++;
                if (k < 63) path[k++] = '\\';
                for (p = names[i]; *p && k < 63; ) path[k++] = *p++;
                path[k] = 0;
                if (uno_mod_desc_read(v, path, &d) < 0) continue;  /* not a module */
                out[n] = d;
                strcpy(file[n], names[i]);
                vol[n] = (signed char)v;
                n++;
            }
        }
    }
    return n;
}

UnoUuiEntry uno_mod_load_uui(const char *file)
{
    unsigned short flags = 0;
    unsigned char *base = 0; unsigned long np = 0;
    void *e;
    long n = mod_read(file, gModBuf, MODBUF_MAX);
    mdbg("modload(uui): "); mdbg(file); mdbg("\n");
    e = mod_instantiate(n, &flags, 0, 0, &base, &np);
    if (e && !(flags & UNO_MODF_UUI)) { mdbg("modload: not a uui module\n"); mod_free(base, np); e = 0; }
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
    unsigned char *base = 0; unsigned long np = 0;
    void *e;
    long n = mod_read("PYRT.UNO", gModBuf, MODBUF_MAX);
    mdbg("modload(pyrt)\n");
    e = mod_instantiate(n, &flags, 0, 0, &base, &np);
    if (e && !(flags & UNO_MODF_PY)) { mdbg("modload: not a pyrt module\n"); mod_free(base, np); return 0; }
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
