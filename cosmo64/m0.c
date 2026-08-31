/* cosmo64/m0.c -- pc64-on-ARM milestone 0: the C toolchain, end to end.
 *
 * This is deliberately NOT "hello world": it is the asm port's fb_init/fb_present
 * contract reimplemented in freestanding C, compiled with llvm-mingw for
 * aarch64-w64-mingw32 (PE/COFF, LLP64 -- the same object format and data model
 * the x86 pc64 uses), flattened by flatten.py and wrapped by cosmo/mkbootimg.py.
 * Because it honours the same FBINFO debug contract as cosmo/kernel.s, the
 * EXISTING cosmo/harness.py gates it across all fifteen FDT combinations with
 * zero changes -- videolfb walk, framebuffer adoption, beacon, and the
 * pixel-exact rotated+scaled eye check.
 *
 * What M0 proves, per research/pc64-arm-port-plan.md in hmofet/cosmo:
 *   - clang/lld emit correct AArch64 from C in this exotic link recipe
 *     (-nostdlib, fixed image base, no CRT, .rdata const data, PE -> flat);
 *   - the LK entry contract works from C (entry.s is ~10 instructions);
 *   - the FDT walk, vram clear, shadow pick and rotated present -- the heart of
 *     the future platform layer's display code -- work as C.
 *
 * Compile flags that are load-bearing:
 *   -mstrict-align       MMU is off, so all memory is Device and an unaligned
 *                        access faults; the compiler must not merge stores.
 *   -mgeneral-regs-only  FP/SIMD traps until CPACR is set up (an M1 job).
 *   -fno-builtin         so our memset is not "optimised" into a call to itself.
 */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define SCRW 640
#define SCRH 480
#define PANEL_W 1080
#define PANEL_H 2160
#define COSMO_PITCH 4352u              /* ALIGN(1080,32)*4, LK's panel stride  */
#define COSMO_FB 0x7E070000ull         /* last-resort guess: 0x80000000 - vram */
#define COSMO_SHADOW 0x40500000ull     /* fallback shadow, clear of carveouts  */
#define FB_SEED_MAGIC 0x53454544u      /* "SEED": FBINFO was deliberately set  */
#define BCN_MAGIC 0x554E4F31u          /* "UNO1" */
#define FB_ROT 270                     /* the panel mounting; see cosmo/README */
#define FB_SCALE 2
#define SHADOW_PITCH (SCRW * 4)
#define SHADOW_BYTES ((u64)SCRW * SCRH * 4)
#define DST_W (SCRH * FB_SCALE)        /* rot 270: the rect is SCRH wide...    */
#define DST_H (SCRW * FB_SCALE)        /* ...and SCRW tall, times the scale    */
#define DST_X0 ((PANEL_W - DST_W) / 2)
#define DST_Y0 ((PANEL_H - DST_H) / 2)
#define FRAME_TICKS 216667             /* ~60 Hz at the MT6771's 13 MHz        */

enum { FB_SRC_FALLBACK = 0, FB_SRC_BLOB = 1, FB_SRC_PROPS = 2, FB_SRC_SEED = 3 };
enum { BCN_FBFALL = 2, BCN_FBDTB = 3, BCN_MAIN = 4 };

/* The FBINFO debug contract at 0x40320000 -- field for field the layout
 * cosmo/kernel.s publishes and cosmo/harness.py reads back. */
struct fbdbg {
    u64 fb_base;        /* +0  drawing origin: the shadow, once picked   */
    u32 fb_pitch;       /* +8  drawing pitch: the shadow's              */
    u32 pad0;
    u64 dtb_ptr;        /* +16 LK's x0                                   */
    u32 fb_vram;        /* +24 vramSize from the DTB (0 = unknown)       */
    u32 pad1;
    u64 fb_raw;         /* +32 panel base BEFORE centring                */
    u32 fb_src;         /* +40 FB_SRC_*                                  */
    u32 bcn_stage;      /* +44 last beacon stage reached                 */
    u32 bcn_magic;      /* +48 BCN_MAGIC once a stage is marked          */
    u32 fb_seed;        /* +52 FB_SEED_MAGIC = fb_base/fb_pitch seeded   */
    u64 fb_panel;       /* +56 alias of fb_raw                           */
    u32 fb_ppitch;      /* +64 LK's panel stride                         */
    u32 pad2;
    u64 fb_dorigin;     /* +72 top-left of the rotated UI rect           */
    u64 fb_shadow;      /* +80 the upright SCRWxSCRH surface             */
    u32 fb_scale;       /* +88 FB_SCALE this build presents at           */
};
_Static_assert(__builtin_offsetof(struct fbdbg, fb_raw) == 32, "fbdbg layout");
_Static_assert(__builtin_offsetof(struct fbdbg, fb_panel) == 56, "fbdbg layout");
_Static_assert(__builtin_offsetof(struct fbdbg, fb_scale) == 88, "fbdbg layout");

#define FB ((volatile struct fbdbg *)0x40320000ull)

/* ---- tiny freestanding runtime ----------------------------------------- */

