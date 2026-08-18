/* ===========================================================================
 * UnoDOS/pc64 - shared HID boot-keyboard report translator.
 *
 * USB HID and I2C-HID keyboards both deliver the SAME 8-byte boot-protocol
 * report: [modifiers][reserved][keycode0..5], keycodes being HID Usage-Table
 * Keyboard-page (0x07) usages.  This translates a stream of those reports into
 * key-DOWN edge events in the EFI SimpleTextIn (scan, unicode) space the pc64
 * input ring already speaks - so both transports feed uefi_main's map_key()
 * unchanged.  Stateful: it diffs each report against the previous to emit only
 * newly-pressed keys, and tracks Caps Lock.
 * ======================================================================== */
#ifndef PC64_HID_KBD_H
#define PC64_HID_KBD_H

typedef struct {
    unsigned char prev[6];      /* keycodes seen in the last report */
    unsigned char prevmod;      /* modifier byte of the last report */
    int           caps;         /* Caps Lock toggle state           */
    int           inited;
} hid_kbd_state;

/* One translated key-down event.  `scan` is an EFI SimpleTextIn scan code
 * (SCAN_UP=1/DOWN=2/RIGHT=3/LEFT=4/DELETE=8/ESC=0x17), 0 for a character key;
 * `uni` is the ASCII/Unicode char (0 for a scan-only key); `ctrl` is 1 if a
 * Ctrl modifier was held; `mods` is the FULL modifier mask held when the key
 * went down, as unoui UI_MOD_* bits (SHIFT 1, CTRL 2, ALT 4, GUI 8).
 *
 * `ctrl` is redundant - it is exactly !!(mods & UI_MOD_CTRL) - and is kept so
 * that a caller which only ever wanted Ctrl reads the same value it always
 * has.  `mods` was ADDED rather than replacing it deliberately: repurposing
 * the third argument would have compiled everywhere and quietly turned "Shift
 * is down" into "Ctrl is down", where a new argument breaks the build at every
 * typed call site instead. */
typedef void (*hid_key_fn)(int scan, int uni, int ctrl, int mods, void *ctx);

void hid_kbd_reset(hid_kbd_state *s);

/* Feed one 8-byte boot keyboard report; `emit` fires once per newly-pressed
 * key.  `rep` must point to at least 8 bytes. */
void hid_kbd_report(hid_kbd_state *s, const unsigned char *rep,
                    hid_key_fn emit, void *ctx);

/* LIVE (held-now) modifier state from the last report, as unoui UI_MOD_* bits
 * (SHIFT 1, CTRL 2, ALT 4, GUI 8), left and right folded together.  The keys
 * above are edges; this is a LEVEL, so it persists across polls that bring no
 * report - a boot keyboard reports on change, and a modifier held with nothing
 * else pressed reports once and then stays silent. */
int  hid_kbd_mods(const hid_kbd_state *s);

/* Currently-HELD navigation/action keys as a bitmask, read from the same
 * latched report hid_kbd_mods() reads.  These bits are the wire format
 * uno_pc64_keys_held() (uefi_main.c) answers in and PYRT's uno.keys_down()
 * hands to Python; the PS/2 tracker in pc64_native.c speaks it too.  A LEVEL,
 * like hid_kbd_mods() - it lasts exactly as long as the key is down. */
#define UNO_KH_UP     0x001
#define UNO_KH_DOWN   0x002
#define UNO_KH_RIGHT  0x004
#define UNO_KH_LEFT   0x008
#define UNO_KH_FIRE   0x010        /* F, or either Ctrl */
#define UNO_KH_USE    0x020        /* Space or E */
#define UNO_KH_SLEFT  0x040        /* comma */
#define UNO_KH_SRIGHT 0x080        /* period */
int  hid_kbd_keys_held(const hid_kbd_state *s);

#endif
