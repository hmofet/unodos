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

/* ---- the pointer -------------------------------------------------------- */
/* The shell never draws its own cursor: on x86 uefi_main.c composites it into
 * each presented row (cursor_row()), and this port replaces uefi_main.c, so
 * until now nothing drew one at all. Touch worked and the shell tracked it --
 * there was simply no arrow on the panel, which is exactly what the hardware
 * test reported.
 *
 * Drawn AFTER the blit rather than inside it: this present repaints the whole
 * UI rect every frame, so the previous position is already erased and a
 * post-pass leaves no trail, at a cost of ~15x9 source pixels instead of a
 * branch in the hot loop.
 *
 * Same sprite as the x86 build, so the pointer looks identical on both.
 * 'B' = black, 'W' = white, ' ' = transparent. */
static const char *const kCursor[] = {
    "B", "BB", "BWB", "BWWB", "BWWWB", "BWWWWB", "BWWWWWB", "BWWWWWWB",
    "BWWWWBBBB", "BWWBWB", "BWB BWB", "BB  BWB", "B    BWB", "      BWB",
    "       BB", 0
};

/* Gated on a pointer having actually reported, exactly like x86's
 * g_have_pointer -- and load-bearing for the gate: QEMU has no touch panel, so
 * no cursor is drawn there and qharness.py's pixel-exact eye check still
 * compares the panel against fb[] with nothing extra composited on top. */
int c64_input_have_pointer(void);

static void draw_cursor(void)
{
    if (!c64_input_have_pointer())
        return;
    int cx, cy, btn;
    uno_pc64_mouse(&cx, &cy, &btn);
    c64_u8 *origin = (c64_u8 *)FBDBG->fb_dorigin;
    c64_u32 ppitch = FBDBG->fb_ppitch;

    for (int r = 0; r < 15 && kCursor[r]; r++) {
        int y = cy + r;                            /* source row  */
        if (y < 0 || y >= C64_SCRH)
            continue;
        const char *row = kCursor[r];
        for (int c = 0; row[c]; c++) {
            int x = cx + c;                        /* source column */
            if (x < 0 || x >= C64_SCRW || row[c] == ' ')
                continue;
            /* the same rotation the blit above uses: source column x lands on
             * panel row (C64_SCRW-1-x), source row y on panel column y, each
             * an FB_SCALE block */
            c64_u32 out = (row[c] == 'B') ? 0xFF000000u : 0xFFFFFFFFu;
            int sr = (C64_SCRW - 1) - x;
            c64_u8 *p = origin + (c64_u64)sr * FB_SCALE * ppitch
                      + (c64_u64)y * FB_SCALE * 4;
            for (int rep = 0; rep < FB_SCALE; rep++) {
                c64_u32 *d = (c64_u32 *)p;
                for (int k = 0; k < FB_SCALE; k++)
                    d[k] = out;
                p += ppitch;
            }
        }
    }
}

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
    draw_cursor();
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
