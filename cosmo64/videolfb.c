/* cosmo64/videolfb.c -- the /chosen videolfb walk, shared by the m0 test
 * payload and the shell's display layer. The packed little-endian
 * "atag,videolfb" blob is what a production LK emits and it WINS; the
 * big-endian -fb_base_h/_l/-vramSize trio is the FPGA shape, and on the real
 * device it is stale preloader data. Byte-assembled loads throughout: prop
 * data is only 4-aligned and pre-MMU code runs on Device memory. */

#include "cosmo64.h"

/* the boot stack (see cosmo64.h): entry.s points SP at the top of this */
c64_u8 c64_boot_stack[C64_BOOT_STACK_BYTES] __attribute__((aligned(16)));

/* the debug page: fbdbg contract + the crash record at +0x1000 (cosmo64.h) */
c64_u8 c64_dbg_page[0x1100] __attribute__((aligned(4096)));

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static c64_u32 be32(const c64_u8 *p)
{
    return ((c64_u32)p[0] << 24) | ((c64_u32)p[1] << 16) | ((c64_u32)p[2] << 8) | p[3];
}

static c64_u32 le32(const c64_u8 *p)
{
    return ((c64_u32)p[3] << 24) | ((c64_u32)p[2] << 16) | ((c64_u32)p[1] << 8) | p[0];
}

static c64_u64 le64(const c64_u8 *p)
{
    return ((c64_u64)le32(p + 4) << 32) | le32(p);
}

c64_u32 c64_fdt_scan(const void *dtb, c64_u64 *base, c64_u32 *vram)
{
    const c64_u8 *d = dtb;
    if (!d || be32(d) != 0xd00dfeedu)
        return 0;
    c64_u32 total = be32(d + 4);
    if (total < 40 || total > 0x200000)
        return 0;
    const c64_u8 *p = d + be32(d + 8);
    const c64_u8 *strs = d + be32(d + 12);
    const c64_u8 *end = d + total;
    int depth = 0, inchosen = 0;
    c64_u64 blob_base = 0;
    c64_u32 blob_vram = 0;
    int have_blob = 0;
    c64_u32 ph = 0, pl = 0, pv = 0;
    int have_l = 0;

    while (p + 4 <= end) {
        c64_u32 tok = be32(p);
        p += 4;
        if (tok == 4) {                          /* FDT_NOP */
            continue;
        } else if (tok == 1) {                   /* FDT_BEGIN_NODE */
            const char *name = (const char *)p;
            while (p < end && *p)
                p++;
            p = (const c64_u8 *)(((c64_u64)p + 4) & ~3ull);
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
            c64_u32 len = be32(p), nameoff = be32(p + 4);
            p += 8;
            const c64_u8 *prop = p;
            p += (len + 3) & ~3u;
            if (p > end)
                return 0;
            if (!inchosen)
                continue;
            const char *pn = (const char *)strs + nameoff;
            if (streq(pn, "atag,videolfb") && len >= 20) {
                blob_base = le64(prop);
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
        c64_u64 b = ((c64_u64)ph << 32) | pl;
        if (b) {
            *base = b;
            *vram = pv;
            return FB_SRC_PROPS;
        }
    }
    return 0;
}

void c64_bcn(c64_u32 stage)
{
    FBDBG->bcn_stage = stage;
    FBDBG->bcn_magic = BCN_MAGIC;
}

/* Adopt LK's framebuffer: resolve the base (DTB blob > props > seeded FBINFO >
 * the guess), clear ALL of LK's vram when a size is proven (spare pages and
 * the DAL layer hold recovery-console leftovers that scan out otherwise --
 * hardware finding, 2026-08-31), paint the white bar beacon, and publish the
 * FBINFO contract including the centred rotated-rect origin. */
c64_u64 c64_fb_adopt(void *dtb, c64_u32 *ppitch_out)
{
    c64_u64 seed_base = 0;
    c64_u32 seed_pitch = 0;
    if (FBDBG->fb_seed == FB_SEED_MAGIC) {
        seed_base = FBDBG->fb_base;
        seed_pitch = FBDBG->fb_pitch;
    }
    FBDBG->dtb_ptr = (c64_u64)dtb;

    c64_u64 base = 0;
    c64_u32 vram = 0;
    c64_u32 src = c64_fdt_scan(dtb, &base, &vram);
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
    FBDBG->fb_src = src;
    FBDBG->fb_vram = vram;
    FBDBG->fb_raw = base;
    FBDBG->fb_panel = base;
    c64_bcn(src == FB_SRC_FALLBACK ? BCN_FBFALL : BCN_FBDTB);

    c64_u32 ppitch = seed_pitch ? seed_pitch : COSMO_PITCH;
    FBDBG->fb_ppitch = ppitch;

    c64_beacon(320, 0xFFFFFFFFu);   /* WHITE: FDT walk + FBINFO writes done */

    c64_u64 page = (c64_u64)PANEL_H * ppitch;
    c64_u64 clr = vram ? vram : page;
    c64_u64 *z = (c64_u64 *)base;
    for (c64_u64 i = 0; i < clr / 8; i++)
        z[i] = 0;

    for (int r = 0; r < 32; r++) {
        c64_u32 *row = (c64_u32 *)(base + (c64_u64)r * ppitch);
        for (int c = 0; c < 32; c++)
            row[c] = 0xFFFFFFFFu;
    }

    FBDBG->fb_dorigin = base + (c64_u64)C64_DST_Y0 * ppitch + C64_DST_X0 * 4;
    FBDBG->fb_scale = FB_SCALE;
    *ppitch_out = ppitch;
    return base;
}
