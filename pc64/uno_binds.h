/* ===========================================================================
 * UnoDOS/pc64 - key bindings and small app preferences.
 *
 * WHY THIS EXISTS
 * Duum's pause menu has a Controls screen and an FPS toggle, and asks the host
 * for both through five OPTIONAL calls (probed with hasattr, so a port without
 * them still plays the whole game and the menu says the platform cannot
 * remap).  This is that port.  See pc64/DUUM-UPSTREAM.md.
 *
 * WHAT A "KEY" IS HERE
 * Two different parts of this machine produce the held-key bitmap from two
 * different key spaces: USB/I2C HID reports Usage-Table codes, the PS/2
 * tracker reports Set-1 scancodes.  Neither is portable and neither is what a
 * player would call the key.  So a binding is stored against a KEY ID:
 *
 *   a character key   its unshifted ASCII, lower case  ('w', ',', ' ')
 *   everything else   UNO_BK_* below                   (arrows, Ctrl)
 *
 * Each producer translates its own space into that, then asks uno_bind_bits()
 * what the key does now.  Adding a third keyboard transport means writing one
 * translation, not another copy of the bindings.
 *
 * WHAT IS DELIBERATELY NOT HERE
 * The DEFAULTS are exactly the keys this machine has always used - arrows, F,
 * Space, E, comma, period, either Ctrl - and no more.  It would have been easy
 * to add WASD while passing, and that would have been a silent change to how
 * every existing app reads the keyboard, hidden inside a commit about a menu.
 * ======================================================================== */
#ifndef PC64_UNO_BINDS_H
#define PC64_UNO_BINDS_H

/* Key ids for the keys that have no character.  Above 0xFF so they cannot
 * collide with an ASCII key id. */
#define UNO_BK_UP     0x101
#define UNO_BK_DOWN   0x102
#define UNO_BK_RIGHT  0x103
#define UNO_BK_LEFT   0x104
#define UNO_BK_CTRL   0x105

/* What this key contributes to the held-key bitmap, as UNO_KH_* bits
 * (hid_kbd.h).  0 for a key nothing is bound to.
 *
 * SAFE TO CALL FROM THE KEYBOARD PATH, and it has to be: this is the one
 * function the two producers call.  It never touches the filesystem - the
 * store is loaded once, lazily, from the first call that can afford it.  The
 * PS/2 tracker was reworked to hold KEY IDS rather than bits precisely so that
 * this is reached from app context and not from a make/break handler. */
int uno_bind_bits(int keyid);

/* The keys on one action, for a menu to print: "Left / A", "Space / E".
 * Writes at most `cap` bytes including the terminator; returns the length. */
int uno_bind_name(int action, char *buf, int cap);

/* Put `keyid` on `action`, exclusively in both directions: the action loses
 * what it had, and the key is taken off every other action.  -> 1 if it took.
 *
 * Refuses UNO_KH_USE.  Use is the one action this machine reads as a key
 * EVENT rather than from the held bitmap (the engine sets use_press from
 * key()), and this table only reaches the bitmap - so accepting a rebind
 * would store a binding that does nothing.  Refusing says so instead. */
int uno_bind_set(int action, int keyid);

/* Every binding back to the shipped defaults. */
void uno_bind_reset(void);

/* Small named strings, persisted beside the bindings.  `uno_pref_get` returns
 * the length written (0 if there is no such preference). */
int uno_pref_get(const char *name, char *buf, int cap);
int uno_pref_set(const char *name, const char *value);

/* Drop the in-memory copy, so the next call reads the store again.  Wanted
 * after the volume map changes under us (detach remaps it), and it is what
 * lets the host gate prove a binding really did reach the file rather than
 * only the table it was set in. */
void uno_binds_reload(void);

/* (uni, scan) as the input ring delivers them -> a key id, or 0 if that key
 * cannot carry a binding.  Lives here rather than in the Python module so
 * that anything else wanting to accept a key for binding agrees about it. */
int uno_bind_keyid(int uni, int scan);

#endif
