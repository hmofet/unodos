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

/* ---- computed style ------------------------------------------------------
 * A FIXED struct, not a property map: every supported property has a slot, so
 * a computed style is one flat record that inherits by copy and compares by
 * memcmp. That is what makes restyle-damage detection cheap in M3's layout
 * pass, and it is why unsupported properties are simply absent rather than
 * carried around as unparsed strings. */
typedef struct { unsigned char r, g, b, a; } uw_color;

/* A length. `unit` decides how `v` is read. Percentages stay symbolic until
 * layout, because they resolve against a containing block that is not known
 * at computed-value time. */
enum { UW_LEN_AUTO = 0, UW_LEN_PX, UW_LEN_PCT };
typedef struct { int v; unsigned char unit; } uw_len;

enum { UW_DISP_INLINE = 0, UW_DISP_BLOCK, UW_DISP_INLINE_BLOCK,
       UW_DISP_LIST_ITEM, UW_DISP_NONE };
enum { UW_FF_SANS = 0, UW_FF_SERIF, UW_FF_MONO };
enum { UW_ALIGN_LEFT = 0, UW_ALIGN_CENTER, UW_ALIGN_RIGHT, UW_ALIGN_JUSTIFY };
enum { UW_WS_NORMAL = 0, UW_WS_PRE, UW_WS_NOWRAP };
enum { UW_BS_NONE = 0, UW_BS_SOLID };
/* side order is CSS order: top, right, bottom, left */
enum { UW_TOP = 0, UW_RIGHT, UW_BOTTOM, UW_LEFT };

typedef struct {
    unsigned char display;
    unsigned char font_family;
    unsigned char font_style;      /* 0 normal, 1 italic */
    unsigned char text_align;
    unsigned char white_space;
    unsigned char underline;
    unsigned char list_bullet;     /* 0 none, 1 disc, 2 decimal */
    unsigned char has_bg;          /* background_color is meaningful */
    int      font_size;            /* px, always resolved */
    int      font_weight;          /* 400 normal, 700 bold */
    int      line_height;          /* px; 0 = "normal" (derived from size) */
    uw_color color, background_color;
    uw_len   margin[4], padding[4];
    int      border_width[4];
    unsigned char border_style[4];
    uw_color border_color[4];
    uw_len   width, height;
} uw_style;

/* ---- stylesheets ---------------------------------------------------------
 * Sheets are arena-allocated inside the document, so they die with it. */
typedef struct uw_sheet uw_sheet;

/* Parse CSS text. `origin` orders the cascade (see below). Returns NULL on a
 * parse failure so severe nothing usable came out; ordinary syntax errors are
 * recovered from per the CSS error rules (skip to the next rule). */
enum { UW_ORIGIN_UA = 0, UW_ORIGIN_AUTHOR };
uw_sheet *uw_css_parse(uw_doc *d, const char *css, int len, int origin);
int       uw_css_nrules(uw_sheet *s);

/* Add a sheet to the document's cascade, in author order. */
int uw_add_sheet(uw_doc *d, uw_sheet *s);
/* Collect and add every <style> element's text. Returns the count added. */
int uw_add_inline_sheets(uw_doc *d);

/* Compute styles for the whole tree: UA sheet, then author sheets in order,
 * then each element's style="" attribute, resolving inheritance. Safe to call
 * repeatedly; layout calls it when UW_DIRTY_STYLE is set. */
int uw_style_document(uw_doc *d, int viewport_w, int viewport_h);

/* The computed style of an element, or NULL if the tree has not been styled
 * (or the node is not an element). */
const uw_style *uw_computed(uw_node *n);

/* Does `n` match the CSS selector text `sel`? This is the same matcher the
 * cascade uses, exposed because querySelector needs it in M5. */
int uw_matches(uw_doc *d, uw_node *n, const char *sel);

/* An indented dump of the computed styles, one element per line. Like uw_dump,
 * the format is part of the contract because the golden tests compare it. */
int uw_style_dump(uw_doc *d, uw_node *n, char *out, int max);

