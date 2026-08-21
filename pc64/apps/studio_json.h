/* Studio - a tiny JSON emitter + tolerant extractor for the AI client.
 * No allocation: the emitter writes into a caller buffer, the extractor
 * reads a value at a dotted path.  Enough for the three chat APIs. */
#ifndef STUDIO_JSON_H
#define STUDIO_JSON_H

/* emitter: append to buf[*pos..cap), always keeping buf NUL-terminated */
void jz_raw (char *buf, int *pos, int cap, const char *s);
void jz_str (char *buf, int *pos, int cap, const char *s);   /* "..." escaped */
void jz_strn(char *buf, int *pos, int cap, const char *s, int n);

/* The same string, written in pieces: one opening quote, any number of escaped
 * runs, one closing quote.  For when two things have to become ONE JSON
 * string - which the conversation does whenever a local system notice has to
 * be dropped from between two turns of the same role. */
void jz_open (char *buf, int *pos, int cap);
void jz_more (char *buf, int *pos, int cap, const char *s, int n);
void jz_close(char *buf, int *pos, int cap);

/* ---- a conversation, as an API wants to see it ----------------------------
 * Lives here rather than in studio_ai.c so it can be TESTED: it is pure
 * text-to-JSON over an explicit array, with no pane, no framebuffer and no
 * network in it.  tools/json_test.c drives it directly.
 *
 * Three rules turn a local transcript into a legal API conversation, and each
 * of them is a bug somebody would otherwise hit:
 *
 *  - JZ_SKIP turns are dropped.  Those are the client's own notices ("No API
 *    key.") - addressed to the user, never to the model.
 *  - Consecutive same-role turns are MERGED into one message.  Dropping the
 *    notices is what creates them: a failed request leaves user, notice, user,
 *    which becomes two user messages in a row, which the APIs reject.
 *  - Any leading assistant turn is skipped, because the history must open
 *    with a user message.
 */
enum { JZ_USER = 0, JZ_ASSISTANT = 1, JZ_SKIP = 2 };

typedef struct { int role; const char *text; int len; } jz_turn;

/* Writes the ELEMENTS of the array - no enclosing brackets - so the caller
 * supplies whatever wrapper its provider wants (a bare list for Anthropic, a
 * list already carrying a system message for OpenAI, "contents" for Gemini).
 * `gemini` selects that provider's {role, parts:[{text}]} shape. */
void jz_msgs(char *buf, int *pos, int cap,
             const jz_turn *turns, int n, int gemini);

/* extractor: copy the string value at dotted `path` (e.g.
 * "choices.0.message.content") into out[0..outmax), decoding \uXXXX and the
 * standard escapes.  Returns the decoded length, or -1 if not found. */
int  jz_get_string(const char *json, const char *path, char *out, int outmax);

#endif
