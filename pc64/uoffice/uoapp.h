/* ===========================================================================
 * uoapp - what an Office app shares with whatever hosts it.   [EXPERIMENTAL]
 *
 * Three things every app in the suite needs and none of them should carry a
 * copy of:
 *
 *   1. THE HOST SEAM.  pc64 hosts a module through the UnoUuiApp vtable, and
 *      that vtable has no way to say "open this file" or "the user closed
 *      your window".  The desktop builds need both - a double-clicked .doc,
 *      and the close box - so an app registers a uoa_app here and the host
 *      calls it.  The other direction (quit, the OS clipboard, the macOS
 *      "edited" dot) is a uoa_host the HOST installs.  pc64 installs none, so
 *      every one of these degrades to what pc64 did before: Exit does
 *      nothing, the clipboard is the app's own.  Same arrangement as
 *      uof_set_native (uofile.h), and for the same reason: a new seam, not a
 *      wider vtable.
 *
 *   2. THE UNSAVED-CHANGES GUARD.  "Do you want to save the changes you made
 *      to Document1?"  Yes / No / Cancel, and whatever the user was doing
 *      (closing, New, Open) happens only after the answer - including after a
 *      Save As the Yes had to open first.  The guard owns that little state
 *      machine; the app owns its dialogs and its save.
 *
 *   3. TEXT ENCODING.  The documents are CP-1252, because that is what unodoc
 *      reads and writes (unodoc/UNODOC.md: the v1 internal encoding) and what
 *      Office 97 stored.  The font engine, the OS and the clipboard speak
 *      UTF-8 / Unicode.  The conversions are here, so an app converts at its
 *      edges - measuring, drawing, typing, the clipboard - and the model stays
 *      one byte per character.
 * ======================================================================== */
#ifndef UOAPP_H
#define UOAPP_H

#include "uodlg.h"

/* ---- 1. the host seam ------------------------------------------------------ */

/* What the guard defers until the document is safe to drop. */
enum { UOA_NONE = 0, UOA_CLOSE, UOA_NEW, UOA_OPEN_DLG, UOA_OPEN_FILE };

typedef struct {
    const char *app;                  /* "UnoWord": the prompt's title        */
    int  (*dirty)(void);              /* unsaved changes?                     */
    const char *(*doc_name)(void);    /* "Document1", "Letter.doc"            */
    /* Save in place.  1 saved; 0 failed (the app has said so); 2 there is no
     * file yet, so the app opened its Save As instead - it then reports the
     * outcome with uoa_save_as_done(). */
    int  (*save)(void);
    /* Do `action` (UOA_*) now; for UOA_OPEN_FILE, uoa_open_vol/_name say
     * which file.  UOA_CLOSE is the guard's to do, and never reaches here. */
    void (*proceed)(int action);
    /* where the prompt is drawn: the app's dialog engine and frame size */
    uod_ui *dl;
    int  (*frame_w)(void), (*frame_h)(void);
    /* the prompt is now up in `dl`: the app marks that dialog as the
     * guard's, so its dialog-closed path calls uoa_prompt_closed().  Called
     * whoever asked - the app's own File > New, or the host's close box. */
    void (*prompted)(void);
} uoa_app;

typedef struct {
    void (*quit)(void);                             /* leave the program      */
    int  (*clip_set)(const char *utf8);             /* 1 = the OS has it      */
    /* the OS clipboard's text as UTF-8 into buf (always NUL-terminated);
     * returns the full length it has, which may exceed cap - 1 */
    long (*clip_get)(char *buf, long cap);
    void (*modified)(int on);                       /* the title's edited mark */
    /* the modifiers held for the key being delivered (UI_MOD_*): the app's
     * key hook is (uni, scan, ctrl) and cannot see Shift otherwise */
    int  (*mods)(void);
} uoa_host;

void uoa_register(const uoa_app *a);       /* the app, from opened()          */
void uoa_set_host(const uoa_host *h);      /* the host, before the app runs   */

/* The host's side.  uoa_host_close: the user asked to close the window; 1 =
 * nothing unsaved, go ahead now; 0 = the app is asking and will quit through
 * uoa_host->quit itself when the answer allows.  uoa_host_open: open a file
 * the OS handed over (a double click, a command-line argument). */
int  uoa_host_close(void);
void uoa_host_open(int vol, const char *name);
/* whether the app reports unsaved changes (0 with no app registered) */
int  uoa_host_dirty(void);
/* Poll once a frame: tells the host's `modified` hook when dirty() changes. */
void uoa_host_tick(void);

/* ---- 2. the guard ----------------------------------------------------------- */

/* Do `action`, asking first if there are unsaved changes.  1 = it ran now;
 * 0 = the prompt is up (the app must mark its dialog as UOA's, see below). */
int  uoa_request(int action);
/* The app's dialog-closed path, for the dialog uoa_request opened.  Call it
 * when the closed dialog was the guard's prompt. */
void uoa_prompt_closed(void);
/* The app's Save As finished: `ok` = the document was written.  Only acts
 * when the guard started that Save As; otherwise it does nothing. */
void uoa_save_as_done(int ok);
/* the file UOA_OPEN_FILE is about */
int         uoa_open_vol(void);
const char *uoa_open_name(void);

/* The modifiers (UI_MOD_*) held for the key being handled, so Shift+arrow
 * can extend a selection.  0 when the host cannot tell (pc64). */
int  uoa_key_mods(void);

/* File > Exit: the same path the close box takes */
void uoa_exit(void);
int  uoa_can_exit(void);                   /* a host that can quit exists     */

/* ---- the clipboard ------------------------------------------------------------
 * UTF-8 text.  With a host clipboard it is the OS's; without one (pc64) it is
 * a buffer inside the app, so Cut and Paste still work within it. */
int  uoa_clip_set(const char *utf8);
/* A copy of the clipboard's text: malloc'd, NUL-terminated, 0 when empty.
 * Free it with the app's own free(). */
char *uoa_clip_get(void);

/* ---- 3. CP-1252 <-> Unicode ------------------------------------------------------ */
int  uoa_1252_to_uc(int b);                /* byte -> code point              */
int  uoa_uc_to_1252(int uc);               /* code point -> byte, -1 if none  */
/* CP-1252 (n bytes, or to the NUL when n < 0) -> UTF-8 in out[cap], always
 * terminated; returns the bytes written.  Stops at a whole character. */
int  uoa_to_utf8(const char *s, long n, char *out, int cap);
/* UTF-8 -> CP-1252 in out[cap], terminated; a character CP-1252 cannot hold
 * becomes '?'.  "\r\n" folds to '\n'.  Returns the bytes written. */
long uoa_from_utf8(const char *u, char *out, long cap);
/* one UTF-8 character at s (n bytes available): its code point in *cp, the
 * byte count as the result (a malformed byte is 1 byte of U+FFFD) */
int  uoa_u8_get(const char *s, int n, int *cp);

#endif /* UOAPP_H */
