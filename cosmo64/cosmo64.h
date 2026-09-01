/* cosmo64.h -- shared platform-layer definitions for pc64-on-ARM (Cosmo). */
#ifndef COSMO64_H
#define COSMO64_H

typedef unsigned char c64_u8;
typedef unsigned int c64_u32;
typedef unsigned long long c64_u64;

#define PANEL_W 1080
#define PANEL_H 2160
#define COSMO_PITCH 4352u              /* ALIGN(1080,32)*4, LK's panel stride  */
#define COSMO_FB 0x7E070000ull         /* last-resort guess: 0x80000000 - vram */
#define FB_SEED_MAGIC 0x53454544u      /* "SEED": FBINFO was deliberately set  */
#define BCN_MAGIC 0x554E4F31u          /* "UNO1" */
#define FB_ROT 270
#define FB_SCALE 2

/* Memory layout. EVERYTHING mutable lives in the image's own .bss -- stack,
 * FBINFO debug block, crash record -- because that is the one stretch of DRAM
 * the boot proves writable (the entry zero loop covers all of it) before
 * anything depends on it. The green+cyan+magenta+yellow boot of 2026-09-01
 * killed the off-image variants one by one: the 0x53E00000 stack and then the
 * 0x53F00000 FBINFO block were the only unproven atoms left in their failure
 * windows. The debug block's address is published into the flat image's own
 * header (offset 0x30, the ARM64 res4 word) at boot so the harness can find
 * it. flatten.py asserts the image ends below C64_IMAGE_CEIL (LK's DTB is at
 * 0x54000000; ram_console at 0x54400000 stays untouched for forensics). */
#define C64_IMAGE_CEIL 0x53000000ull
#define C64_DBG_PTR_SLOT 0x40080030ull   /* u64: &c64_dbg_page, entry.s writes */

/* one page of fbdbg + the crash record at +0x1000 */
extern c64_u8 c64_dbg_page[0x1100];

enum { FB_SRC_FALLBACK = 0, FB_SRC_BLOB = 1, FB_SRC_PROPS = 2, FB_SRC_SEED = 3 };
enum { BCN_FBFALL = 2, BCN_FBDTB = 3, BCN_MAIN = 4 };

/* The UI surface is 640x480; the rotated (270) 2x rect it lands in: */
#define C64_SCRW 640
#define C64_SCRH 480
#define C64_DST_W (C64_SCRH * FB_SCALE)
#define C64_DST_H (C64_SCRW * FB_SCALE)
#define C64_DST_X0 ((PANEL_W - C64_DST_W) / 2)
#define C64_DST_Y0 ((PANEL_H - C64_DST_H) / 2)

/* The FBINFO debug contract -- field for field what qharness.py reads back. */
struct fbdbg {
    c64_u64 fb_base;        /* +0  drawing origin: the source surface        */
    c64_u32 fb_pitch;       /* +8  its pitch                                 */
    c64_u32 pad0;
    c64_u64 dtb_ptr;        /* +16 LK's x0                                   */
    c64_u32 fb_vram;        /* +24 vramSize from the DTB (0 = unknown)       */
    c64_u32 pad1;
    c64_u64 fb_raw;         /* +32 panel base BEFORE centring                */
    c64_u32 fb_src;         /* +40 FB_SRC_*                                  */
    c64_u32 bcn_stage;      /* +44 last beacon stage reached                 */
    c64_u32 bcn_magic;      /* +48 BCN_MAGIC once a stage is marked          */
    c64_u32 fb_seed;        /* +52 FB_SEED_MAGIC = fb_base/fb_pitch seeded   */
    c64_u64 fb_panel;       /* +56 alias of fb_raw                           */
    c64_u32 fb_ppitch;      /* +64 LK's panel stride                         */
    c64_u32 pad2;
    c64_u64 fb_dorigin;     /* +72 top-left of the rotated UI rect           */
    c64_u64 fb_shadow;      /* +80 the upright source surface                */
    c64_u32 fb_scale;      /* +88 FB_SCALE this build presents at           */
};
_Static_assert(__builtin_offsetof(struct fbdbg, fb_raw) == 32, "fbdbg layout");
_Static_assert(__builtin_offsetof(struct fbdbg, fb_panel) == 56, "fbdbg layout");
_Static_assert(__builtin_offsetof(struct fbdbg, fb_scale) == 88, "fbdbg layout");

#define FBDBG ((volatile struct fbdbg *)c64_dbg_page)

/* cpu.s / mmu.c */
void cpu_early_init(void);
void dcache_inv_all(void);
void mmu_on(c64_u64 ttbr0, c64_u64 mair, c64_u64 tcr);
void mmu_init(void);

/* videolfb.c: walk /chosen for LK's framebuffer handoff
 * (FB_SRC_BLOB/FB_SRC_PROPS with *base/*vram set, or 0), adopt it
 * (clear vram, bar beacon, publish the FBINFO contract; returns the raw
 * panel base and fills *ppitch), and mark beacon stages. */
c64_u32 c64_fdt_scan(const void *dtb, c64_u64 *base, c64_u32 *vram);
c64_u64 c64_fb_adopt(void *dtb, c64_u32 *ppitch);
void c64_bcn(c64_u32 stage);

/* Stage beacon painted straight onto the panel at the MEASURED base/pitch
 * (0x7DF70000/4352, mblock-7-framebuffer, device-verified). Works with the
 * MMU off (Device memory) and on (the region is mapped Normal-NC). The adopt
 * path's vram clear wipes them. Diagnostic bring-up aid; remove at polish. */
static inline void c64_beacon(int x, c64_u32 color)
{
    volatile c64_u32 *p = (volatile c64_u32 *)(0x7DF70000ull + (c64_u64)x * 4);
    for (int r = 0; r < 32; r++) {
        for (int c = 0; c < 32; c++)
            p[c] = color;
        p += 4352 / 4;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* the boot stack lives in the image's own .bss (videolfb.c) -- the one DRAM
 * the zero loop proves writable before anything depends on it */
#define C64_BOOT_STACK_BYTES 0x80000
extern c64_u8 c64_boot_stack[C64_BOOT_STACK_BYTES];

/* i2c.c: polled MTK I2C. Returns <0 on error/NAK. Transfers are PIO, so
 * each direction is capped at the 8-byte FIFO. */
#define C64_I2C_KBD 0                /* bus 4 @ 0x11008000, arbitrated */
#define C64_I2C_TP  1                /* bus 0 @ 0x11007000, plain      */
int c64_i2c_init(int bus);
int c64_i2c_xfer(int bus, c64_u8 dev, const c64_u8 *wr, int nwr,
                 c64_u8 *rd, int nrd);
int c64_i2c_write_reg(int bus, c64_u8 dev, c64_u8 reg, c64_u8 val);
int c64_i2c_read_reg(int bus, c64_u8 dev, c64_u8 reg);
void c64_kbd_power(int on);          /* AW9523 SHDN/HWEN, GPIO175 */

/* touch.c: the NT36672 panel as the shell's pointer */
void c64_touch_init(void);
void c64_touch_poll(void);
int c64_touch_present(void);

/* kbd.c: the AW9523 matrix keyboard -> the input ring */
void c64_kbd_init(void);
void c64_kbd_poll(void);
int c64_kbd_present(void);

/* input.c: producers push edges and publish level state */
void c64_key_push(int scan, int uni, int mods);
void c64_input_set_level(int mods, int held);
void c64_input_set_pointer(int x, int y, int btn);

static inline c64_u64 c64_cnt_now(void)
{
    c64_u64 v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline c64_u64 c64_cnt_freq(void)
{
    c64_u64 v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

#endif
