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

/* The panel is FIXED -- 1080x2160 portrait, mounted landscape -- so its native
 * size is 2160x1080, and the desktop is presented at a whole-pixel zoom of it.
 * (It started 640x480 at 2x, the size inherited from the rpi port this lane
 * began as; that left a 960x1280 window on a 1080x2160 panel.)
 *
 * THE DEFAULT IS ZOOM 2: a 1080x540 desktop covering the whole 2160x1080
 * panel. That is the phone convention -- a 2x device pixel ratio, logical
 * points at half the physical pixels -- and at 403 DPI it is the difference
 * between a readable UI and a beautiful unreadable one. The true 2160x1080
 * desktop is one click away in Control Panel > Display.
 *
 * Do NOT reach for the shell's "UI scale" preference to get this effect here.
 * uno_font_set_ui_scale() is a no-op unless a TTF face is loaded (pc64_font.c
 * gates it on g_active >= 0), the faces are read off a FAT volume, and this
 * device has none -- so the shell runs on the built-in 8x8 and that dropdown
 * does nothing at all. The zoom is the knob that works.
 *
 * FB_SCALE is the zoom of the STARTING size, kept as a macro because the m0
 * and calib payloads are static and compile their geometry in. The shell's is
 * runtime -- c64_scale, below -- because Control Panel > Display can change
 * it. */
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
 * 0x54000000; the kernel's persistent-RAM reservations at 0x54400000 are the
 * log's, see below). */
#define C64_IMAGE_CEIL 0x53000000ull
#define C64_DBG_PTR_SLOT 0x40080030ull   /* u64: &c64_dbg_page, entry.s writes */

/* log.c: the persistent debug log. This is the ramoops CONSOLE zone of the
 * Gemian kernel's pstore reservation -- derived in full in log.c's header
 * comment, and reserved, preserved across reset and read back at the next
 * Linux boot by the kernel itself. Mapped Normal-NC (mmu.c) so a warm reset
 * cannot strand the tail of the log in the D-cache. */
#define C64_LOG_ZONE 0x5449F000ull       /* pstore 0x54410000 + dump 0x8F000  */
#define C64_LOG_SIZE 0x40000u            /* ramoops console_size              */
#define C64_LOG_NC_BLOCK 0x54400000ull   /* the 2 MB block that contains it   */

/* M4: where the linker put xhci.c's DMA memory (the ".xdma" section; see
 * c64_usbglue.h). flatten.py writes the absolute start and end as two u64s at
 * image offsets 0x40 and 0x48 -- just past the 64-byte ARM64 Image header,
 * inside the page nothing else uses -- and mmu.c maps the range as Device
 * memory. Both zero when the image carries no such section. */
#define C64_XDMA_SLOT 0x40080040ull

void c64_log_init(void);
void c64_log(const char *s);
void c64_log_write(const char *s, unsigned n);
void c64_logf(const char *fmt, ...);
void c64_logv(const char *fmt, __builtin_va_list ap);
void c64_dbg_log(const char *fmt, ...);      /* pc64's uno_dbg_log, routed */
unsigned c64_log_bytes(void);
unsigned c64_log_total(void);                /* bytes ever written, monotonic */
void c64_log_read(unsigned off, c64_u8 *dst, unsigned n);
/* survey() counts surviving ramoops signatures and MUST run before init()
 * overwrites one of them; report() says what it found, once the log exists. */
unsigned c64_log_survey(void);
void c64_log_survey_report(void);
/* msdc.c: push the log to its durable home in p38's tail. The DRAM copy
 * survives the reset but never reaches pstore (cause open); this one is read
 * back by readlog.sh from any Linux boot. */
void c64_log_flush(void);
extern c64_u8 c64_fault_stack[0x2000];

/* one page of fbdbg + the crash record at +0x1000 */
extern c64_u8 c64_dbg_page[0x1100];

enum { FB_SRC_FALLBACK = 0, FB_SRC_BLOB = 1, FB_SRC_PROPS = 2, FB_SRC_SEED = 3 };
enum { BCN_FBFALL = 2, BCN_FBDTB = 3, BCN_MAIN = 4 };

/* The STARTING UI surface -- half the panel's native landscape size, so that
 * at FB_SCALE 2 it covers the panel exactly -- and the rotated (270) rect it
 * lands in. The static payloads (m0, calib) use these directly; the shell uses
 * the runtime geometry below, which starts here. */
#define C64_SCRW 1080
#define C64_SCRH 540
#define C64_DST_W (C64_SCRH * FB_SCALE)
#define C64_DST_H (C64_SCRW * FB_SCALE)
#define C64_DST_X0 ((PANEL_W - C64_DST_W) / 2)
#define C64_DST_Y0 ((PANEL_H - C64_DST_H) / 2)

/* The ceiling every desktop-sized buffer is allocated at. A smaller desktop
 * is presented at a bigger integer zoom, so the rect is always the panel or
 * less, and these are exactly the panel's two axes swapped. */
#define C64_UI_MAX_W PANEL_H
#define C64_UI_MAX_H PANEL_W