/* ---- layout ---------------------------------------------------------------
 * unoweb measures nothing itself. Text metrics come from the embedder through
 * uw_metrics, because the shape of a glyph is a property of the FONT SYSTEM,
 * not of the document - and hard-coding pc64's font here would make the web
 * core untestable off the OS and unusable anywhere else. The host tests pass a
 * fixed-width fake font, which is also what makes the golden box geometry
 * exact and reproducible. */
typedef struct {
    void *user;
    /* Width in px of `len` bytes of text in style `s`. */
    int (*text_width)(void *user, const uw_style *s, const char *t, int len);
    /* Height of one line box in style `s` (ascent+descent+leading). */
    int (*line_height)(void *user, const uw_style *s);
} uw_metrics;

/* A laid-out box. Geometry is in DOCUMENT coordinates (the page origin, not
 * the viewport), so scrolling never invalidates layout - it only changes what
 * the paint pass replays. */
typedef struct uw_box uw_box;

enum { UW_BOX_BLOCK = 0, UW_BOX_LINE, UW_BOX_TEXT, UW_BOX_BULLET,
       UW_BOX_IMAGE };

/* Lay the document out into `width` pixels. Styles are computed first if the
 * tree is dirty. Returns the total content height, or -1 on failure. */
int uw_layout(uw_doc *d, int width, int height, const uw_metrics *m);

uw_box *uw_layout_root(uw_doc *d);
int     uw_box_type(uw_box *b);
void    uw_box_rect(uw_box *b, int *x, int *y, int *w, int *h);
uw_node *uw_box_node(uw_box *b);
const uw_style *uw_box_style(uw_box *b);
const char *uw_box_text(uw_box *b, int *len);
uw_box *uw_box_first_child(uw_box *b);
uw_box *uw_box_next(uw_box *b);

/* Golden dump of the box tree: one box per line with its geometry. */
int uw_layout_dump(uw_doc *d, char *out, int max);

/* ---- display list ---------------------------------------------------------
 * Layout emits a flat, ordered list of paint commands. The canvas replays it
 * translated by the scroll offset, so SCROLLING NEVER RELAYOUTS. */
enum { UW_CMD_RECT = 1, UW_CMD_BORDER, UW_CMD_TEXT, UW_CMD_BULLET,
       UW_CMD_IMAGE };

typedef struct {
    int      cmd;
    int      x, y, w, h;
    uw_color color;
    /* UW_CMD_TEXT only */
    const char *text;
    int         len;
    const uw_style *style;
    void       *image;             /* UW_CMD_IMAGE: the embedder's handle */
} uw_paint_cmd;

/* Build the display list for the last layout. Returns the command count. */
int uw_paint(uw_doc *d);
int uw_paint_count(uw_doc *d);
const uw_paint_cmd *uw_paint_at(uw_doc *d, int i);
/* ---- images ---------------------------------------------------------------
 * unoweb decodes nothing. An embedder that wants <img> to occupy space and
 * paint supplies this hook: given the resolved src, hand back the intrinsic
 * size and an opaque handle that comes straight back in UW_CMD_IMAGE. Return
 * 0 and the image lays out as an empty replaced box, which is exactly what a
 * broken or still-loading image should do. */
typedef struct {
    void *user;
    int (*resolve)(void *user, const char *src, int *w, int *h, void **handle);
} uw_images;

void uw_set_images(uw_doc *d, const uw_images *im);

/* ---- hit testing ----------------------------------------------------------
 * Which node is at document point (x,y)? The display list is walked BACKWARDS,
 * so the topmost painted thing wins - one geometry source for painting and for
 * pointing, which is what keeps a link's clickable area matching its ink.
 * Returns NULL when nothing is there. */
uw_node *uw_hit_test(uw_doc *d, int x, int y);

/* The nearest enclosing <a href> of `n` (or `n` itself), else NULL. Its href
 * is read with uw_attr as usual. */
uw_node *uw_link_at(uw_doc *d, uw_node *n);

/* Golden dump of the display list. */
int uw_paint_dump(uw_doc *d, char *out, int max);

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
