/* A small JavaScript interpreter for the pc64 browser (js.c).
 * Runs a script; document.write() output is appended to `out`, console.log()
 * to `log`. Returns 0 on success, non-zero on a parse/runtime error (with a
 * short message appended to `log`). A tree-walking subset: numbers/strings/
 * booleans, var/let, operators, if/for/while, functions + recursion, arrays,
 * and console.log / document.write / Math.* / String / Number / parseInt. */
#ifndef PC64_JS_H
#define PC64_JS_H
int js_run(const char *src, char *out, int outmax, char *log, int logmax);
#endif
