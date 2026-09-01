/* cosmo64/m0.c -- the standalone M0/M1 test payload: the C toolchain, MMU,
 * FPU and the display path, end to end, with no pc64 code linked.
 *
 * It draws a test card into its own .bss shadow and presents it rotated and
 * scaled -- the same contract the shell's platform layer implements over
 * pc64's fb[] (display.c). Adoption, the FDT walk and the FBINFO debug
 * contract live in videolfb.c, shared with the shell build; qharness.py (and
 * originally the asm port's Unicorn harness, up to M0) gates the result.
 * History and the load-bearing flags: see README.md and the git log.
 */

#include "cosmo64.h"

#define SCRW C64_SCRW
#define SCRH C64_SCRH
#define FRAME_TICKS 216667             /* ~60 Hz at the MT6771's 13 MHz */

/* freestanding runtime for THIS payload only (the shell build gets these from
 * pc64_libc.c, so m0.c must not link there) */
void *memset(void *dst, int c, unsigned long long n)
{
    c64_u8 *d = dst;
    while (n--)
        *d++ = (c64_u8)c;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned long long n)
{
    c64_u8 *d = dst;
    const c64_u8 *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

/* The shadow lives in OUR OWN .bss, not in LK's vram: vram page 1 is scanned
 * out as a garbage band by a leftover display layer (hardware-confirmed
 * 2026-08-31 -- the band vanished the moment the shadow moved here). */
static c64_u32 shadow_buf[SCRW * SCRH] __attribute__((aligned(64)));

static void frect(int x, int y, int w, int h, c64_u32 c)
{
    for (int r = 0; r < h; r++) {
        c64_u32 *row = shadow_buf + (c64_u64)(y + r) * SCRW + x;
        for (int i = 0; i < w; i++)
            row[i] = c;
    }
}

/* just the glyphs the test card needs, 8x8, MSB = leftmost pixel */
static const char glyph_set[] = "PC64 M0ARH1";
static const c64_u8 glyphs[][8] = {
    {0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x00},   /* P */
    {0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00},   /* C */
    {0x3C,0x40,0x40,0x7C,0x42,0x42,0x3C,0x00},   /* 6 */
    {0x44,0x44,0x44,0x7E,0x04,0x04,0x04,0x00},   /* 4 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},   /* space */
    {0x42,0x66,0x5A,0x42,0x42,0x42,0x42,0x00},   /* M */
    {0x3C,0x42,0x46,0x5A,0x62,0x42,0x3C,0x00},   /* 0 */
    {0x3C,0x42,0x42,0x7E,0x42,0x42,0x42,0x00},   /* A */
    {0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00},   /* R */
    {0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x00},   /* H */
    {0x08,0x18,0x08,0x08,0x08,0x08,0x1C,0x00},   /* 1 */
};

static void text(int x, int y, int scale, c64_u32 c, const char *s)
{
    for (; *s; s++, x += 8 * scale) {
        const c64_u8 *g = 0;
        for (unsigned i = 0; i < sizeof glyph_set - 1; i++)
            if (glyph_set[i] == *s)
                g = glyphs[i];
        if (!g)
            continue;
        for (int r = 0; r < 8; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] & (0x80u >> b))
                    frect(x + b * scale, y + r * scale, scale, scale, c);
    }
}

static void draw_card(void)
{
    frect(0, 0, SCRW, SCRH, 0xFF103070u);                 /* deep blue field  */
    frect(0, 0, SCRW, 4, 0xFFFFFFFFu);                    /* border           */
    frect(0, SCRH - 4, SCRW, 4, 0xFFFFFFFFu);
    frect(0, 0, 4, SCRH, 0xFFFFFFFFu);
    frect(SCRW - 4, 0, 4, SCRH, 0xFFFFFFFFu);
    text(SCRW / 2 - 7 * 8 * 6 / 2, 120, 6, 0xFFFFFFFFu, "PC64 M1");
    text(SCRW / 2 - 9 * 8 * 3 / 2, 200, 3, 0xFF80C0FFu, "AARCH64 C");
    static const c64_u32 bars[8] = { 0xFFFFFFFFu, 0xFFFFFF00u, 0xFF00FFFFu,
        0xFF00FF00u, 0xFFFF00FFu, 0xFFFF0000u, 0xFF0000FFu, 0xFF000000u };
    for (int i = 0; i < 8; i++)
        frect(40 + i * 70, 320, 70, 100, bars[i]);
    /* an FP-computed gradient: visible proof CPACR is set and the FPU works */
    for (int x = 0; x < SCRW - 80; x++) {
        float t = (float)x / (float)(SCRW - 80);
        c64_u32 g = (c64_u32)(t * 255.0f);
        frect(40 + x, 440, 1, 24, 0xFF000000u | (g << 16) | (g << 8) | g);
    }
}

static void present(void)
{
    c64_u8 *drow = (c64_u8 *)FBDBG->fb_dorigin;
    c64_u32 ppitch = FBDBG->fb_ppitch;
    const c64_u8 *srow = (const c64_u8 *)shadow_buf + (SCRW - 1) * 4;
    for (int sr = 0; sr < C64_DST_H / FB_SCALE; sr++) {
        for (int rep = 0; rep < FB_SCALE; rep++) {
            c64_u32 *dst = (c64_u32 *)drow;
            const c64_u8 *s = srow;
            for (int sc = 0; sc < C64_DST_W / FB_SCALE; sc++) {
                c64_u32 px = *(const c64_u32 *)s;
                for (int k = 0; k < FB_SCALE; k++)
                    *dst++ = px;
                s += SCRW * 4;
            }
            drow += ppitch;
        }
        srow -= 4;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

void c_main(void *dtb)
{
    c64_beacon(224, 0xFFFF00FFu);   /* MAGENTA: C reached, the stack works   */
    mmu_init();
    c64_beacon(272, 0xFFFFFF00u);   /* YELLOW: translation + caches survived */
    c64_u32 ppitch;
    c64_fb_adopt(dtb, &ppitch);
    FBDBG->fb_shadow = (c64_u64)shadow_buf;
    FBDBG->fb_base = (c64_u64)shadow_buf;
    FBDBG->fb_pitch = SCRW * 4;
    draw_card();
    c64_bcn(BCN_MAIN);
    for (;;) {
        present();
        c64_u64 until = c64_cnt_now() + FRAME_TICKS;
        while (c64_cnt_now() < until)
            ;
    }
}
