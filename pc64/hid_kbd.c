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
    if (scan || uni) emit(scan, uni, ctrl, ctx);
}

void hid_kbd_report(hid_kbd_state *s, const unsigned char *rep,
                    hid_key_fn emit, void *ctx)
{
    unsigned char mod = rep[0];
    int i;
    if (!s->inited) hid_kbd_reset(s);

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
    s->prevmod = mod;
}
