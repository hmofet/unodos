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
 * uc_lsp.h - a Language Server Protocol client (UCD-22).
 *
 * WHY A CLIENT AND NOT A LANGUAGE ENGINE.  Everything the editor still fakes -
 * completions from words already in the buffer, "go to definition" that greps -
 * is faked because writing a C parser, then a Python one, then a TypeScript
 * one, is not a task with an end.  LSP is the standing answer: someone else's
 * compiler front end, already correct, already maintained, speaking JSON-RPC
 * down a pipe.  What UnoCode has to build is the pipe and the bookkeeping.
 *
 * THREE THINGS MAKE THIS HARDER THAN "SPAWN AND TALK":
 *
 *   1. A server is a CHILD PROCESS THAT DIES.  clangd runs out of memory on a
 *      pathological header; pyright is killed by the machine going to sleep.  A
 *      client that treats death as fatal leaves the editor permanently without
 *      language support and no way back short of a restart.  So death is
 *      ordinary here: the server is restarted, with a backoff that rises so a
 *      server which cannot start at all does not spin, and the documents that
 *      were open are re-opened on the new one.
 *
 *   2. The protocol is FRAMED, and a frame does not arrive at once.  Reads are
 *      non-blocking and return whatever the pipe had; a header can be split
 *      across two of them and a body across twenty.  Everything here buffers
 *      and re-checks rather than assuming a read is a message.
 *
 *   3. A server sends REQUESTS BACK, and it blocks on them.  clangd asks the
 *      client to create a progress token before it will report progress; if
 *      nobody answers, it waits.  A client that only handles replies looks
 *      like it works and then hangs on the first real workspace.
 *
 * DOCUMENT SYNC IS FULL, NOT INCREMENTAL, and deliberately so.  Incremental
 * sync means the client and the server each hold a copy of the text and agree
 * on every edit forever; one dropped or mis-ranged change and they diverge
 * silently, with the server answering questions about a file that no longer
 * exists.  Full sync costs a copy of the buffer per change and cannot diverge.
 * It is sent on a quiet timer rather than per keystroke, which is where the
 * cost actually goes away.
 *
 * NOTHING HERE BLOCKS.  uc_lsp_tick() is called once a frame beside the
 * terminal's tick and does only what has already arrived.
 *
 * ON pc64 THERE ARE NO PROCESSES, so uc_proc_available() answers 0, no server
 * is ever spawned, and every call here is a no-op that costs nothing.  The
 * editor keeps the heuristics it had.  This is the same shape as the terminal:
 * ask the platform, offer what the answer allows.
 * ======================================================================== */
#ifndef UC_LSP_H
#define UC_LSP_H

/* Unlike uc_net.h and uc_proc.h, this is not a platform seam - it is a
 * subsystem, and it speaks in the editor's own types.  So it takes unocode.h
 * rather than forward-declaring UcDoc and UcJson, neither of which can be
 * forward-declared usefully anyway (UcDoc is a typedef of an anonymous
 * struct). */
#include "unocode.h"

typedef struct UcLsp UcLsp;

/* Lifecycle -------------------------------------------------------------- */

/* Called once at start-up, and again when settings change (the server table
 * lives in settings.json).  Starts nothing by itself: a server is spawned when
 * a document that needs it is opened. */
void uc_lsp_init(void);

/* Once a frame.  Drains the pipes, dispatches whole messages, sends debounced
 * changes, and restarts anything that died and is out of its backoff. */
void uc_lsp_tick(void);

/* Shut every server down politely (shutdown, then exit), then kill whatever is
 * still alive.  A server left running is a compiler holding a gigabyte. */
void uc_lsp_shutdown_all(void);

/* 1 when this platform can run servers at all. */
int uc_lsp_available(void);

/* Turn the traffic log on for this run only, without writing a settings file.
 * The headless --lsp driver uses it; nothing else should. */
void uc_lsp_trace_on(void);

/* Documents -------------------------------------------------------------- */

/* Tell the client about a document.  Idempotent: opening a document twice, or
 * calling this on a document that is already synced, does nothing.  Returns the
 * server, or NULL when this language has none configured. */
UcLsp *uc_lsp_open_doc(UcDoc *d);

/* Note that a document was saved, and that it is gone.  A change needs no call:
 * uc_lsp_tick() notices d->rev and sends it. */
void uc_lsp_did_save(UcDoc *d);
void uc_lsp_close_doc(UcDoc *d);

/* The server serving this document, or NULL.  Only READY servers are returned,
 * so a caller never has to ask about state. */
UcLsp *uc_lsp_for_doc(UcDoc *d);

/* The document's `file://` URI, for building request parameters.  0 on
 * failure (an untitled document has no URI and cannot be sent). */
int uc_lsp_doc_uri(UcDoc *d, char *out, int cap);

/* Talking to a server ---------------------------------------------------- */

/* A reply.  Exactly one of `result` and `error` is non-NULL.  Both are owned by
 * the client and freed as soon as this returns - copy anything kept.
 *
 * It is also called with BOTH NULL when the server died with the request still
 * outstanding, so a caller can stop waiting rather than leak a spinner. */
typedef void (*UcLspReplyFn)(UcJson *result, UcJson *error, void *user);

/* Send a request.  `params` is JSON TEXT, because that is what every caller
 * already has to build and a node tree would be built only to be printed.
 * Returns the request id, or 0 if it could not be sent. */
int uc_lsp_request(UcLsp *s, const char *method, const char *params,
                   UcLspReplyFn cb, void *user);

/* Send a notification: no id, no reply, no way to know it arrived. */
void uc_lsp_notify(UcLsp *s, const char *method, const char *params);

/* Server-sent notifications (publishDiagnostics, logMessage, progress).
 * `params` is owned by the client for the duration of the call. */
typedef void (*UcLspNotifyFn)(UcLsp *s, const char *method, UcJson *params,
                              void *user);
void uc_lsp_on_notify(UcLspNotifyFn fn, void *user);

/* What the server said it can do: the `capabilities` object from its initialize
 * reply, owned by the client and valid until the server restarts.  NULL before
 * initialize completes.  Ask this rather than assuming - clangd has no rename
 * prepare, pyright has no document formatting. */
UcJson *uc_lsp_caps(UcLsp *s);

/* Introspection, for the status bar and the tests ------------------------ */

enum { UC_LSP_OFF = 0, UC_LSP_STARTING, UC_LSP_READY, UC_LSP_DEAD };

int         uc_lsp_count(void);
UcLsp      *uc_lsp_at(int i);
int         uc_lsp_state(UcLsp *s);
const char *uc_lsp_name(UcLsp *s);        /* the command line it was started on */
int         uc_lsp_restarts(UcLsp *s);    /* how many times it has died         */

/* Escape `s` as a JSON string body (no quotes) into `out`.  Every params
 * builder here needs it and uc_json_esc is the one that exists. */
int uc_lsp_esc(char *out, int cap, const char *s);

/* Positions -------------------------------------------------------------
 *
 * LSP counts a column in UTF-16 CODE UNITS, and UnoCode counts one in bytes,
 * in code points and in visual cells depending on who is asking.  All four
 * agree on ASCII and diverge on the first emoji, so a conversion done by hand
 * anywhere but here will be right in every test and wrong in use.  Use these. */
int uc_lsp_pos_to_offset(UcDoc *d, int line, int u16_character);
int uc_lsp_offset_to_u16(UcDoc *d, int off);
/* `{"line":L,"character":C}` for a document offset, ready to paste into a
 * request's params. */
int uc_lsp_pos_json(UcDoc *d, int off, char *out, int cap);

#endif
