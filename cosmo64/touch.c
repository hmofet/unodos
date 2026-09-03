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
static int g_x = C64_SCRW / 2, g_y = C64_SCRH / 2, g_btn;  /* starting size */
static c64_u32 g_maxx = 1080, g_maxy = 2160;

/* The last report as the controller gave it, before any mapping. calib.c
 * works entirely in these numbers, which is the point: the calibration path
 * must not contain the transform it is trying to measure. */
static c64_u32 g_raw_x, g_raw_y;
static int g_raw_down;

int c64_touch_raw(int *x, int *y)
{
    *x = (int)g_raw_x;
    *y = (int)g_raw_y;
    return g_raw_down;
}

void c64_touch_maxima(int *mx, int *my)
{
    *mx = (int)g_maxx;
    *my = (int)g_maxy;
}

int c64_touch_present(void)
{
    return g_present;
}

#ifdef C64_TOUCHDBG
/* Paint a 32-bit value as bit-cells straight onto the panel, in the black
 * band ABOVE the UI rect (panel y < c64_dst_y0) so the shell's present never
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
    if (c64_i2c_init(C64_I2C_TP) < 0) {
        c64_log("touch: i2c bus 0 would not init\n");
        return;
    }
    if (set_page(EVENT_BUF) < 0) {
        /* Bus 0 now runs at 400 kHz, which this controller supports. If the
         * page select does not take there, fall back to Standard mode and try
         * once more -- a pointer that degrades beats one that vanishes. */
        if (c64_i2c_khz(C64_I2C_TP) > 100) {
            c64_logf("touch: no answer at %d kHz; retrying in Standard mode\n",
                     c64_i2c_khz(C64_I2C_TP));
            c64_i2c_set_khz(C64_I2C_TP, 100);
        }
        if (set_page(EVENT_BUF) < 0) {
            c64_log("touch: xdata page select NAKed\n");
            return;
        }
    }
    /* the FW info block: ver, ~ver, then x_num/y_num and the maxima. This is
     * a read-only presence test -- unlike the vendor's chip-ID trim check,
     * which bootloader-resets the part and halts its MCU. */
    c64_u8 off = OFF_FWINFO, info[8];
    if (c64_i2c_xfer(C64_I2C_TP, TP_ADDR, &off, 1, info, 8) < 0) {
        c64_log("touch: FW-info read failed\n");
        return;
    }
    if (((info[0] + info[1]) & 0xFF) != 0xFF) {
        c64_logf("touch: no NT36xxx answering (ver=%02x ~ver=%02x)\n",
                 info[0], info[1]);
        return;                          /* not an answering NT36xxx */
    }
    c64_u32 mx = ((c64_u32)info[4] << 8) | info[5];
    c64_u32 my = ((c64_u32)info[6] << 8) | info[7];
    if (mx > 100 && my > 100) {          /* trust it only if plausible */
        g_maxx = mx;
        g_maxy = my;
    }
    g_present = 1;
    c64_logf("touch: NT36xxx ver=%02x, reported maxima %dx%d, bus %d kHz\n",
             info[0], (int)g_maxx, (int)g_maxy, c64_i2c_khz(C64_I2C_TP));
}

