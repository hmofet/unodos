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
static int g_mods, g_held;
static int g_cx = 320, g_cy = 240, g_btn, g_wheel;
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

void c64_input_set_pointer(int x, int y, int btn)
{
    g_cx = x;
    g_cy = y;
    g_btn = btn;
}

void c64_input_set_level(int mods, int held)
{
    g_mods = mods;
    g_held = held;
}

int uno_pc64_mods(void)
{
    return g_mods;
}

int uno_pc64_keys_held(void)
{
    return g_held;
}

void uno_pc64_mouse(int *x, int *y, int *btn)
{
    *x = g_cx;
    *y = g_cy;
    *btn = g_btn;
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
    return g_btn;
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
}

void uno_pc64_ptr_status(int *nsimple, int *nabs, int *blocked)
{
    *nsimple = 0;
    *nabs = 0;
    *blocked = 0;
}
