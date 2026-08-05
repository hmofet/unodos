/* uw_bridge.h - internals shared by the csslib<->unoweb bridge:
 * uw_cascade.c (registration, tree walk, computed->uw_style mapping) and
 * uw_select.c (the css_select_handler over unoweb's PUBLIC dom API).
 *
 * The embedder-facing surface is three functions (bottom); everything else
 * here is private to the two files. */
#ifndef CSSLIB_UW_BRIDGE_H
#define CSSLIB_UW_BRIDGE_H

#include <libcss/libcss.h>
#include "../unoweb/unoweb.h"

/* One cascade pass's state. `pool` backs the class-list arrays the select
 * handler returns: libcss unrefs the STRINGS but never frees the ARRAY, and
 * two arrays can be live at once (style-sharing candidates), so they come
 * from a per-pass pool freed when the pass ends - never from a static. */
typedef struct uwx_pool_chunk {
    struct uwx_pool_chunk *next;
} uwx_pool_chunk;

/* libcss's per-node data (ancestor bloom filters etc.), stored for the
 * DURATION OF ONE PASS in a pointer-keyed map and handed back to libcss for
 * deletion at pass end. It cannot be deleted eagerly the way the upstream
 * example does: on a real tree libcss reads a node's data while selecting
 * its DESCENDANTS, and the eager delete is a use-after-free (found by the
 * ASan build of css_cascade_test). It cannot live on uw_node either - it is
 * malloc'd by libcss and would outlive a pass into uw_doc_free, which only
 * reclaims the arena. */
typedef struct uwx_nodedata {
    struct uwx_nodedata *next;
    uw_node *node;
    void    *data;
} uwx_nodedata;

#define UWX_ND_BUCKETS 256

typedef struct {
    uw_doc         *doc;
    uwx_pool_chunk *pool;
    uwx_nodedata   *nd[UWX_ND_BUCKETS];
} uwx_ctx;

void *uwx_pool_alloc(uwx_ctx *cx, size_t n);
void  uwx_pool_free_all(uwx_ctx *cx);
/* hand every stored node-data back to libcss for deletion (pass end) */
void  uwx_nodedata_drop_all(uwx_ctx *cx);

extern css_select_handler uwx_select_handler;

/* ---- embedder surface ----------------------------------------------------- */
/* Register/unregister the libcss stack as unoweb's cascade engine
 * (uw_cascade_set underneath). Registration is idempotent. */
void uwx_libcss_register(void);
void uwx_libcss_unregister(void);
/* "" when healthy; else a short reason the last pass fell back to the
 * built-in cascade (for diagnostics/spectest). */
const char *uwx_libcss_status(void);

#endif /* CSSLIB_UW_BRIDGE_H */
