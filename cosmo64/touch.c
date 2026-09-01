/* cosmo64/touch.c -- the Cosmo's main touch panel as the shell's pointer.
 *
 * Novatek NT36672 TDDI (the same die drives the display), I2C bus 0, and the
 * addressing is counter-intuitive: point data is read at 7-bit address 0x01
 * (the vendor's "I2C_FW_Address"), while 0x62 -- the address the DT registers
 * -- takes only raw MCU commands. 0x01 sits in I2C's reserved range, which
 * some controllers refuse; the MTK one does not care.
 *
 * Protocol (vendor NT36xxx driver): an "xdata index" page is selected by
 * writing FF <addr23:16> <addr15:8>, after which the low byte is the offset
 * for ordinary reads. The page is STICKY, so it is set once at init. The
 * event buffer is at 0x11E00: offset 0x00 is ten 6-byte finger slots, offset
 * 0x78 is the firmware info block (version, then X/Y maxima).
 *
 * We read ONE finger, not ten: the I2C FIFO is 8 bytes and the vendor driver
 * switches to DMA past that, so an 8-byte read (finger slot 0 plus slack) is
 * the largest PIO transaction available -- and a single contact is exactly
 * what a mouse pointer is. Coordinates arrive in panel-native portrait space
 * (0..1080 x 0..2160), so they are mapped through the inverse of display.c's
 * rotate-and-scale present to land on the 640x480 UI surface.
 *
 * LK powers and lights this panel, and the NT36672 needs no reset GPIO and no
 * regulator of its own (its silicon is the display driver IC), so init is
 * just: select the page, read the FW info, sanity-check it.
 */

#include "cosmo64.h"

#define TP_ADDR 0x01                     /* NOT 0x62: see the header note */
#define EVENT_BUF 0x11E00
#define OFF_FWINFO 0x78

static int g_present;
static int g_x = C64_SCRW / 2, g_y = C64_SCRH / 2, g_btn;
static c64_u32 g_maxx = 1080, g_maxy = 2160;

int c64_touch_present(void)
{
    return g_present;
}

#ifdef C64_TOUCHDBG
/* Paint a 32-bit value as bit-cells straight onto the panel, in the black
 * band ABOVE the UI rect (panel y < C64_DST_Y0) so the shell's present never
 * covers it -- in the landscape view that band is the strip along one edge.
 * White = 1, dark = 0, bit 31 leftmost. A photograph decodes the raw touch
 * report, which is the only channel this device has. */
static void dbg_word(int py, c64_u32 v)
{
    for (int i = 0; i < 32; i++) {
        c64_u32 col = (v & (1u << (31 - i))) ? 0xFFFFFFFFu : 0xFF303030u;
        for (int r = 0; r < 24; r++) {
            volatile c64_u32 *p = (volatile c64_u32 *)
                (0x7DF70000ull + (c64_u64)(py + r) * 4352 + (280 + i * 16) * 4);
            for (int c = 0; c < 12; c++)
                p[c] = col;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
#endif

static int set_page(c64_u32 addr)
{
    c64_u8 w[3] = { 0xFF, (c64_u8)(addr >> 16), (c64_u8)(addr >> 8) };
    return c64_i2c_xfer(C64_I2C_TP, TP_ADDR, w, 3, 0, 0);
}

void c64_touch_init(void)
{
    if (c64_i2c_init(C64_I2C_TP) < 0)
        return;
    if (set_page(EVENT_BUF) < 0)
        return;
    /* the FW info block: ver, ~ver, then x_num/y_num and the maxima. This is
     * a read-only presence test -- unlike the vendor's chip-ID trim check,
     * which bootloader-resets the part and halts its MCU. */
    c64_u8 off = OFF_FWINFO, info[8];
    if (c64_i2c_xfer(C64_I2C_TP, TP_ADDR, &off, 1, info, 8) < 0)
        return;
    if (((info[0] + info[1]) & 0xFF) != 0xFF)
        return;                          /* not an answering NT36xxx */
    c64_u32 mx = ((c64_u32)info[4] << 8) | info[5];
    c64_u32 my = ((c64_u32)info[6] << 8) | info[7];
    if (mx > 100 && my > 100) {          /* trust it only if plausible */
        g_maxx = mx;
        g_maxy = my;
    }
    g_present = 1;
}

void c64_touch_poll(void)
{
    if (!g_present)
        return;
    c64_u8 off = 0x00, b[8];
    if (c64_i2c_xfer(C64_I2C_TP, TP_ADDR, &off, 1, b, 8) < 0)
        return;

    int status = b[0] & 0x07;            /* 1 = enter, 2 = moving */
    if (status != 1 && status != 2) {
        g_btn = 0;                       /* everything else = not down */
        c64_input_set_pointer(g_x, g_y, 0);
        return;
    }
    c64_u32 tx = ((c64_u32)b[1] << 4) | (b[3] >> 4);      /* 12-bit */
    c64_u32 ty = ((c64_u32)b[2] << 4) | (b[3] & 0x0F);
    if (tx > g_maxx || ty > g_maxy)
        return;
#ifdef C64_TOUCHDBG
    dbg_word(100, (tx << 16) | ty);
    dbg_word(140, (g_maxx << 16) | g_maxy);
#endif
    /* SCALE into panel pixels. The controller's range is whatever its
     * firmware reports, which is NOT guaranteed to be the panel's pixel
     * count -- the vendor's own fallback is 1080x1920 against a 1080x2160
     * panel, and the DTBO's stale tpd-resolution says the same. Treating raw
     * counts as pixels is what put the pointer in the wrong place. */
    tx = tx * PANEL_W / (g_maxx ? g_maxx : PANEL_W);
    ty = ty * PANEL_H / (g_maxy ? g_maxy : PANEL_H);

    /* Inverse of display.c's present: the UI rect is C64_DST_W x C64_DST_H at
     * (C64_DST_X0, C64_DST_Y0), each UI pixel an FB_SCALE block, rotated 270
     * (down the upright image is +x on the panel, right is -y). */
    int px = (int)tx - C64_DST_X0;
    int py = (int)ty - C64_DST_Y0;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= C64_DST_W) px = C64_DST_W - 1;
    if (py >= C64_DST_H) py = C64_DST_H - 1;
    int ux = (C64_SCRW - 1) - py / FB_SCALE;
    int uy = px / FB_SCALE;
    if (ux < 0) ux = 0;
    if (ux >= C64_SCRW) ux = C64_SCRW - 1;
    if (uy < 0) uy = 0;
    if (uy >= C64_SCRH) uy = C64_SCRH - 1;

    g_x = ux;
    g_y = uy;
    g_btn = 1;                           /* a contact IS the left button */
    c64_input_set_pointer(ux, uy, 1);
}
