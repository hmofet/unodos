/* cosmo64/display.c -- the pc64 shell's display platform layer on the Cosmo.
 *
 * The shell draws into pc64's software framebuffer fb[] (fb.h, 0xAABBGGRR,
 * FB_W x FB_H = 640x480 here) and calls uno_pc64_present() each frame; this
 * rotates it 270 degrees, scales it 2x, swizzles R<->B (the panel wants
 * 0xAARRGGBB) and writes it centred into LK's adopted framebuffer -- the same
 * present the m0 payload proved on hardware, plus the channel swap the x86
 * build did with gSwapRB. Boot-time adoption itself lives in videolfb.c.
 */

#include "cosmo64.h"
#include "fb.h"
#include "mac_compat.h"

int uno_fb_w = 640, uno_fb_h = 480;

void uno_pc64_present(void)
{
    /* publish fb[] as the source surface for the harness's eye check (the
     * harness detects the R<->B swizzle by trying both channel orders) */
    FBDBG->fb_shadow = (c64_u64)fb;
    FBDBG->fb_base = (c64_u64)fb;
    FBDBG->fb_pitch = C64_SCRW * 4;
    c64_u8 *drow = (c64_u8 *)FBDBG->fb_dorigin;
    c64_u32 ppitch = FBDBG->fb_ppitch;
    const fb_px *scol = fb + (C64_SCRW - 1);      /* rot 270 source start */
    for (int sr = 0; sr < C64_DST_H / FB_SCALE; sr++) {
        for (int rep = 0; rep < FB_SCALE; rep++) {
            c64_u32 *dst = (c64_u32 *)drow;
            const fb_px *s = scol;
            for (int sc = 0; sc < C64_DST_W / FB_SCALE; sc++) {
                fb_px px = *s;
                c64_u32 out = 0xFF000000u | ((px & 0xFFu) << 16)
                            | (px & 0xFF00u) | ((px >> 16) & 0xFFu);
                for (int k = 0; k < FB_SCALE; k++)
                    *dst++ = out;
                s += C64_SCRW;                    /* one source row down */
            }
            drow += ppitch;
        }
        scol -= 1;                                /* one source column left */
    }
    __asm__ volatile("dsb sy" ::: "memory");
    c64_bcn(BCN_MAIN);                            /* the shell is presenting */
}

void uno_pc64_present_cursor(void)
{
    uno_pc64_present();
}

static fb_px g_scene[C64_SCRW * C64_SCRH];

void uno_pc64_scene_save(void)
{
    for (int i = 0; i < C64_SCRW * C64_SCRH; i++)
        g_scene[i] = fb[i];
}

void uno_pc64_scene_restore(void)
{
    for (int i = 0; i < C64_SCRW * C64_SCRH; i++)
        fb[i] = g_scene[i];
}

void uno_pc64_lowres(int on)
{
    (void)on;                                     /* one mode on this panel */
}

int uno_pc64_res_count(void)
{
    return 1;
}

void uno_pc64_res_get(int idx, short *w, short *h, short *zoom, Boolean *active)
{
    (void)idx;
    *w = C64_SCRW;
    *h = C64_SCRH;
    *zoom = FB_SCALE;
    *active = 1;
}

void uno_pc64_res_set(int idx)
{
    (void)idx;
}
