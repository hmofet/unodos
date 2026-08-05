/* uwx.h - the csslib bridge's EMBEDDER surface: what the browser, spectest
 * and any other consumer includes. Deliberately free of libcss types so a
 * consumer needs no csslib include paths - the full internal contract is
 * uw_bridge.h, private to the bridge's own compiles. */
#ifndef CSSLIB_UWX_H
#define CSSLIB_UWX_H

/* Register/unregister the vendored libcss stack as unoweb's cascade engine
 * (uw_cascade_set underneath). Registration is idempotent. */
void uwx_libcss_register(void);
void uwx_libcss_unregister(void);

/* "" when healthy; else a short reason the last pass fell back to the
 * built-in cascade (diagnostics/spectest). */
const char *uwx_libcss_status(void);

#endif /* CSSLIB_UWX_H */
