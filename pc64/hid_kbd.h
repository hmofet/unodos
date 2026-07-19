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
 * Ctrl modifier was held. */
typedef void (*hid_key_fn)(int scan, int uni, int ctrl, void *ctx);

void hid_kbd_reset(hid_kbd_state *s);

/* Feed one 8-byte boot keyboard report; `emit` fires once per newly-pressed
 * key.  `rep` must point to at least 8 bytes. */
void hid_kbd_report(hid_kbd_state *s, const unsigned char *rep,
                    hid_key_fn emit, void *ctx);

#endif