/* Runtime desktop geometry (videolfb.c owns it; display.c changes it).
 * c64_scale is the biggest integer zoom at which c64_scrw x c64_scrh still
 * fits the panel rotated, and dst_* is the resulting centred rect in panel
 * pixels. Whole-pixel zoom only: a fractional nearest-neighbour upscale
 * duplicates some source columns and not others, which mangles glyph stems --
 * the x86 port learned that one and floors its scale for the same reason. */
extern int c64_scrw, c64_scrh, c64_scale;
extern int c64_dst_x0, c64_dst_y0, c64_dst_w, c64_dst_h;
void c64_geom_set(int w, int h);
/* Wipe every byte of LK's vram. Needed when the UI rect SHRINKS: the pixels
 * the old rect owned are outside the new one, so nothing will ever overwrite
 * them and the previous desktop stays on screen as a frame around this one. */
void c64_fb_clear_panel(void);

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
    c64_u32 fb_scale;       /* +88 the integer zoom being presented at       */
    c64_u32 fb_scrw;        /* +92 the desktop size behind it                */
    c64_u32 fb_scrh;        /* +96                                           */
    c64_u32 pad3;
    c64_u64 fb_presented;   /* +104 display.c's copy of the LAST PRESENTED
                             *      frame (its dirty-compare shadow), 0 in
                             *      the static payloads. The gate compares
                             *      the panel against this when it caught
                             *      fb[] mid-render -- see qharness.py      */
};
_Static_assert(__builtin_offsetof(struct fbdbg, fb_raw) == 32, "fbdbg layout");
_Static_assert(__builtin_offsetof(struct fbdbg, fb_panel) == 56, "fbdbg layout");
_Static_assert(__builtin_offsetof(struct fbdbg, fb_scale) == 88, "fbdbg layout");
_Static_assert(__builtin_offsetof(struct fbdbg, fb_scrh) == 96, "fbdbg layout");
_Static_assert(__builtin_offsetof(struct fbdbg, fb_presented) == 104, "fbdbg layout");

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
/* 1 if the root node's `compatible` names `needle` ("linux,dummy-virt" = the
 * QEMU gate). urc.c picks the URC transport off this. */
int c64_fdt_root_compat_has(const void *dtb, const char *needle);

/* netup.c: has pc64_net_boot() had its one attempt yet? urc.c waits for it,
 * because the listen transport needs an address to bind. */
int c64_net_boot_ran(void);

/* urc.c (M6): bring the unoautomate remote channel up, once, after that. */
void c64_urc_tick(void);

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

/* msdc.c: the eMMC as a block device. LK leaves MSDC0 clocked and the card in
 * the transfer state, so this issues commands rather than bringing anything
 * up. c64_blk_write() refuses any LBA outside the UnoDOS data partition. */
void c64_blk_init(void);
int c64_blk_ready(void);
int c64_blk_read(c64_u64 lba, void *buf, unsigned nblk);
int c64_blk_write(c64_u64 lba, const void *buf, unsigned nblk);
c64_u64 c64_blk_data_lba(void);
c64_u64 c64_blk_data_sectors(void);

/* blk.c: the same partition presented to pc64's storage stack as a block
 * device (LBA 0 = the partition's first sector), and the boot-time report of
 * what mounted -- the log is the only gate storage has, since the QEMU virt
 * board has no MSDC. */
void c64_storage_report(void);

/* display.c: where the frame time goes. c64_perf_loop() is called once per
 * shell loop iteration and reports a breakdown to the log every 2 seconds. */
void c64_perf_add_poll(c64_u64 cyc);
void c64_perf_loop(void);

/* touch.c: the NT36672 panel as the shell's pointer */
void c64_touch_init(void);
void c64_touch_poll(void);
int c64_touch_present(void);
/* the controller's report before any mapping, and the maxima it claims --
 * calib.c measures in these, so the calibration path carries none of the
 * transform it exists to measure */
int c64_touch_raw(int *x, int *y);
void c64_touch_maxima(int *mx, int *my);

/* kbd.c: the AW9523 matrix keyboard -> the input ring */
void c64_kbd_init(void);
void c64_kbd_poll(void);
int c64_kbd_present(void);

/* input.c: producers push edges and publish level state. The touch panel is
 * an absolute pointer (set_pointer); a USB mouse is relative (move_pointer);
 * each keeps its own button state and the shell sees the OR, as on x86. */
void c64_key_push(int scan, int uni, int mods);
void c64_input_set_level(int mods, int held);          /* the AW9523 matrix */
void c64_input_set_level_usb(int mods, int held);      /* a USB keyboard    */
void c64_input_set_pointer(int x, int y, int btn);
void c64_input_move_pointer(int dx, int dy, int btn);
void c64_input_rescale_pointer(int ow, int oh, int nw, int nh);
void c64_input_add_wheel(int notches);

/* ssusb.c: MediaTek's host block, brought to where a standard xHCI driver can
 * take over. usb.c: pc64's xhci.c + usbhid.c on top of it, feeding input.c. */
int  c64_ssusb_present(void);
int  c64_ssusb_host_up(void);
void c64_usb_init(void);
void c64_usb_poll(void);
int  c64_usb_mice(void);
void c64_pci_expose_xhci(int on);    /* pci.c: put the controller on the bus */

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
