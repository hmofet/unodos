/* ===========================================================================
 * unoweb internals - shared by the DOM store and the HTML parser.
 * NOT a public header: embedders see unoweb.h only.
 * ======================================================================== */
#ifndef UW_INT_H
#define UW_INT_H

#include "unoweb.h"
#include <string.h>

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

/* ---- the document arena --------------------------------------------------
 * Bump allocation out of a chain of chunks. Individual objects are never
 * freed; the whole chain dies with the document. Chunks are never moved or
 * reallocated, which is what makes a uw_node* stable for the life of the page
 * even as the tree keeps growing under it. */
#define UW_CHUNK_MIN (64u * 1024u)

typedef struct uw_chunk {
    struct uw_chunk *next;
    size_t used, cap;
    char   data[1];
} uw_chunk;

/* ---- attributes ----------------------------------------------------------
 * A singly-linked list rather than a growable array: the arena cannot realloc,
 * and per-attribute allocation avoids abandoning a half-full array every time
 * one more attribute shows up. Counts are small, so the linear walk is fine. */
typedef struct uw_attr_ent {
    struct uw_attr_ent *next;
    uw_atom  name;
    char    *value;
    u32      vlen;
} uw_attr_ent;

struct uw_node {
    u8       type;
    u8       flags;
    u16      dirty;
    uw_atom  tag;                       /* elements only */
    struct uw_node *parent, *first, *last, *prev, *next;
    char    *text;   u32 tlen;          /* text / comment / doctype payload */
    uw_attr_ent *attrs;
    /* Reserved for later milestones; unoweb hands these out but never
     * interprets them, which is how the CSS/layout passes and the JS binding
     * attach their own state without the DOM knowing what it is. */
    void    *style;                     /* uw_style*  (M3) */
    void    *box;                       /* uw_box*    (M3) */
    void    *wrapper;                   /* the binding's (M5) */
};

/* node flags */
enum { UW_F_VOID = 1 };                 /* an element that cannot have children */

typedef struct { uw_atom id; uw_node *n; } uw_idslot;

struct uw_doc {
    uw_config cfg;
    uw_chunk *chunks;
    size_t    used, max;
    int       truncated;
    int       max_depth;

    /* atoms: interned tag/attribute names, arena-allocated and never freed */
    struct { char *s; u32 len, hash; } *atoms;
    u32       natoms, atomcap;
    u32      *atom_hash;
    u32       atom_hashcap;

    uw_node  *document, *html, *head, *body;

    /* id -> node, keyed by the interned id VALUE */
    uw_idslot *ids;
    u32        nids, idcap;

    /* the cascade: UA sheet first, then author sheets in document order */
    uw_sheet  *sheets, *sheets_tail;
    struct uw_box *layout_root;
    uw_metrics     metrics;
    int            layout_w, layout_h;
    struct uw_paint_list *paint;
    int        styled;
    int        vw, vh;                  /* viewport, for percentage roots */
};

/* ---- CSS internals (uw_css.c <-> uw_style.c) ------------------------------
 * The rule/declaration structs stay private to uw_css.c; the cascade reaches
 * them only through these accessors, so the selector representation can change
 * without touching the property code. */
typedef struct uw_rule uw_rule;
typedef struct uw_decl uw_decl;

uw_rule    *uw_sheet_rules(uw_sheet *s);
uw_rule    *uw_rule_next(uw_rule *r);
uw_decl    *uw_rule_decls(uw_rule *r);
int         uw_rule_spec(uw_rule *r);
int         uw_rule_order(uw_rule *r);
int         uw_rule_matches(uw_doc *d, uw_node *n, uw_rule *r);
uw_decl    *uw_decl_next(uw_decl *x);
uw_atom     uw_decl_prop(uw_decl *x);
const char *uw_decl_value(uw_decl *x);
int         uw_decl_important(uw_decl *x);
int         uw_sheet_origin(uw_sheet *s);
uw_sheet   *uw_sheet_next(uw_sheet *s);
void        uw_sheet_link(uw_sheet *s, uw_sheet *n);

/* ---- internal API -------------------------------------------------------- */
void *uw_arena(uw_doc *d, size_t n);          /* zeroed; NULL when capped */
char *uw_arena_str(uw_doc *d, const char *s, int len);

uw_node *uw_node_new(uw_doc *d, int type);
void     uw_mark_dirty(uw_node *n, unsigned bits);
void     uw_index_id(uw_doc *d, uw_node *n, const char *id, int len);

/* ASCII-only case folding: HTML tag/attribute names are ASCII case-insensitive
 * and locale must never enter into it. */
int  uw_lc(int c);
int  uw_ieq(const char *a, int alen, const char *b, int blen);

/* Is `tag` a void element (br, img, hr, ...)? */
int  uw_is_void(uw_doc *d, uw_atom tag);
/* Tags whose content is not parsed as markup: 1 = RCDATA (entities decoded),
 * 2 = RAWTEXT, 3 = script data. 0 = normal. */
int  uw_raw_kind(uw_doc *d, uw_atom tag);

/* Decode HTML character references from `src` into `dst` (which must have room
 * for `len` bytes; decoding never grows the text). Returns bytes written. */
int  uw_decode_entities(const char *src, int len, char *dst);

#endif /* UW_INT_H */
