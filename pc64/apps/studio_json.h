/* Studio - a tiny JSON emitter + tolerant extractor for the AI client.
 * No allocation: the emitter writes into a caller buffer, the extractor
 * reads a value at a dotted path.  Enough for the three chat APIs. */
#ifndef STUDIO_JSON_H
#define STUDIO_JSON_H

/* emitter: append to buf[*pos..cap), always keeping buf NUL-terminated */
void jz_raw (char *buf, int *pos, int cap, const char *s);
void jz_str (char *buf, int *pos, int cap, const char *s);   /* "..." escaped */
void jz_strn(char *buf, int *pos, int cap, const char *s, int n);

/* extractor: copy the string value at dotted `path` (e.g.
 * "choices.0.message.content") into out[0..outmax), decoding \uXXXX and the
 * standard escapes.  Returns the decoded length, or -1 if not found. */
int  jz_get_string(const char *json, const char *path, char *out, int outmax);

#endif
