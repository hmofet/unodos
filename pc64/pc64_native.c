/* ===========================================================================
 * UnoDOS/pc64 - native platform services (M3).
 *
 * Everything the OS needs from the machine once ExitBootServices has run and
 * the firmware is out of the picture:
 *
 *   - TSC delays        (replaces gBS->Stall; calibrated against Stall once,
 *                        while boot services are still live)
 *   - CMOS RTC          (replaces runtime GetTime/SetTime; ports 0x70/0x71)
 *   - PS/2 kbd + mouse  (replaces ConIn / Simple|Absolute Pointer; a polled
 *                        i8042 driver, scan-set 1 via the controller's
 *                        translation - QEMU q35 and classic PCs; laptops
 *                        without an i8042 simply never detach)
 *   - CF9 reset         (replaces ResetSystem for restart)
 *
 * Every wait is bounded - a missing or hostile controller times out instead
 * of hanging (the same rule the EC/ACPI stack follows).  All port I/O, no
 * firmware calls anywhere in this file.
 * ======================================================================== */

static inline void n_outb(unsigned short port, unsigned char v)
{ __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline unsigned char n_inb(unsigned short port)
{ unsigned char v; __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }

/* ---- TSC time base -------------------------------------------------------- */
static unsigned long long gTscPerUs;     /* cycles per microsecond (calibrated) */

unsigned long long uno_native_rdtsc(void)
{
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | lo;
}

void uno_native_tsc_set(unsigned long long cycles_per_us)
{ gTscPerUs = cycles_per_us ? cycles_per_us : 1000; }

int uno_native_tsc_ok(void) { return gTscPerUs != 0; }

unsigned long long uno_native_tsc_per_us(void) { return gTscPerUs; }

void uno_native_delay_us(unsigned long us)
{
    unsigned long long end;
    if (!gTscPerUs) gTscPerUs = 1000;            /* ~1 GHz guess, never hang */
    end = uno_native_rdtsc() + (unsigned long long)us * gTscPerUs;
    while (uno_native_rdtsc() < end)
        __asm__ volatile ("pause");
}

/* ---- CMOS RTC (ports 0x70/0x71) ------------------------------------------- */
static unsigned char cmos_read(unsigned char reg)
{
    n_outb(0x70, (unsigned char)(reg | 0x80));   /* NMI masked while we poke  */
    return n_inb(0x71);
}
static void cmos_write(unsigned char reg, unsigned char v)
{
    n_outb(0x70, (unsigned char)(reg | 0x80));
    n_outb(0x71, v);
}
static int bcd2bin(unsigned char v) { return (v >> 4) * 10 + (v & 0x0F); }
static unsigned char bin2bcd(int v) { return (unsigned char)(((v / 10) << 4) | (v % 10)); }

/* returns 1 on success; waits out an update-in-progress (bounded) */
int uno_native_rtc_read(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    int spin = 100000, bin, h24, yy, hh;
    unsigned char b;
    while ((cmos_read(0x0A) & 0x80) && spin-- > 0) { }   /* UIP clear         */
    if (spin <= 0) return 0;
    b   = cmos_read(0x0B);
    bin = b & 0x04;                            /* 1 = registers are binary    */
    h24 = b & 0x02;                            /* 1 = 24-hour mode            */
    hh  = cmos_read(0x04);
    if (!h24) {                                /* 12-hour: bit7 = PM          */
        int pm = hh & 0x80; hh &= 0x7F;
        hh = bin ? hh : bcd2bin((unsigned char)hh);
        if (hh == 12) hh = 0;
        if (pm) hh += 12;
    } else hh = bin ? hh : bcd2bin((unsigned char)hh);
    yy = bin ? cmos_read(0x09) : bcd2bin(cmos_read(0x09));
    if (y)  *y  = 2000 + yy;                   /* the family's century        */
    if (mo) *mo = bin ? cmos_read(0x08) : bcd2bin(cmos_read(0x08));
    if (d)  *d  = bin ? cmos_read(0x07) : bcd2bin(cmos_read(0x07));
    if (h)  *h  = hh;
    if (mi) *mi = bin ? cmos_read(0x02) : bcd2bin(cmos_read(0x02));
    if (s)  *s  = bin ? cmos_read(0x00) : bcd2bin(cmos_read(0x00));
    return 1;
}

int uno_native_rtc_write(int y, int mo, int d, int h, int mi, int s)
{
    unsigned char b = cmos_read(0x0B);
    int bin = b & 0x04;
    cmos_write(0x0B, (unsigned char)(b | 0x80));         /* SET: halt updates */
    if (bin) {
        cmos_write(0x00, (unsigned char)s);  cmos_write(0x02, (unsigned char)mi);
        cmos_write(0x04, (unsigned char)h);  cmos_write(0x07, (unsigned char)d);
        cmos_write(0x08, (unsigned char)mo); cmos_write(0x09, (unsigned char)(y % 100));
    } else {
        cmos_write(0x00, bin2bcd(s));  cmos_write(0x02, bin2bcd(mi));
        cmos_write(0x04, bin2bcd(h));  cmos_write(0x07, bin2bcd(d));
        cmos_write(0x08, bin2bcd(mo)); cmos_write(0x09, bin2bcd(y % 100));
    }
    cmos_write(0x0B, (unsigned char)((b & ~0x80) | 0x02));  /* 24h, run       */
    return 1;
}

/* ---- raw port I/O (kernel tier: unoscript io.* surface) ------------------- *
 * Byte/word/dword IN and OUT for a script's guarded io.in/io.out.  `width` is
 * in BYTES (1/2/4); an unsupported width reads 0 / writes nothing. */
unsigned uno_native_port_in(unsigned port, int width)
{
    unsigned short p = (unsigned short)port;
    if (width == 1) { unsigned char  v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
    if (width == 2) { unsigned short v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
    if (width == 4) { unsigned int   v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
    return 0;
}
void uno_native_port_out(unsigned port, int width, unsigned val)
{
    unsigned short p = (unsigned short)port;
    if (width == 1) { __asm__ volatile("outb %0,%1"::"a"((unsigned char)val), "Nd"(p)); }
    else if (width == 2) { __asm__ volatile("outw %0,%1"::"a"((unsigned short)val), "Nd"(p)); }
    else if (width == 4) { __asm__ volatile("outl %0,%1"::"a"((unsigned int)val), "Nd"(p)); }
}

/* ---- reset ----------------------------------------------------------------
 * Two entry points over one mechanism.  uno_native_reset_try() attempts the
 * PCH reset-control port and RETURNS if the board ignored it, so a caller can
 * fall through to another method; uno_native_reset() is the terminal form that
 * also pulses the i8042 and then halts.  The split exists because the single
 * power policy in uefi_main.c wants to order native and firmware resets
 * differently depending on whether the firmware is still there, and a reset
 * routine that never returns cannot be first in any such order. */
int uno_native_reset_try(void)
{
    n_outb(0xCF9, 0x02);                       /* arm                          */
    uno_native_delay_us(50);
    n_outb(0xCF9, 0x06);                       /* hard reset                   */
    uno_native_delay_us(200000);
    return 0;                                  /* still here: it did not take  */
}

/* The i8042 pulse on its own, non-terminal.  Same argument as the CF9 split
 * above: a caller that still has an ACPI reset register to try cannot use a
 * helper that halts. */
void uno_native_kbd_reset_pulse(void)
{
    n_outb(0x64, 0xFE);
    uno_native_delay_us(200000);
}

void uno_native_reset(void)
{
    uno_native_reset_try();
    uno_native_kbd_reset_pulse();              /* fallback: i8042 pulse        */
    for (;;) __asm__ volatile ("hlt");
}

/* ---- PS/2 (i8042), polled ------------------------------------------------- */
#define PS2_DATA 0x60
#define PS2_STAT 0x64
#define ST_OBF   0x01
#define ST_IBF   0x02
#define ST_AUX   0x20

static int gPs2Kbd, gPs2Aux;                   /* brought-up flags             */
static int gPs2Wheel;                          /* 4-byte packets with a wheel  */
static int gPs2AuxPort;                        /* 0xA9 aux-port self-test ok   */
static int gPs2AuxId = -1;                     /* 0xF2 device id, -1 = no answer */

/* bounded waits: ~a few ms each, far beyond any healthy controller */
static int ibf_clear(void)
{ int n = 50000; while ((n_inb(PS2_STAT) & ST_IBF) && n-- > 0) { } return n > 0; }
static int obf_set(void)
{ int n = 50000; while (!(n_inb(PS2_STAT) & ST_OBF) && n-- > 0) { } return n > 0; }

static void ctl_cmd(unsigned char c) { if (ibf_clear()) n_outb(PS2_STAT, c); }
static void dat_out(unsigned char c) { if (ibf_clear()) n_outb(PS2_DATA, c); }
static int  dat_in(void)             { return obf_set() ? n_inb(PS2_DATA) : -1; }

static int kbd_cmd(unsigned char c)            /* to the keyboard, expect ACK  */
{
    int r, tries = 3;
    while (tries-- > 0) {
        dat_out(c);
        r = dat_in();
        if (r == 0xFA) return 1;
        if (r != 0xFE) break;                  /* resend -> retry              */
    }
    return 0;
}
static int aux_cmd(unsigned char c)            /* to the mouse, expect ACK     */
{
    int r, tries = 3;
    while (tries-- > 0) {
        ctl_cmd(0xD4); dat_out(c);
        r = dat_in();
        if (r == 0xFA) return 1;
        if (r != 0xFE) break;
    }
    return 0;
}

int uno_ps2_present(void)                      /* passive - safe pre-detach    */
{ return n_inb(PS2_STAT) != 0xFF; }

/* Full bring-up. Call ONLY once detached - while boot services are live the
 * firmware owns the i8042 (same one-driver-per-controller rule as storage). */
int uno_ps2_init(void)
{
    int cfg, n;
    if (!uno_ps2_present()) return 0;
    for (n = 0; n < 32 && (n_inb(PS2_STAT) & ST_OBF); n++) n_inb(PS2_DATA);
    ctl_cmd(0x20); cfg = dat_in();             /* read config                  */
    if (cfg < 0) cfg = 0x45;
    cfg |= 0x40;                               /* scan-set-1 translation on    */
    cfg &= ~0x03;                              /* both IRQs off - we poll      */
    ctl_cmd(0x60); dat_out((unsigned char)cfg);
    ctl_cmd(0xAE);                             /* enable keyboard port         */
    gPs2Kbd = kbd_cmd(0xF4);                   /* enable scanning              */
    if (!gPs2Kbd && uno_ps2_present()) gPs2Kbd = 1;   /* controller yes, ack
                                        lost: poll anyway (QEMU always ACKs) */
    ctl_cmd(0xA8);                             /* enable aux port              */
    /* Aux self-test first: 0xA9 answers 0x00 only when a second port really
     * exists. Without it a machine with no aux device is indistinguishable
     * from one whose mouse merely lost an ACK, and the detach gate has no way
     * to know it is about to throw away the only working pointer. */
    ctl_cmd(0xA9);
    gPs2AuxPort = (dat_in() == 0x00);
    aux_cmd(0xF6);                             /* mouse defaults               */
    /* identify: 0x00 = plain 3-byte mouse, 0x03 = wheel, 0x04 = 5-button.
     * Recorded for the System readout - on a ThinkPad the TrackPoint answers
     * here even when the touchpad is off on I2C entirely. */
    if (aux_cmd(0xF2)) { int id = dat_in(); gPs2AuxId = (id >= 0) ? id : -1; }
    /* the Microsoft IntelliMouse knock: sample rate 200, 100, 80 makes a wheel
     * mouse switch to device id 3 and 4-byte packets. A mouse without a wheel
     * keeps answering 0, so this is safe to try on anything. */
    if (aux_cmd(0xF3) && aux_cmd(200) &&       /* each byte goes through 0xD4  */
        aux_cmd(0xF3) && aux_cmd(100) &&
        aux_cmd(0xF3) && aux_cmd(80)  && aux_cmd(0xF2)) {
        int id = dat_in();
        if (id >= 0) { gPs2AuxId = id; gPs2Wheel = (id == 3 || id == 4); }
    }
    if (aux_cmd(0xF3)) aux_cmd(100);           /* back to a sane sample rate   */
    gPs2Aux = aux_cmd(0xF4);                   /* stream reporting on          */
    return gPs2Kbd;
}

/* what actually bound, for the System window (there was previously no way to
 * tell an empty aux port from a mouse that simply never ACKed) */
void uno_ps2_status(int *kbd, int *aux, int *auxport, int *auxid)
{
    if (kbd)     *kbd     = gPs2Kbd;
    if (aux)     *aux     = gPs2Aux;
    if (auxport) *auxport = gPs2AuxPort;
    if (auxid)   *auxid   = gPs2AuxId;
}

/* ---- scan-set-1 decode -> the (EFI-style scan, unicode) space map_key eats */
static const char kSet1[128] = {
      0,   0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',   8,   9,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',  13,   0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';','\'', '`',   0,'\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/',   0, '*',   0, ' ',   0,   0,   0,   0,   0,   0,
      0,   0,   0,   0,   0,   0,   0, '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.',   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
};
static const char kSet1Sh[128] = {
      0,   0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+',   8,   9,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}',  13,   0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',   0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?',   0, '*',   0, ' ',   0,   0,   0,   0,   0,   0,
};

/* EFI scan codes (uefi.h): UP 1 DOWN 2 RIGHT 3 LEFT 4 DELETE 8 F10 0x14 ESC 0x17 */
#define K_UP 1
#define K_DN 2
#define K_RT 3
#define K_LT 4
#define K_DEL 8
#define K_F1  0x0B                             /* F1..F12 are contiguous       */
#define K_F10 0x14
#define K_ESC 0x17

#define KQ 16
static struct { int scan, uni, mods; } gKQ[KQ];
static int gKQh, gKQt;
static int gShift, gCtrl, gCaps, gE0;
static int gAlt, gGui;                         /* make/break tracked, like shift */

/* mirrors unoui.h's UI_MOD_* - this file includes no headers by design (it is
 * the no-firmware layer), so the four bits are restated rather than pulled in.
 * They are a wire format between here and uefi_main's raw ring; keep in step. */
#define PS2_MOD_SHIFT 1
#define PS2_MOD_CTRL  2
#define PS2_MOD_ALT   4
#define PS2_MOD_GUI   8

/* live modifier state, UI_MOD_* bits. Authoritative while native input owns
 * the keyboard: every modifier here has a real make AND break edge, so
 * Alt-held reads true for exactly as long as Alt is physically down. */
static int ps2_mods(void)
{
    return (gShift ? PS2_MOD_SHIFT : 0) | (gCtrl ? PS2_MOD_CTRL : 0) |
           (gAlt   ? PS2_MOD_ALT   : 0) | (gGui  ? PS2_MOD_GUI  : 0);
}
int uno_ps2_mods(void) { return ps2_mods(); }

static void kq_push(int scan, int uni, int mods)
{
    int n = (gKQt + 1) % KQ;
    if (n == gKQh) return;
    gKQ[gKQt].scan = scan; gKQ[gKQt].uni = uni; gKQ[gKQt].mods = mods;
    gKQt = n;
}

static void kbd_byte(unsigned char b)
{
    int brk, code, uni, md;
    if (b == 0xE0) { gE0 = 1; return; }
    if (b == 0xE1) { return; }                 /* pause prefix - ignore        */
    brk  = b & 0x80;
    code = b & 0x7F;
    if (gE0) {
        gE0 = 0;
        switch (code) {
        case 0x1D: gCtrl = !brk; return;       /* right ctrl                   */
        case 0x38: gAlt  = !brk; return;       /* AltGr / right alt            */
        case 0x5B: case 0x5C: gGui = !brk; return;   /* left/right logo        */
        }
        if (brk) return;
        md = ps2_mods();
        switch (code) {
        case 0x48: kq_push(K_UP,  0, md); break;
        case 0x50: kq_push(K_DN,  0, md); break;
        case 0x4B: kq_push(K_LT,  0, md); break;
        case 0x4D: kq_push(K_RT,  0, md); break;
        case 0x53: kq_push(K_DEL, 0, md); break;
        }
        return;
    }
    switch (code) {
    case 0x2A: case 0x36: gShift = !brk; return;
    case 0x1D:            gCtrl  = !brk; return;
    case 0x38:            gAlt   = !brk; return;
    case 0x3A: if (!brk)  gCaps  = !gCaps; return;
    case 0x01: if (!brk)  kq_push(K_ESC, 0, ps2_mods()); return;
    }
    /* function keys: set-1 F1..F10 = 0x3B..0x44, F11/F12 = 0x57/0x58, and the
     * EFI scan codes they map to are contiguous from F1 = 0x0B. The shell's
     * F2 window-switcher fallback is unreachable on a detached boot without
     * this, which is exactly the boot where Alt may be the only other route. */
    if (code >= 0x3B && code <= 0x44) { if (!brk) kq_push(K_F1 + (code - 0x3B), 0, ps2_mods()); return; }
    if (code == 0x57 || code == 0x58) { if (!brk) kq_push(K_F1 + 10 + (code - 0x57), 0, ps2_mods()); return; }
    if (brk || code >= 128) return;
    uni = gShift ? kSet1Sh[code] : kSet1[code];
    if (!uni) return;
    if (gCaps && uni >= 'a' && uni <= 'z' && !gShift) uni += 'A' - 'a';
    else if (gCaps && uni >= 'A' && uni <= 'Z' && gShift) uni += 'a' - 'A';
    kq_push(0, uni, ps2_mods());
}

/* ---- mouse packet assembly ------------------------------------------------ *
 * A wheel mouse (device id 3, see the knock in uno_ps2_init) sends FOUR bytes:
 * the fourth is a signed wheel delta, positive when the wheel is rolled toward
 * the user. gPs2Wheel picks the packet length, so a plain 3-byte mouse is
 * unaffected. */
static unsigned char gMPk[4];
static int gMPn, gMdx, gMdy, gMdz, gMBtn, gMNew;

static void aux_byte(unsigned char b)
{
    int plen = gPs2Wheel ? 4 : 3;
    if (gMPn == 0 && !(b & 0x08)) return;      /* resync on the sync bit       */
    gMPk[gMPn++] = b;
    if (gMPn < plen) return;
    gMPn = 0;
    if (gMPk[0] & 0xC0) return;                /* overflow - drop              */
    if (gPs2Wheel) {
        int z = (int)(signed char)((gMPk[3] & 0x08) ? (gMPk[3] | 0xF0)
                                                    : (gMPk[3] & 0x0F));
        gMdz += z;                             /* + = toward the user (down)   */
    }
    gMdx += (int)gMPk[1] - ((gMPk[0] & 0x10) ? 256 : 0);
    gMdy -= (int)gMPk[2] - ((gMPk[0] & 0x20) ? 256 : 0);   /* PS/2 y is up    */
    /* Keep the buttons SEPARATE (bit0 left, bit1 right, bit2 middle) rather
     * than collapsing them to "something is pressed" - the shell needs to tell
     * a right-click from a left one to open the launcher on the desktop. */
    gMBtn = gMPk[0] & 0x07;
    gMNew = 1;
}

/* drain the controller: route bytes by the AUX status bit (bounded) */
void uno_ps2_pump(void)
{
    int budget = 32;
    if (!gPs2Kbd && !gPs2Aux) return;
    while (budget-- > 0) {
        unsigned char st = n_inb(PS2_STAT);
        if (!(st & ST_OBF)) break;
        if (st & ST_AUX) aux_byte(n_inb(PS2_DATA));
        else             kbd_byte(n_inb(PS2_DATA));
    }
}

int uno_ps2_next_key2(int *scan, int *uni, int *mods)
{
    if (gKQh == gKQt) return 0;
    *scan = gKQ[gKQh].scan; *uni = gKQ[gKQh].uni; *mods = gKQ[gKQh].mods;
    gKQh = (gKQh + 1) % KQ;
    return 1;
}

/* compat wrapper: the ctrl-only stream every pre-modifier caller reads */
int uno_ps2_next_key(int *scan, int *uni, int *ctrl)
{
    int md;
    if (!uno_ps2_next_key2(scan, uni, &md)) return 0;
    *ctrl = (md & PS2_MOD_CTRL) != 0;
    return 1;
}

/* accumulated relative motion + latched button since the last call */
int uno_ps2_mouse(int *dx, int *dy, int *btn)
{
    int had = gMNew;
    *dx = gMdx; *dy = gMdy; *btn = gMBtn;
    gMdx = gMdy = 0; gMNew = 0;
    return had;
}

/* accumulated wheel notches since the last call (+ = scroll down); 0 on a
 * mouse with no wheel, which never sets gPs2Wheel */
int uno_ps2_mouse_wheel(void)
{ int z = gMdz; gMdz = 0; return z; }
