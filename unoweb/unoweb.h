/* ===========================================================================
 * unoweb - the web core: DOM store, HTML parser, and (from M3) CSS, layout
 * and paint.
 *
 * CONTRACT VERSION 0.1  [EXPERIMENTAL until M2 lands; see UNOWEB.md changelog]
 *
 * The counterpart to unojs. Where unojs is a JavaScript engine that knows
 * nothing about documents, unoweb is a document engine that knows nothing
 * about JavaScript: there is no ujs_val in this header, no script evaluation,
 * and the library must build and render with no JS engine linked at all.
 * Scripts reach the outside through ONE callback (uw_hooks::script), and the
 * embedder decides what - if anything - to do with them.
 *
 * Memory model: one arena per document. Nodes are bump-allocated and never
 * individually freed, so a uw_node* stays valid for the life of the document
 * and dies wholesale with uw_doc_free(). That is deliberate: navigation is the
 * unit of reclamation, and it removes the entire dangling-node class of bugs
 * at the cost of holding detached subtrees until the page goes away.
 * ======================================================================== */
#ifndef UNOWEB_H
#define UNOWEB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UW_VERSION_MAJOR 0
#define UW_VERSION_MINOR 1

typedef struct uw_doc    uw_doc;
typedef struct uw_node   uw_node;
typedef struct uw_parser uw_parser;

/* An interned name (tag or attribute). 0 is "no atom". Comparing atoms is the
 * fast path the CSS matcher will lean on in M3. */
typedef unsigned int uw_atom;

/* ---- node types ---------------------------------------------------------- */
typedef enum {
    UW_NODE_DOCUMENT = 1,
    UW_NODE_ELEMENT,
    UW_NODE_TEXT,
    UW_NODE_COMMENT,
    UW_NODE_DOCTYPE
} uw_node_type;

/* ---- configuration ------------------------------------------------------- */
typedef struct {
    /* Allocator for the document arena. NULL uses malloc/free. */
    void *(*alloc)(void *user, size_t n);
    void  (*free)(void *user, void *p);
    void  *alloc_user;

    /* Hard ceiling on the document arena in bytes; 0 = the default (16 MB).
     * Hitting it stops the parse and flags uw_doc_truncated(), rather than
     * growing without bound on a hostile or runaway page. */
    size_t arena_max;

    /* Maximum open-element depth; 0 = the default (256). Deeply nested markup
     * is a classic denial-of-service shape, and the tree builder is recursive
     * in structure even though it is iterative in code. */
    int max_depth;
} uw_config;

/* ---- documents ----------------------------------------------------------- */
uw_doc *uw_doc_new(const uw_config *cfg);
void    uw_doc_free(uw_doc *d);

/* 1 if the arena ceiling or a depth limit cut the document short. */
int     uw_doc_truncated(uw_doc *d);
size_t  uw_doc_used(uw_doc *d);

uw_node *uw_document(uw_doc *d);      /* the document node   */
uw_node *uw_root(uw_doc *d);          /* <html>, once parsed */
uw_node *uw_head(uw_doc *d);
uw_node *uw_body(uw_doc *d);

/* ---- parsing -------------------------------------------------------------
 * The parser is a push interface: bytes arrive as they do from the network,
 * and the tree grows incrementally. */
typedef struct {
    void *user;

    /* A </script> was reached. `src`/`len` are the element's text. The
     * embedder may call uw_parse_insert() from inside this callback to splice
     * generated markup into the input stream (that is document.write). When no
     * hook is installed the script text is simply dropped and parsing
     * continues - which is the NoScript build, and the way the golden tests
     * run. */
    void (*script)(void *user, uw_parser *p, uw_node *el, const char *src, int len);

    /* A <link rel=stylesheet>, <style> block, or <img> was seen. Purely
     * informational in M2; the fetch queue hangs off these in M4. */
    void (*resource)(void *user, uw_node *el, const char *url, int kind);
} uw_hooks;

enum { UW_RES_STYLESHEET = 1, UW_RES_IMAGE, UW_RES_SCRIPT_SRC };

uw_parser *uw_parse_begin(uw_doc *d, const uw_hooks *hooks);
/* Feed bytes. Returns 0 on success, -1 if the document hit a limit. */
int  uw_parse_feed(uw_parser *p, const char *bytes, int n);
/* Finish: closes any open elements. The parser is invalid afterwards. */
int  uw_parse_end(uw_parser *p);
/* Splice markup at the current insertion point (document.write). Only valid
 * from inside a script hook. */