void c64_touch_poll(void)
{
    if (!g_present)
        return;
    c64_u8 off = 0x00, b[8];
    if (c64_i2c_xfer(C64_I2C_TP, TP_ADDR, &off, 1, b, 8) < 0)
        return;

    static int was_down;
    int status = b[0] & 0x07;            /* 1 = enter, 2 = moving */
    if (status != 1 && status != 2) {
        g_btn = 0;                       /* everything else = not down */
        g_raw_down = 0;
        /* Report the release ONCE, on the edge. This used to publish the
         * panel's idle position every poll, which is fine with one pointer
         * and wrong with two: a USB mouse moved the cursor and the very next
         * poll snapped it back to wherever the last finger had been -- the
         * centre, on a boot with no touch at all. An absolute pointer speaks
         * when it has a contact to report, and stays silent otherwise. */
        if (was_down)
            c64_input_set_pointer(g_x, g_y, 0);
        was_down = 0;
        return;
    }
    c64_u32 tx = ((c64_u32)b[1] << 4) | (b[3] >> 4);      /* 12-bit */
    c64_u32 ty = ((c64_u32)b[2] << 4) | (b[3] & 0x0F);
    c64_u32 rawx = tx, rawy = ty;
    g_raw_x = rawx;
    g_raw_y = rawy;
    g_raw_down = 1;
    if (tx > g_maxx || ty > g_maxy) {
        if (!was_down)
            c64_logf("touch: report out of range: %d,%d vs max %d,%d\n",
                     (int)tx, (int)ty, (int)g_maxx, (int)g_maxy);
        was_down = 1;
        return;
    }
#ifdef C64_TOUCHDBG
    dbg_word(100, (tx << 16) | ty);
    dbg_word(140, (g_maxx << 16) | g_maxy);
#endif
    /* INVERT BOTH AXES: the controller's origin is the opposite corner from
     * the panel's. Measured, not guessed -- calib.c (./build.sh calib) drew
     * five crosshairs in raw panel pixels, a finger was put on each, and the
     * controller's reports came back mirrored in both axes:
     *
     *   target(panel)   raw report    1080-x, 2160-y    residual
     *      216, 432      829,1750       251, 410         +35,-22
     *      864, 432      220,1733       860, 427          -4, -5
     *      540,1080      487,1118       593,1042         +53,-38
     *      216,1728      809, 443       271,1717         +55,-11
     *      864,1728      264, 455       816,1705         -48,-23
     *
     * which takes the error from as much as 1318 px down to 55, the rest
     * being where a fingertip actually lands versus where it is aimed (one
     * capture moved 28 px between the last poll and the keypress). No scale
     * or offset is fitted on top: five hand-placed points cannot tell a real
     * gain error from the aiming bias of the hand that placed them. */
    tx = tx < g_maxx ? g_maxx - tx : 0;
    ty = ty < g_maxy ? g_maxy - ty : 0;

    /* SCALE into panel pixels. The controller's range is whatever its
     * firmware reports, which is NOT guaranteed to be the panel's pixel
     * count -- the vendor's own fallback is 1080x1920 against a 1080x2160
     * panel, and the DTBO's stale tpd-resolution says the same. Treating raw
     * counts as pixels is what put the pointer in the wrong place. */
    tx = tx * PANEL_W / (g_maxx ? g_maxx : PANEL_W);
    ty = ty * PANEL_H / (g_maxy ? g_maxy : PANEL_H);

    /* Inverse of display.c's present: the UI rect is c64_dst_w x c64_dst_h at
     * (c64_dst_x0, c64_dst_y0), each UI pixel a c64_scale block, rotated 270
     * (down the upright image is +x on the panel, right is -y). Runtime, not
     * the C64_DST_* macros: Control Panel > Display changes the desktop size
     * while this driver is running, and a mapping compiled against the
     * starting size would put the pointer somewhere else entirely. */
    int px = (int)tx - c64_dst_x0;
    int py = (int)ty - c64_dst_y0;
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= c64_dst_w) px = c64_dst_w - 1;
    if (py >= c64_dst_h) py = c64_dst_h - 1;
    int ux = (c64_scrw - 1) - py / c64_scale;
    int uy = px / c64_scale;
    if (ux < 0) ux = 0;
    if (ux >= c64_scrw) ux = c64_scrw - 1;
    if (uy < 0) uy = 0;
    if (uy >= c64_scrh) uy = c64_scrh - 1;

    /* One line per contact, not per frame: this is the whole "does the
     * pointer land where you touch?" question, answered in text instead of
     * bit-cells decoded from a photograph. */
    if (!was_down)
        c64_logf("touch: down raw=%d,%d (max %d,%d) -> panel %d,%d -> ui %d,%d\n",
                 (int)rawx, (int)rawy, (int)g_maxx, (int)g_maxy,
                 (int)tx, (int)ty, ux, uy);
    was_down = 1;

    g_x = ux;
    g_y = uy;
    g_btn = 1;                           /* a contact IS the left button */
    c64_input_set_pointer(ux, uy, 1);
}
