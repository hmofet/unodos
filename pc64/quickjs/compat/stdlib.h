/* pc64/quickjs/compat/stdlib.h - pc64's stdlib plus strtod, which the mini
 * libc omits and quickjs's JSON lexer calls. Without a declaration in scope
 * the (-w silenced) implicit prototype returns int through the WRONG
 * REGISTER - every JSON number quietly parsed to 0. The implementation is
 * qjs_port.c's wrapper over the vendored correctly-rounding js_atod. */
#ifndef QJS_COMPAT_STDLIB_H
#define QJS_COMPAT_STDLIB_H

#include_next <stdlib.h>

double strtod(const char *s, char **end);

#endif /* QJS_COMPAT_STDLIB_H */