void *memset(void *dst, int c, unsigned long long n)
{
    u8 *d = dst;
    while (n--)
        *d++ = (u8)c;
    return dst;
}

void *memcpy(void *dst, const void *src, unsigned long long n)
{
    u8 *d = dst;
    const u8 *s = src;
    while (n--)
        *d++ = *s++;
    return dst;
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* byte-assembled loads: FDT prop data is only 4-aligned, and with the MMU off
 * even a merged 8-byte load of it would fault under -mstrict-align anyway */
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u32 le32(const u8 *p) { return ((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0]; }
static u64 le64(const u8 *p) { return ((u64)le32(p + 4) << 32) | le32(p); }

static u64 cnt_now(void)
{
    u64 v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

/* ---- the videolfb walk (fb_dtb_scan, in C) ------------------------------ */
/* Walk /chosen for LK's framebuffer handoff. The packed little-endian
 * "atag,videolfb" blob is what a production LK emits and it WINS; the
 * big-endian -fb_base_h/_l/-vramSize trio is the FPGA shape, and on the real
 * device it is stale preloader data (see cosmo/README.md). */
static u32 fdt_scan(const void *dtb, u64 *base, u32 *vram)
{
    const u8 *d = dtb;
    if (!d || be32(d) != 0xd00dfeedu)
        return 0;
    u32 total = be32(d + 4);
    if (total < 40 || total > 0x200000)
        return 0;
    const u8 *p = d + be32(d + 8);
    const u8 *strs = d + be32(d + 12);
    const u8 *end = d + total;
    int depth = 0, inchosen = 0;
    u64 blob_base = 0;
    u32 blob_vram = 0;
    int have_blob = 0;
    u32 ph = 0, pl = 0, pv = 0;
    int have_l = 0;

    while (p + 4 <= end) {
        u32 tok = be32(p);
        p += 4;
        if (tok == 4) {                          /* FDT_NOP */
            continue;
        } else if (tok == 1) {                   /* FDT_BEGIN_NODE */
            const char *name = (const char *)p;
            while (p < end && *p)
                p++;
            p = (const u8 *)(((u64)p + 4) & ~3ull);   /* skip NUL, pad to 4 */
            depth++;
            if (depth == 2)
                inchosen = streq(name, "chosen");
        } else if (tok == 2) {                   /* FDT_END_NODE */
            if (--depth < 1)
                inchosen = 0;
            if (depth < 1)
                break;
        } else if (tok == 3) {                   /* FDT_PROP */
            if (p + 8 > end)
                return 0;
            u32 len = be32(p), nameoff = be32(p + 4);
            p += 8;
            const u8 *prop = p;
            p += (len + 3) & ~3u;
            if (p > end)
                return 0;
            if (!inchosen)
                continue;
            const char *pn = (const char *)strs + nameoff;
            if (streq(pn, "atag,videolfb") && len >= 20) {
                blob_base = le64(prop);          /* native LE inside a BE tree */
                blob_vram = le32(prop + 16);
                have_blob = 1;
            } else if (streq(pn, "atag,videolfb-fb_base_l") && len >= 4) {
                pl = be32(prop);
                have_l = 1;
            } else if (streq(pn, "atag,videolfb-fb_base_h") && len >= 4) {
                ph = be32(prop);
            } else if (streq(pn, "atag,videolfb-vramSize") && len >= 4) {
                pv = be32(prop);
            }
        } else if (tok == 9) {                   /* FDT_END */
            break;
        } else {
            return 0;                            /* junk: not a real tree */
        }
    }
    if (have_blob && blob_base) {
        *base = blob_base;
        *vram = blob_vram;
        return FB_SRC_BLOB;
    }
    if (have_l) {
        u64 b = ((u64)ph << 32) | pl;
        if (b) {
            *base = b;
            *vram = pv;
            return FB_SRC_PROPS;
        }
    }
    return 0;
}

void mmu_init(void);

/* ---- drawing into the upright shadow ------------------------------------ */
/* The shadow lives in OUR OWN .bss, not in LK's vram. The asm port parks it in
 * vram page 1, and the first-light photographs showed that page scanned out as
 * a garbage band beside the UI -- a leftover display layer still composites
 * it. Image memory is ours by definition, and with the MMU on it is cacheable,
 * which also makes the present()'s reads fast. */
static u32 shadow_buf[SCRW * SCRH] __attribute__((aligned(64)));
static u32 *g_sh;                                /* = shadow_buf */

static void frect(int x, int y, int w, int h, u32 c)
{
    for (int r = 0; r < h; r++) {
        u32 *row = g_sh + (u64)(y + r) * SCRW + x;
        for (int i = 0; i < w; i++)
            row[i] = c;
    }
}

/* just the glyphs the test card needs, 8x8, MSB = leftmost pixel */
static const char glyph_set[] = "PC64 M0ARH";
static const u8 glyphs[][8] = {
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
};

static void text(int x, int y, int scale, u32 c, const char *s)
{
    for (; *s; s++, x += 8 * scale) {
        const u8 *g = 0;
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
    text(SCRW / 2 - 7 * 8 * 6 / 2, 120, 6, 0xFFFFFFFFu, "PC64 M0");
    text(SCRW / 2 - 9 * 8 * 3 / 2, 200, 3, 0xFF80C0FFu, "AARCH64 C");
    static const u32 bars[8] = { 0xFFFFFFFFu, 0xFFFFFF00u, 0xFF00FFFFu,
        0xFF00FF00u, 0xFFFF00FFu, 0xFFFF0000u, 0xFF0000FFu, 0xFF000000u };
    for (int i = 0; i < 8; i++)
        frect(40 + i * 70, 320, 70, 100, bars[i]);
    /* an FP-computed gradient: visible proof CPACR is set and the FPU works
     * (M1 retired -mgeneral-regs-only; this line would trap without it) */
    for (int x = 0; x < SCRW - 80; x++) {
        float t = (float)x / (float)(SCRW - 80);
        u32 g = (u32)(t * 255.0f);
        frect(40 + x, 440, 1, 24, 0xFF000000u | (g << 16) | (g << 8) | g);
    }
}

/* ---- the rotated + scaled present (fb_present, in C) -------------------- */

static void present(u64 dorigin, u32 ppitch)
{
    u8 *drow = (u8 *)dorigin;
    const u8 *srow = (const u8 *)g_sh + (SCRW - 1) * 4;   /* rot 270 start   */
    for (int sr = 0; sr < DST_H / FB_SCALE; sr++) {
        for (int rep = 0; rep < FB_SCALE; rep++) {
            u32 *dst = (u32 *)drow;
            const u8 *s = srow;
            for (int sc = 0; sc < DST_W / FB_SCALE; sc++) {
                u32 px = *(const u32 *)s;
                for (int k = 0; k < FB_SCALE; k++)
                    *dst++ = px;
                s += SHADOW_PITCH;                        /* SRC_IN          */
            }
            drow += ppitch;
        }
        srow -= 4;                                        /* SRC_OUT         */
    }
    __asm__ volatile("dsb sy" ::: "memory");              /* panel DMAs DRAM */
}

static void bcn(u32 stage)
{
    FB->bcn_stage = stage;
    FB->bcn_magic = BCN_MAGIC;
}

void c_main(void *dtb)
{
    /* M1: vectors are live (entry.s), now translation and caches -- everything
     * after this line runs on a cached, MMU-on CPU with the framebuffer region
     * mapped non-cacheable so the display DMA stays coherent */
    mmu_init();

    /* distrust FBINFO unless deliberately seeded -- on hardware it is
     * whatever DRAM woke up as */
    u64 seed_base = 0;
    u32 seed_pitch = 0;
    if (FB->fb_seed == FB_SEED_MAGIC) {
        seed_base = FB->fb_base;
        seed_pitch = FB->fb_pitch;
    }
    FB->dtb_ptr = (u64)dtb;

    u64 base = 0;
    u32 vram = 0;
    u32 src = fdt_scan(dtb, &base, &vram);
    if (!src) {
        vram = 0;
        if (seed_base) {
            base = seed_base;
            src = FB_SRC_SEED;
        } else {
            base = COSMO_FB;
            src = FB_SRC_FALLBACK;
        }
    }
    FB->fb_src = src;
    FB->fb_vram = vram;
    FB->fb_raw = base;
    FB->fb_panel = base;
    bcn(src == FB_SRC_FALLBACK ? BCN_FBFALL : BCN_FBDTB);

    u32 ppitch = seed_pitch ? seed_pitch : COSMO_PITCH;
    FB->fb_ppitch = ppitch;

    /* clear LK's vram -- all of it when the DTB proved a size (spare pages
     * and the DAL layer included), one visible page otherwise */
    u64 page = (u64)PANEL_H * ppitch;
    u64 clr = vram ? vram : page;
    u64 *z = (u64 *)base;
    for (u64 i = 0; i < clr / 8; i++)
        z[i] = 0;

    /* bar beacon: a white 32x32 block at the raw origin */
    for (int r = 0; r < 32; r++) {
        u32 *row = (u32 *)(base + (u64)r * ppitch);
        for (int c = 0; c < 32; c++)
            row[c] = 0xFFFFFFFFu;
    }

    /* the shadow: our own .bss (see shadow_buf above for why not vram page 1) */
    u64 shadow = (u64)shadow_buf;
    g_sh = shadow_buf;
    FB->fb_shadow = shadow;
    FB->fb_base = shadow;
    FB->fb_pitch = SHADOW_PITCH;

    u64 dorigin = base + (u64)DST_Y0 * ppitch + DST_X0 * 4;
    FB->fb_dorigin = dorigin;
    FB->fb_scale = FB_SCALE;

    draw_card();
    bcn(BCN_MAIN);

    for (;;) {
        present(dorigin, ppitch);
        u64 until = cnt_now() + FRAME_TICKS;
        while (cnt_now() < until)
            ;
    }
}
