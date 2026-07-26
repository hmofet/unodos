/* ===========================================================================
 * unoscript_path - PURE path helpers for the unoscript fs surface.
 *
 * No filesystem, no allocation, no globals: just the security-critical parsing
 * and the home-scope classification, isolated so the host self-test
 * (unoscript_path_test.c) can pin the escape/traversal rules without booting
 * the OS.  The scheme (decided 2026-07-25): unix-like "/vol/path" for an
 * absolute volume path, a bare relative path for the acting user's home
 * ("USERS/<uid>/..." on the primary writable native-FAT volume).  Parent
 * traversal ("..") and "." and "//" are REJECTED outright - a script names a
 * file by explicit path; the absolute "/vol/..." form is the only escape hatch
 * out of home, and it is fs.sys (tier 2).  Rejecting traversal structurally is
 * what keeps a relative path from ever leaving its home subtree.
 * ======================================================================== */
#ifndef UNOSCRIPT_PATH_H
#define UNOSCRIPT_PATH_H

/* 1 if `s` contains any unsafe component - an empty one ("" or "//" or a
 * trailing '/'), "." , or ".." - so "a/../b", "./x", "a//b", "a/" are all
 * unsafe.  An empty string is unsafe.  Caller strips a leading '/' first. */
int uscp_has_traversal(const char *s);

/* Write "USERS/<uid>/<rel>" into out[cap].  `rel` must be non-empty and already
 * traversal-checked.  Returns the string length (>0), or -1 if it would not fit
 * (out is left NUL-terminated-empty on overflow). */
int uscp_home_name(unsigned long uid, const char *rel, char *out, int cap);

/* 1 if the root-relative volume path `name` is inside the acting user's home -
 * it begins with exactly "USERS/<uid>/" and has at least one more char. */
int uscp_under_home(unsigned long uid, const char *name);

/* Split an absolute "/label/rest" path: copy `label` into lab[labcap] and point
 * *rest at the first char past the separating '/'.  Returns 0 on success, -1 if
 * malformed (no leading '/', empty label, label overflow, or no rest after the
 * label).  `*rest` still needs uscp_has_traversal() by the caller. */
int uscp_split_abs(const char *path, char *lab, int labcap, const char **rest);

#endif /* UNOSCRIPT_PATH_H */
