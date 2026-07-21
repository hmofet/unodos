/* Opt-in MTRR rebuild for a write-combining framebuffer (P3) - see pc64_mtrr.c.
 * OPT-IN and reversible: STRESS.CFG `mtrr-wc`, operator present. Returns 0 on
 * success, -1 if it refused (logged why). Snapshots + can restore in-session. */
#ifndef PC64_MTRR_H
#define PC64_MTRR_H

int  uno_pc64_mtrr_wc_fb(unsigned long long fb_base, unsigned long long fb_size);
void uno_pc64_mtrr_restore(void);

#endif
