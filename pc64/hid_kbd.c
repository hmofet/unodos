/* ===========================================================================
 * UnoDOS/pc64 - shared HID boot-keyboard report translator (see hid_kbd.h).
 * ======================================================================== */
#include "hid_kbd.h"

/* EFI SimpleTextIn scan codes (mirror uefi.h so this stays standalone) */
#define K_UP 1
#define K_DN 2
#define K_RT 3
#define K_LT 4
#define K_DEL 8
#define K_ESC 0x17

/* unoui's UI_MOD_* bits (unoui.h), mirrored the same way the scan codes above
 * mirror uefi.h - this file sits below the toolkit and includes neither. They
 * are the wire format hid_kbd_mods() answers in; keep them in step. */
#define HK_SHIFT 1
#define HK_CTRL  2
#define HK_ALT   4
#define HK_GUI   8

/* HID Keyboard/Keypad usage (0x07 page) -> ASCII, unshifted / shifted.
 * Index is the usage code; entries are 0 for keys handled as scan codes or
 * with no character.  Covers 0x00..0x38 (letters, digits, the symbol row);
 * arrows / esc / del / enter / bksp / tab are special-cased below. */
static const char kUnshift[0x39] = {
    0,0,0,0, 'a','b','c','d','e','f','g','h','i','j','k','l','m',      /* 0x04-0x10 */
    'n','o','p','q','r','s','t','u','v','w','x','y','z',              /* 0x11-0x1D */
    '1','2','3','4','5','6','7','8','9','0',                          /* 0x1E-0x27 */
    0,0,0,0, ' ',                                                     /* enter esc bksp tab space */
    '-','=','[',']','\\', 0, ';','\'','`',',','.','/'                  /* 0x2D-0x38 (0x32 non-US #) */
};
static const char kShift[0x39] = {
    0,0,0,0, 'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    0,0,0,0, ' ',
    '_','+','{','}','|', 0, ':','"','~','<','>','?'
};

void hid_kbd_reset(hid_kbd_state *s)
{
    int i;
    for (i = 0; i < 6; i++) s->prev[i] = 0;
    s->prevmod = 0; s->caps = 0; s->inited = 1;
}

/* the HID modifier byte -> UI_MOD_* bits: bit0 LCtrl, bit1 LShift, bit2 LAlt,
 * bit3 LGUI, bits 4-7 the right-hand four. Left and right fold together -
 * nothing above this cares which hand held Alt. */
static int fold_mods(unsigned char m)
{
    return ((m & 0x11) ? HK_CTRL  : 0) |
           ((m & 0x22) ? HK_SHIFT : 0) |
           ((m & 0x44) ? HK_ALT   : 0) |
           ((m & 0x88) ? HK_GUI   : 0);
}

static int was_down(const hid_kbd_state *s, unsigned char u)
{
    int i;
    for (i = 0; i < 6; i++) if (s->prev[i] == u) return 1;
    return 0;
}

/* translate one usage (already known to be a fresh press) into scan/uni and
 * emit it.  mod: HID modifier byte of this report. */
static void emit_usage(unsigned char u, unsigned char mod, hid_kbd_state *s,
                       hid_key_fn emit, void *ctx)
{
    int shift = (mod & 0x22) != 0;             /* L/R Shift */
    int ctrl  = (mod & 0x11) != 0;             /* L/R Ctrl  */
    int uni = 0, scan = 0;

    switch (u) {
    case 0x28: uni = '\r'; break;              /* Enter      */
    case 0x2A: uni = '\b'; break;              /* Backspace  */
    case 0x2B: uni = '\t'; break;              /* Tab        */
    case 0x29: scan = K_ESC; break;            /* Escape     */
    case 0x4C: scan = K_DEL; break;            /* Delete Fwd */
    case 0x4F: scan = K_RT;  break;            /* Right      */
    case 0x50: scan = K_LT;  break;            /* Left       */
    case 0x51: scan = K_DN;  break;            /* Down       */
    case 0x52: scan = K_UP;  break;            /* Up         */
    default:
        if (u < 0x39) {
            uni = shift ? kShift[u] : kUnshift[u];
            /* Caps Lock affects letters only, and inverts the shift choice */
            if (s->caps && u >= 0x04 && u <= 0x1D)
                uni = shift ? kUnshift[u] : kShift[u];
        }
        break;
    }
    /* mods is the mask held AT THE MOMENT OF THIS PRESS, taken from this
     * report's own modifier byte - not the live level. A binding like Alt+Tab
     * has to know Alt was down when Tab arrived, which a later poll cannot
     * answer once the key has been let go. */
    if (scan || uni) emit(scan, uni, ctrl, fold_mods(mod), ctx);
}

void hid_kbd_report(hid_kbd_state *s, const unsigned char *rep,
                    hid_key_fn emit, void *ctx)
{
    unsigned char mod = rep[0];
    int i;
    if (!s->inited) hid_kbd_reset(s);

    /* Record the modifier byte FIRST, and unconditionally. It is LEVEL state
     * (hid_kbd_mods() answers "held right now"), so it has to be current after
     * every report - including the rollover one below, which means too many
     * keys are down, not that the modifiers stopped being held. */
    s->prevmod = mod;

    /* 0x01 in keycode[0] = rollover error; ignore the whole report */
    if (rep[2] == 0x01) return;

    for (i = 2; i < 8; i++) {
        unsigned char u = rep[i];
        if (u == 0) continue;
        if (was_down(s, u)) continue;          /* still held - not an edge */
        if (u == 0x39) { s->caps = !s->caps; continue; }   /* Caps Lock toggle */
        emit_usage(u, mod, s, emit, ctx);
    }
    for (i = 0; i < 6; i++) s->prev[i] = rep[2 + i];
}

/* The HID modifier byte as UI_MOD_* bits: bit0 LCtrl, bit1 LShift, bit2 LAlt,
 * bit3 LGUI, bits 4-7 the right-hand four. Left and right fold together -
 * nothing above this cares which hand held Alt.
 *
 * LEVEL, not an edge: a modifier-only change IS a report (all six keycodes
 * zero), so this stays current while a modifier is held with no key pressed,
 * and a poll that brings no report leaves it alone rather than clearing it. */
int hid_kbd_mods(const hid_kbd_state *s) { return fold_mods(s->prevmod); }
