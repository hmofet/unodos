/* cosmo64/kbd.c -- the Cosmo's AW9523 matrix keyboard, polled.
 *
 * Hardware (device-verified + vendor aw9523_key.c in hmofet/cosmo analysis/):
 * an AW9523 GPIO expander at I2C bus 4 (controller 0x11008000), address 0x5b,
 * driving a 7-column (P1_0..P1_6) x 8-row (P0_0..P0_7) matrix, active low.
 * Scan, per the vendor driver: for each column, configure ONLY that P1 bit as
 * an output driven low (others input/floating), read P0 -- pressed keys in
 * that column read 0. The Fn layer is OURS: the chip has no Fn plane, Planet
 * reported KEY_FN raw and did the layer in userspace, so this driver owns a
 * four-plane keymap (base / Shift / Fn / Shift+Fn). The chassis-printed Fn
 * legends are honoured where known (/ on Y, = on I, ; on L, \ on 3, [ ] on
 * 7 8, - on Shift+comma); the rest are our assignments. The physical Esc key
 * is the PMIC power button (NOT in the matrix), so the "~" position -- the
 * matrix's top-left -- is Esc here, with ` and ~ on its Fn plane.
 *
 * Emission contract = hid_kbd.c's: printables as uni (Shift applied HERE),
 * specials as EFI scan codes, mods = UI_MOD_* mask at press time, plus the
 * UNO_KH_* live-held bits for games. Idle fast path: all columns driven low
 * at once, one P0 read -- 0xFF means nothing is down and the sweep is skipped.
 */

#include "cosmo64.h"

#define AW_ADDR 0x5B
#define R_P0_IN 0x00
#define R_P1_IN 0x01
#define R_P0_OUT 0x02
#define R_P1_OUT 0x03
#define R_P0_CFG 0x04
#define R_P1_CFG 0x05
#define R_P0_INT 0x06
#define R_P1_INT 0x07
#define R_ID 0x10
#define R_P0_LEDMODE 0x12
#define R_P1_LEDMODE 0x13
#define R_SW_RSTN 0x7F
#define AW_ID 0x23
#define NCOL 7
#define NROW 8
#define COLMASK 0x7F

/* keymap cells: 0 = nothing; 0x01xx = EFI scan code xx; 0x02xx = modifier
 * bit xx (UI_MOD_*: shift 1, ctrl 2, alt 4); 0x0300 = the Fn key; else a
 * unicode character. Planes: base, shift, fn, fn+shift. */
#define SC(x) (0x0100 | (x))
#define MOD(x) (0x0200 | (x))
#define FNK 0x0300
#define K_UP_ SC(0x01)
#define K_DN_ SC(0x02)
#define K_RT_ SC(0x03)
#define K_LT_ SC(0x04)
#define K_DEL_ SC(0x08)
#define K_F9_ SC(0x13)
#define K_F10_ SC(0x14)
#define K_ESC_ SC(0x17)

struct keydef { unsigned short p[4]; };

/* [col][row], rows P0_0..P0_7 -- the vendor key_map, re-planed. */
static const struct keydef map[NCOL][NROW] = {
/* col 0 */ {
    {{'1', '!', 0, 0}}, {{'u', 'U', '_', '_'}}, {{'s', 'S', 0, 0}},
    {{'z', 'Z', 0, 0}}, {{',', '-', '<', '_'}},
    {{K_ESC_, K_ESC_, '`', '~'}},                /* chassis "~": Esc here   */
    {{'8', '(', ']', '}'}}, {{'j', 'J', 0, 0}} },
/* col 1 */ {
    {{'2', '"', '@', '@'}}, {{'w', 'W', 0, 0}}, {{'d', 'D', 0, 0}},
    {{'c', 'C', 0, 0}}, {{MOD(4), MOD(4), MOD(4), MOD(4)}},   /* Alt   */
    {{K_LT_, K_LT_, K_LT_, K_LT_}},
    {{'9', ')', '*', '*'}}, {{'k', 'K', 0, 0}} },
/* col 2 */ {
    {{'3', '#', '\\', '|'}}, {{'y', 'Y', '/', '?'}}, {{'\t', '\t', '\t', '\t'}},
    {{'n', 'N', 0, 0}}, {{'m', 'M', 0, 0}},
    {{K_DN_, K_DN_, K_DN_, K_DN_}},
    {{'\b', '\b', K_DEL_, K_DEL_}},              /* Fn+Del = forward delete */
    {{'i', 'I', '=', '+'}} },
/* col 3 */ {
    {{'4', '$', 0, 0}}, {{'t', 'T', 0, 0}}, {{'f', 'F', 0, 0}},
    {{'x', 'X', 0, 0}}, {{FNK, FNK, FNK, FNK}},               /* Fn    */
    {{MOD(1), MOD(1), MOD(1), MOD(1)}},                       /* RShift*/
    {{'p', 'P', 0, 0}}, {{0, 0, 0, 0}} },
/* col 4 */ {
    {{'5', '%', 0, 0}}, {{'e', 'E', 0, 0}}, {{'g', 'G', 0, 0}},
    {{'v', 'V', 0, 0}}, {{' ', ' ', ' ', ' '}},
    {{K_UP_, K_UP_, K_UP_, K_UP_}},
    {{'o', 'O', 0, 0}}, {{0, 0, 0, 0}} },
/* col 5 */ {
    {{'6', '&', '^', '^'}}, {{'q', 'Q', 0, 0}}, {{'a', 'A', 0, 0}},
    {{'b', 'B', 0, 0}}, {{'.', '?', '>', '>'}},
    {{K_RT_, K_RT_, K_RT_, K_RT_}},
    {{'\r', '\r', '\r', '\r'}}, {{0, 0, 0, 0}} },
/* col 6 */ {
    {{'7', '\'', '[', '{'}}, {{'r', 'R', 0, 0}}, {{'h', 'H', 0, 0}},
    {{MOD(1), MOD(1), MOD(1), MOD(1)}},                       /* LShift*/
    {{MOD(2), MOD(2), MOD(2), MOD(2)}},                       /* Ctrl  */
    {{'l', 'L', ';', ':'}},
    {{'0', '0', 0, 0}}, {{0, 0, 0, 0}} },
};

