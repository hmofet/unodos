/* ===========================================================================
 * host_fbdim.c - fb_width()/fb_height() for the HOST harness only.
 *
 * pc64's FB_W / FB_H expand to `uno_fb_w` / `uno_fb_h`, which are VARIABLES,
 * and a .UNO module can only import FUNCTIONS - the loader resolves every
 * undefined symbol into a jmp thunk.  pc64 therefore exports fb_width() and
 * fb_height(), and the uoffice lane calls those and never the macros.
 *
 * The host harness links ps2/fb.c, whose FB_W / FB_H are compile-time
 * constants and which has no such functions, so it gets them here.  One shim
 * on the host beats an #ifdef in every file that needs to know how wide the
 * screen is.
 * ======================================================================== */
#include "fb.h"

int fb_width(void)  { return FB_W; }
int fb_height(void) { return FB_H; }
