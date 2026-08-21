/*
 * VENDORED FILE - DO NOT EDIT HERE.
 *
 * UnoCode is developed at https://github.com/hmofet/unocode-desktop, in its core/ directory.
 * An edit made here is lost at the next sync, and until then it silently
 * forks the editor away from the tree the desktop builds are cut from.
 *
 * Change it there; bring it back with pc64/tools/sync_unocode.py.
 * See pc64/UNOCODE-UPSTREAM.md.
 */
/* ===========================================================================
 * uc_secret.h - the secret store seam: where an API key lives, and the whole
 * of the editor's opinion about it.
 *
 * The shape is VS Code's SecretStorage - store, get, delete, by name - and
 * the POINT is what the store is backed by, which is the one thing the editor
 * cannot know and must not guess.  A hosted build answers with what its OS
 * actually offers: DPAPI on Windows, the Keychain on macOS, and on Linux a
 * file that only this user can read, because inventing a vault where the OS
 * provides none would be a lie with a padlock icon.  pc64 answers with a plain
 * file on a FAT volume, which protects nothing and SAYS so.
 *
 * That honesty is load-bearing, not decorative: uc_secret_store_name() is
 * shown in the UI whenever a key is saved, and uc_secret_plaintext() is how
 * the UI knows to warn rather than reassure.  Studio kept keys in AI.CFG in
 * plaintext and said so in the file; this seam keeps that virtue while letting
 * a platform that can do better, do better.
 *
 * What this is NOT: a password manager.  Secrets are small (a few hundred
 * bytes), few (a handful), and read at the moment of use rather than held.
 * Nothing here enumerates them, because a list of everyone's secret NAMES is
 * itself information an extension has no business asking for.
 *
 * Callers and their names:
 *   - the built-in assistant's key is "anthropic.key" (uc_cmd.c sets it,
 *     UCD-49 reads it);
 *   - an extension reaching context.secrets gets its names prefixed
 *     "EXT.<ID>." by uc_api.c, so extensions cannot read each other's
 *     secrets - or the assistant's key - by guessing a string.
 * ======================================================================== */
#ifndef UC_SECRET_H
#define UC_SECRET_H

/* Longest value the seam moves, chosen for API keys with headroom.  A caller's
 * buffer of this size always suffices. */
#define UC_SECRET_MAX 512

/* Store `value` under `name`, replacing what was there.  Returns 1, or 0 when
 * the platform store refused (disk full, keychain locked) - in which case
 * nothing was stored and the old value, if any, is intact. */
int uc_secret_set(const char *name, const char *value);

/* Fetch into out[cap], NUL-terminated.  Returns 1, or 0 when there is no such
 * secret - out[0] is 0 either way, so a caller that forgets to check reads an
 * empty string rather than stale stack. */
int uc_secret_get(const char *name, char *out, int cap);

/* Remove.  Returns 1 whether it existed or not - "make it not exist" has one
 * outcome - and 0 only when the store failed to carry the removal out. */
int uc_secret_del(const char *name);

/* The backing store, named for a human: "Windows DPAPI", "the macOS Keychain",
 * "a file only this user can read".  Shown in the UI every time a key is
 * saved, so the user is told where it went rather than left to assume. */
const char *uc_secret_store_name(void);

/* 1 when the store is really just a readable file - anyone with the disk has
 * the key.  The UI warns instead of reassuring when this is set. */
int uc_secret_plaintext(void);

#endif
