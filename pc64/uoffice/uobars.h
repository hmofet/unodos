/* ===========================================================================
 * uobars - the Office 97 status bar, ruler and Assistant.  [EXPERIMENTAL]
 * (OFFICE97-PLAN §5 phase 6d; SPEC S-UOW-03 and S-OFF-03.)
 *
 * The three pieces of chrome that are neither a command bar nor a dialog.
 * Each is independent, each is a pure function of its own little model plus
 * the event stream, and each draws through the same uoc_look table so the
 * whole suite still re-tunes from one place.
 * ======================================================================== */
#ifndef UOBARS_H
#define UOBARS_H

#include "uochrome.h"

/* ---- the status bar --------------------------------------------------------
 * Word 97's, left to right: the page/section cells, the position cells, the
 * four mode cells (REC TRK EXT OVR - greyed when off, and DOUBLE-CLICKABLE to
 * toggle, which is the detail people remember), and the spelling book at the
 * right end. */
enum { UOB_CELL_NONE = 0, UOB_CELL_REC, UOB_CELL_TRK, UOB_CELL_EXT,
       UOB_CELL_OVR, UOB_CELL_SPELL };

typedef struct {
    const char *page;      /* "Page 1    Sec 1    1/1"                       */
    const char *pos;       /* "At 2.5cm   Ln 3   Col 12"                     */
    int rec, trk, ext, ovr;/* 0 = greyed out, 1 = active                     */
    int spell_busy;        /* the book animates while a check runs           */
    int spell_errors;      /* 0 = a red tick, 1 = a red cross                */
} uob_status;

int  uob_status_h(void);
void uob_status_render(const uob_status *st, int x, int y, int w);
/* Which cell is at (mx,my)?  UOB_CELL_NONE for the plain areas. */
int  uob_status_hit(int x, int y, int w, int mx, int my);

/* ---- the ruler -------------------------------------------------------------
 * Word's horizontal ruler: a white text area over grey margins, the three
 * indent markers (first line, hanging, and the left square that moves both),
 * the right indent, tab stops, and the tab-type selector at the far left that
 * cycles L / C / R / Decimal. */
enum { UOB_TAB_LEFT = 0, UOB_TAB_CENTER, UOB_TAB_RIGHT, UOB_TAB_DECIMAL };
enum { UOB_DRAG_NONE = 0, UOB_DRAG_FIRST, UOB_DRAG_HANG, UOB_DRAG_BOTH,
       UOB_DRAG_RIGHT, UOB_DRAG_TAB };

#define UOB_MAXTAB 16

typedef struct {
    int text_x, text_w;              /* the text column, in ruler pixels     */
    int first, hang, right;          /* indents, relative to text_x          */
    int tab[UOB_MAXTAB];             /* tab stops, relative to text_x        */
    int tabtype[UOB_MAXTAB];
    int ntab;
    int pick;                        /* the tab type the selector shows      */
    int drag, drag_tab;              /* live drag state                      */
} uob_ruler;

void uob_ruler_init(uob_ruler *r, int text_x, int text_w);
int  uob_ruler_h(void);
void uob_ruler_render(const uob_ruler *r, int x, int y, int w);
/* 1 if the ruler consumed the event. */
int  uob_ruler_handle(uob_ruler *r, const unoui_event *e, int x, int y, int w);

/* ---- the Office Assistant --------------------------------------------------
 * The frame, faithfully; the character, ours.  "Uno" is a friendly card with
 * two eyes - deliberately not a paperclip, a dog, a cat or a wizard.  The
 * SPEC asks for the fidelity of the balloon, the query box and the numbered
 * blue-bullet answers, not for anyone's likeness.  Off by default. */
#define UOB_MAXQUERY 48

typedef struct {
    int x, y;                        /* the character's top-left             */
    int open;                        /* is it on screen at all               */
    int balloon;                     /* is the balloon showing               */
    int bulb;                        /* the lightbulb tip indicator          */
    char query[UOB_MAXQUERY];
    const char *const *topic;        /* the numbered answers                 */
    int ntopic;
    int hot;                         /* the topic under the pointer          */
    int drag, drag_dx, drag_dy;
    int tick;                        /* drives the idle animation            */
} uob_assist;

void uob_assist_open(uob_assist *a, int x, int y);
void uob_assist_close(uob_assist *a);
void uob_assist_ask(uob_assist *a, const char *const *topics, int n);
int  uob_assist_handle(uob_assist *a, const unoui_event *e);
void uob_assist_render(const uob_assist *a);
/* Which numbered topic was chosen, or -1.  Cleared by reading it. */
int  uob_assist_taken(uob_assist *a);

#endif /* UOBARS_H */
