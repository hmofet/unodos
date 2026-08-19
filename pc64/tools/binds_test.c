/* binds_test.c - host gate for uno_binds.c and the keyboard path it feeds.
 *
 * Seconds, no QEMU, no UEFI.  Run it after every edit to uno_binds.c,
 * hid_kbd.c or the defaults; tools/binds_test.sh builds and runs it twice,
 * once with uno_binds.c linked and once WITHOUT, because the weak fallback in
 * hid_kbd.c has to keep behaving exactly as this machine always did.
 *
 * The check that matters most is the dullest one: A REPORT WITH THE ARROW
 * KEYS DOWN STILL PRODUCES THE SAME BITS.  Everything on this machine that
 * reads the keyboard goes through hid_kbd_keys_held(), so a defaults change
 * hidden inside a commit about a menu would be a change to every app's input,
 * and nothing else in the tree would notice.
 */
#include <stdio.h>
#include <string.h>
#include "hid_kbd.h"
#include "uno_binds.h"

static int fails;
static void ck(const char *what, int ok)
{
    printf("  %-46s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

/* ---- a filesystem, in memory --------------------------------------------
 * uno_binds.c persists through uno_fs_*; the gate is about the table, not
 * about FAT, so these three stand in.  They are also how the round-trip check
 * below sees what was written. */
static unsigned char g_file[2048];
static long g_len;
static int  g_writes;

int uno_fs_pref_vol(void) { return 1; }

long uno_fs_read(int vol, const char *name, unsigned char *buf, long max)
{
    (void)vol; (void)name;
    if (g_len <= 0) return -1;
    if (max > g_len) max = g_len;
    memcpy(buf, g_file, (unsigned long)max);
    return max;
}

int uno_fs_write(int vol, const char *name, const unsigned char *buf, long len)
{
    (void)vol; (void)name;
    if (len > (long)sizeof g_file) return 0;
    memcpy(g_file, buf, (unsigned long)len);
    g_len = len;
    g_writes++;
    return 1;
}

/* ---- helpers -------------------------------------------------------------- */
static int held(unsigned char a, unsigned char b, unsigned char mod)
{
    hid_kbd_state s;
    hid_kbd_reset(&s);
    s.prev[0] = a; s.prev[1] = b;
    s.prevmod = mod;
    return hid_kbd_keys_held(&s);
}

#define U_UP 0x52
#define U_DN 0x51
#define U_RT 0x4F
#define U_LT 0x50
#define U_F  0x09
#define U_SP 0x2C
#define U_E  0x08
#define U_CM 0x36
#define U_PD 0x37
#define U_K  0x0E
#define U_W  0x1A

int main(void)
{
    /* 1. THE DEFAULTS ARE THE OLD HARDCODED TABLE, key for key.  Written out
     *    rather than looped so that a change to any single one of them shows
     *    up as a named failure. */
    ck("Up arrow -> UP",            held(U_UP, 0, 0) == UNO_KH_UP);
    ck("Down arrow -> DOWN",        held(U_DN, 0, 0) == UNO_KH_DOWN);
    ck("Right arrow -> RIGHT",      held(U_RT, 0, 0) == UNO_KH_RIGHT);
    ck("Left arrow -> LEFT",        held(U_LT, 0, 0) == UNO_KH_LEFT);
    ck("F -> FIRE",                 held(U_F,  0, 0) == UNO_KH_FIRE);
    ck("Space -> USE",              held(U_SP, 0, 0) == UNO_KH_USE);
    ck("E -> USE",                  held(U_E,  0, 0) == UNO_KH_USE);
    ck("comma -> strafe left",      held(U_CM, 0, 0) == UNO_KH_SLEFT);
    ck("period -> strafe right",    held(U_PD, 0, 0) == UNO_KH_SRIGHT);
    ck("left Ctrl -> FIRE",         held(0, 0, 0x01) == UNO_KH_FIRE);
    ck("right Ctrl -> FIRE",        held(0, 0, 0x10) == UNO_KH_FIRE);
    ck("two keys at once merge",
       held(U_UP, U_CM, 0) == (UNO_KH_UP | UNO_KH_SLEFT));
    ck("an unbound key does nothing", held(U_W, 0, 0) == 0);
    ck("W is NOT bound by default",   held(U_W, 0, 0) == 0);

#ifdef BINDS_LINKED
    char nm[32];

    /* 2. key ids: what the app's key event turns into */
    ck("scan 4 is the Left key",  uno_bind_keyid(0, 4) == UNO_BK_LEFT);
    ck("'K' binds as 'k'",        uno_bind_keyid('K', 0) == 'k');
    ck("space is a key id",       uno_bind_keyid(' ', 0) == ' ');
    ck("Enter cannot be bound",   uno_bind_keyid(13, 0) == 0);

    /* 3. rebinding, in both directions */
    ck("rebind turn-left to K",   uno_bind_set(UNO_KH_LEFT, 'k') == 1);
    ck("K now turns left",        held(U_K, 0, 0) == UNO_KH_LEFT);
    ck("the Left arrow no longer does",  held(U_LT, 0, 0) == 0);
    uno_bind_name(UNO_KH_LEFT, nm, (int)sizeof nm);
    ck("the menu would print K",  !strcmp(nm, "K"));

    ck("a key is taken off its old action",
       (uno_bind_set(UNO_KH_UP, 'k') == 1) && held(U_K, 0, 0) == UNO_KH_UP);

    /* 4. Use is refused: this table feeds the held bitmap, and Use is read as
     *    a key event, so a stored binding would do nothing at all. */
    ck("Use refuses a rebind",    uno_bind_set(UNO_KH_USE, 'j') == 0);
    ck("...and nothing was stored", held(0x0D /* j */, 0, 0) == 0);

    /* 5. names for the keys that have no character */
    uno_bind_name(UNO_KH_USE, nm, (int)sizeof nm);
    ck("Use prints as Space / E", !strcmp(nm, "Space / E"));
    uno_bind_name(UNO_KH_FIRE, nm, (int)sizeof nm);
    ck("Fire prints as F / Ctrl", !strcmp(nm, "F / Ctrl"));

    /* 6. reset */
    uno_bind_reset();
    ck("reset brings the arrows back", held(U_LT, 0, 0) == UNO_KH_LEFT);
    ck("reset takes K off",            held(U_K,  0, 0) == 0);

    /* 7. preferences, and that both survive a reload from what was written */
    ck("a preference stores",     uno_pref_set("fps", "1") == 1);
    ck("...and reads back",
       uno_pref_get("fps", nm, (int)sizeof nm) == 1 && !strcmp(nm, "1"));
    ck("an unset preference is empty",
       uno_pref_get("nope", nm, (int)sizeof nm) == 0);
    ck("something was actually written", g_writes > 0);

    uno_bind_set(UNO_KH_LEFT, 'k');
    {
        /* reload from the bytes on "disk", the way a reboot would */
        uno_binds_reload();
        ck("a binding survives a reload", held(U_K, 0, 0) == UNO_KH_LEFT);
        ck("a preference survives a reload",
           uno_pref_get("fps", nm, (int)sizeof nm) == 1 && !strcmp(nm, "1"));
    }
#endif

    printf("%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