int  uw_parse_insert(uw_parser *p, const char *bytes, int n);

/* Parse a fragment in the context of `ctx` (this is innerHTML). Existing
 * children of `ctx` are removed first. */
int  uw_parse_fragment(uw_doc *d, uw_node *ctx, const char *html, int n);

/* One-shot convenience: parse a whole string into a fresh document. */
uw_doc *uw_parse_string(const char *html, int n, const uw_config *cfg);

/* ---- tree navigation ----------------------------------------------------- */
uw_node_type uw_type(uw_node *n);
uw_atom      uw_tag(uw_node *n);                       /* elements only */
const char  *uw_tag_name(uw_doc *d, uw_node *n);       /* "" for non-elements */
/* Text/comment payload. `len` may be NULL. Returns NULL for other types. */
const char  *uw_text(uw_node *n, int *len);

uw_node *uw_parent(uw_node *n);
uw_node *uw_first_child(uw_node *n);
uw_node *uw_last_child(uw_node *n);
uw_node *uw_next_sibling(uw_node *n);
uw_node *uw_prev_sibling(uw_node *n);

/* ---- attributes ---------------------------------------------------------- */
/* Value of `name` (ASCII case-insensitive), or NULL. */
const char *uw_attr(uw_doc *d, uw_node *n, const char *name);
int         uw_has_attr(uw_doc *d, uw_node *n, const char *name);
int         uw_nattrs(uw_node *n);
/* Enumerate: i in [0, uw_nattrs). Either out pointer may be NULL. */
int         uw_attr_at(uw_doc *d, uw_node *n, int i,
                       const char **name, const char **value);

/* ---- atoms --------------------------------------------------------------- */
uw_atom     uw_intern(uw_doc *d, const char *s, int len);
const char *uw_atom_name(uw_doc *d, uw_atom a);

/* ---- mutation ------------------------------------------------------------
 * Every mutation marks the affected subtree dirty for restyle/relayout; the
 * flags are read (and cleared) by the layout pass in M3. */
uw_node *uw_create_element(uw_doc *d, const char *tag);
uw_node *uw_create_text(uw_doc *d, const char *text, int len);
/* Insert `child` into `parent` before `ref` (NULL = append). 0 on success. */
int      uw_insert_before(uw_doc *d, uw_node *parent, uw_node *child, uw_node *ref);
int      uw_append(uw_doc *d, uw_node *parent, uw_node *child);
int      uw_remove(uw_doc *d, uw_node *n);
int      uw_set_attr(uw_doc *d, uw_node *n, const char *name, const char *value);
int      uw_remove_attr(uw_doc *d, uw_node *n, const char *name);
int      uw_set_text(uw_doc *d, uw_node *n, const char *text, int len);

enum { UW_DIRTY_STYLE = 1, UW_DIRTY_SUBTREE = 2, UW_DIRTY_LAYOUT = 4 };
unsigned uw_dirty(uw_node *n);
void     uw_clear_dirty(uw_doc *d);

/* ---- queries ------------------------------------------------------------- */
uw_node *uw_get_element_by_id(uw_doc *d, const char *id);
/* Depth-first walk helper: the next node in document order after `n`, or NULL.
 * Pass `root` to stay inside a subtree. */
uw_node *uw_next_in_order(uw_node *n, uw_node *root);
/* Collect elements with a given tag. Returns the count; fills up to `max`. */
int uw_elements_by_tag(uw_doc *d, uw_node *root, const char *tag,
                       uw_node **out, int max);

/* ---- serialization ------------------------------------------------------- */
/* Serialize `n`'s children as HTML (this is innerHTML). Returns the length
 * written, or the length it WOULD need when that exceeds `max` (so a caller
 * can size a buffer). Always NUL-terminates when max > 0. */
int uw_serialize(uw_doc *d, uw_node *n, char *out, int max);

/* An indented tree dump, one node per line. This is the format the golden
 * tests compare, so its shape is part of the contract:
 *
 *   #document
 *     html
 *       head
 *       body
 *         p class="x"
 *           "hello"
 */
int uw_dump(uw_doc *d, uw_node *n, char *out, int max);

/* ---- text extraction ----------------------------------------------------- */
/* Concatenated descendant text (this is textContent). Same return rule as
 * uw_serialize. */
int uw_text_content(uw_doc *d, uw_node *n, char *out, int max);

#ifdef __cplusplus
}
#endif
#endif /* UNOWEB_H */
