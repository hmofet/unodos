/* cosmo64/input.c -- the input side of the pc64 platform seam.
 *
 * Same shape as uefi_main.c's ring: producers call c64_key_push() (the AW9523
 * matrix driver will, in M2; unoauto injection does today), the shell drains
 * it with uno_pc64_next_key2(). Until M2 there is no producer, so the shell
 * runs mouse-less and key-less but alive -- unoui is built for exactly that
 * (UI_EV_TICK still animates). */

#include "cosmo64.h"
#include "mac_compat.h"

#define RAWK 32
static struct { int scan, uni, mods; } g_ring[RAWK];
static int g_rd, g_wr;
/* Two keyboards and two pointers can be live at once -- the AW9523 matrix
 * and a USB keyboard, the touch panel and a USB mouse -- so each source keeps
 * its own level state and the shell sees the OR, which is what x86's poll
 * loop does across its devices ("a click on ANY device's ANY button"). */
static int g_mods, g_held;              /* the matrix keyboard   */
static int g_mods_usb, g_held_usb;      /* a USB keyboard        */
static int g_cx = C64_SCRW / 2, g_cy = C64_SCRH / 2, g_wheel;
static int g_btn;                       /* the touch panel (absolute) */
static int g_btn_usb;                   /* a USB mouse (relative)     */
static int g_lock;
static int g_speed = 100;

void c64_key_push(int scan, int uni, int mods)
{
    int nx = (g_wr + 1) % RAWK;
    if (nx == g_rd)
        return;                                   /* full: drop, like x86 */
    g_ring[g_wr].scan = scan;
    g_ring[g_wr].uni = uni;
    g_ring[g_wr].mods = mods;
    g_wr = nx;
}

int uno_pc64_next_key2(int *scan, int *uni, int *mods)
{
    if (g_rd == g_wr)
        return 0;
    *scan = g_ring[g_rd].scan;
    *uni = g_ring[g_rd].uni;
    *mods = g_ring[g_rd].mods;
    g_rd = (g_rd + 1) % RAWK;
    return 1;
}

int uno_pc64_next_key(int *scan, int *uni, int *ctrl)
{
    int mods;
    if (!uno_pc64_next_key2(scan, uni, &mods))
        return 0;
    *ctrl = (mods & 2) ? 1 : 0;                   /* UI_MOD_CTRL */
    return 1;
}

/* Set once a real pointer has reported, so display.c knows whether to draw a
 * cursor at all -- the same idea as x86's g_have_pointer. On a machine with no
 * pointer (QEMU, where the touch panel is absent) it stays 0 and nothing is
 * composited, which keeps the harness's pixel-exact eye check honest. */
static int g_have_ptr;

int c64_input_have_pointer(void)
{
    return g_have_ptr;
}

void c64_input_set_pointer(int x, int y, int btn)
{
    g_cx = x;
    g_cy = y;
    g_btn = btn;
    g_have_ptr = 1;
}

/* A USB mouse reports deltas, and a boot mouse reports only on change, so the
 * button mask arriving here is the LATCHED level usbhid.c holds for it --
 * store it as such. Deltas are applied raw, the way x86 applies a USB HID
 * mouse's (the pointer-speed preference scales the firmware pointer's
 * normalised motion, not a mouse's counts). */
void c64_input_move_pointer(int dx, int dy, int btn)
{
    if (g_lock)
        return;
    g_cx += dx;
    g_cy += dy;
    if (g_cx < 0) g_cx = 0;
    if (g_cy < 0) g_cy = 0;
    if (g_cx > c64_scrw - 1) g_cx = c64_scrw - 1;
    if (g_cy > c64_scrh - 1) g_cy = c64_scrh - 1;
    g_btn_usb = btn;
    g_have_ptr = 1;
}

/* The desktop size changed under the pointer: keep it under the same spot on
 * the glass rather than clamping it, which throws it at an edge when the
 * desktop shrinks. display.c's apply_desktop calls this. */
void c64_input_rescale_pointer(int ow, int oh, int nw, int nh)
{
    if (ow > 0 && oh > 0) {
        g_cx = (int)(((long long)g_cx * nw) / ow);
        g_cy = (int)(((long long)g_cy * nh) / oh);
    }
    if (g_cx < 0) g_cx = 0;
    if (g_cy < 0) g_cy = 0;
    if (g_cx > nw - 1) g_cx = nw - 1;
    if (g_cy > nh - 1) g_cy = nh - 1;
}

void c64_input_add_wheel(int notches)
{
    g_wheel += notches;
}

void c64_input_set_level(int mods, int held)
{
    g_mods = mods;
    g_held = held;
}

void c64_input_set_level_usb(int mods, int held)
{
    g_mods_usb = mods;
    g_held_usb = held;
}

int uno_pc64_mods(void)
{
    return g_mods | g_mods_usb;
}

int uno_pc64_keys_held(void)
{
    return g_held | g_held_usb;
}

void uno_pc64_mouse(int *x, int *y, int *btn)
{
    *x = g_cx;
    *y = g_cy;
    *btn = g_btn | g_btn_usb;
}

int uno_pc64_wheel(void)
{
    int w = g_wheel;
    g_wheel = 0;
    return w;
}

int uno_pc64_mac_mouse(short *h, short *v)
{
    *h = (short)g_cx;
    *v = (short)g_cy;
    return g_btn | g_btn_usb;
}

void uno_pc64_pointer_speed(int pct)
{
    g_speed = pct;
}

int uno_pc64_pointer_speed_get(void)
{
    return g_speed;
}

void uno_pc64_input_lock(int on)
{
    g_lock = on;
}

int uno_pc64_input_locked(void)
{
    return g_lock;
}

void uno_pc64_inject_key(int scan, int uni, int ctrl)
{
    if (!g_lock)
        c64_key_push(scan, uni, ctrl ? 2 : 0);
}

void uno_pc64_inject_pointer(int x, int y, int btn)
{
    if (g_lock)
        return;
    g_cx = x;
    g_cy = y;
    g_btn = btn;
    g_have_ptr = 1;
}

void uno_pc64_ptr_status(int *nsimple, int *nabs, int *blocked)
{
    /* The touch panel is an ABSOLUTE pointer, so report it as one rather than
     * leaving the Control Panel claiming this machine has no pointer while the
     * cursor is visibly tracking a finger. */
    *nsimple = c64_usb_mice();          /* USB mice are relative pointers */
    *nabs = c64_touch_present() ? 1 : 0;
    *blocked = 0;
}