static int g_present;
static c64_u8 g_state[NCOL];       /* 1 bits = pressed (inverted P0 reads) */
static int g_mods, g_fn;

int c64_kbd_present(void)
{
    return g_present;
}

void c64_kbd_init(void)
{
    if (c64_i2c_init() < 0)
        return;
    int id = c64_i2c_read_reg(AW_ADDR, R_ID);
    if (id != AW_ID)
        return;                             /* absent (or QEMU): stay silent */
    c64_i2c_write_reg(AW_ADDR, R_SW_RSTN, 0x00);
    c64_i2c_write_reg(AW_ADDR, R_P0_LEDMODE, 0xFF);   /* GPIO, not LED mode */
    c64_i2c_write_reg(AW_ADDR, R_P1_LEDMODE, 0xFF);
    c64_i2c_write_reg(AW_ADDR, R_P0_CFG, 0xFF);       /* P0: inputs (rows)  */
    c64_i2c_write_reg(AW_ADDR, R_P1_CFG, 0x00);       /* P1: outputs (cols) */
    c64_i2c_write_reg(AW_ADDR, R_P1_OUT, 0x00);       /* all columns low    */
    c64_i2c_write_reg(AW_ADDR, R_P0_INT, 0xFF);       /* polled: no irqs    */
    c64_i2c_write_reg(AW_ADDR, R_P1_INT, 0xFF);
    g_present = 1;
}

static void emit(const struct keydef *k, int shift, int fn)
{
    unsigned short v = k->p[(fn ? 2 : 0) | (shift ? 1 : 0)];
    if (!v && fn)
        v = k->p[shift ? 1 : 0];            /* no Fn plane: fall through */
    if (!v || (v & 0xFF00) == 0x0200 || v == FNK)
        return;                             /* modifiers are level, not edges */
    int scan = 0, uni = 0;
    if ((v & 0xFF00) == 0x0100)
        scan = v & 0xFF;
    else
        uni = v;
    c64_key_push(scan, uni, g_mods);
}

/* live level for uno_pc64_mods()/keys_held(): modifiers + the game bits */
static void publish_level(void)
{
    int mods = 0, held = 0, c, r;
    for (c = 0; c < NCOL; c++) {
        for (r = 0; r < NROW; r++) {
            if (!(g_state[c] & (1u << r)))
                continue;
            unsigned short v = map[c][r].p[0];
            if ((v & 0xFF00) == 0x0200) {
                mods |= v & 0xFF;
                if (v & 2)
                    held |= 0x010;          /* Ctrl doubles as FIRE */
                continue;
            }
            switch (v) {                    /* UNO_KH_* (hid_kbd.h) */
            case K_UP_: held |= 0x001; break;
            case K_DN_: held |= 0x002; break;
            case K_RT_: held |= 0x004; break;
            case K_LT_: held |= 0x008; break;
            case 'f': held |= 0x010; break;
            case ' ': case 'e': held |= 0x020; break;
            case ',': held |= 0x040; break;
            case '.': held |= 0x080; break;
            }
        }
    }
    g_fn = (g_state[3] & (1u << 4)) != 0;   /* Fn = col 3, row 4 (map[3][4]) */
    g_mods = mods;
    c64_input_set_level(mods, held);
}

void c64_kbd_poll(void)
{
    if (!g_present)
        return;
    /* idle fast path: every column low at once, one read */
    c64_i2c_write_reg(AW_ADDR, R_P1_CFG, 0x00);
    c64_i2c_write_reg(AW_ADDR, R_P1_OUT, 0x00);
    int all = c64_i2c_read_reg(AW_ADDR, R_P0_IN);
    if (all < 0)
        return;
    int any_now = ((all & 0xFF) != 0xFF);
    int any_before = 0;
    for (int c = 0; c < NCOL; c++)
        any_before |= g_state[c];
    if (!any_now && !any_before)
        return;

    /* full sweep: one column at a time */
    c64_u8 newstate[NCOL];
    for (int c = 0; c < NCOL; c++) {
        c64_i2c_write_reg(AW_ADDR, R_P1_CFG, (c64_u8)(COLMASK & ~(1u << c)));
        c64_i2c_write_reg(AW_ADDR, R_P1_OUT, (c64_u8)(COLMASK & ~(1u << c)));
        int v = c64_i2c_read_reg(AW_ADDR, R_P0_IN);
        newstate[c] = (v < 0) ? 0 : (c64_u8)(~v & 0xFF);
    }
    /* park: all columns output low again (the idle/fast-path state) */
    c64_i2c_write_reg(AW_ADDR, R_P1_CFG, 0x00);
    c64_i2c_write_reg(AW_ADDR, R_P1_OUT, 0x00);

    /* level first (so a chord's modifier is live before its key's edge) */
    c64_u8 old[NCOL];
    for (int c = 0; c < NCOL; c++) {
        old[c] = g_state[c];
        g_state[c] = newstate[c];
    }
    publish_level();

    int shift = (g_mods & 1) != 0;
    for (int c = 0; c < NCOL; c++) {
        c64_u8 pressed = (c64_u8)(newstate[c] & ~old[c]);
        for (int r = 0; pressed && r < NROW; r++)
            if (pressed & (1u << r))
                emit(&map[c][r], shift, g_fn);
    }
}
