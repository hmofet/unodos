/* ===========================================================================
 * uodlg - the Office 97 dialog engine (OFFICE97-PLAN §5 phase 6c).
 *                                                          [EXPERIMENTAL]
 *
 * Office 97 has on the order of thirty shared dialogs - Font, Paragraph,
 * Tabs, Borders and Shading, Page Setup, Format Cells, Options, Print... -
 * and they are all the same handful of controls arranged differently.  So
 * this is one engine and they are DATA TABLES over it, not thirty pieces of
 * bespoke code.  An app declares a `uod_dlg` of `uod_item`s at
 * dialog-relative coordinates and hands it over; uodlg lays it out, draws it
 * in the Windows 95 idiom, runs the keyboard, and reports which button
 * closed it.
 *
 * MODAL WITHIN THE CANVAS.  unoui has no dialog primitive at all - apps fake
 * one with a second window and nothing blocks input to the parent - so the
 * suite draws its dialogs inside its own UI_CANVAS, over the document, and
 * swallows every event while one is up.  That is also what Office 97 looked
 * like: a dialog was a window, not an overlay panel.
 *
 * Same three rules as uochrome: geometry computed once (uod_item_rect is the
 * only source), metrics derived from the font, behaviour a pure function of
 * the event stream.
 * ======================================================================== */
#ifndef UODLG_H
#define UODLG_H

#include "uochrome.h"

/* ---- controls --------------------------------------------------------------
 * Every one carries a label that may hold a '&' mnemonic, and a rect in
 * DIALOG-RELATIVE coordinates.  `page` is which tab it belongs to, or -1 for
 * "every page" (the OK/Cancel row, the preview). */
enum {
    UOD_LABEL = 0,   /* static text                                          */
    UOD_BUTTON,      /* a push button; UOD_DEFAULT gives it the extra ring    */
    UOD_CHECK,       /* a check box; value is 0/1                            */
    UOD_RADIO,       /* one of a group: `group` ties them together           */
    UOD_EDIT,        /* a text field                                         */
    UOD_LIST,        /* a scrolling list; value is the selected row          */
    UOD_COMBO,       /* a drop-down list; value is the selected row          */
    UOD_SPIN,        /* a number field with steppers; value is the number    */
    UOD_GROUP,       /* an etched group box with a label notch               */
    UOD_PREVIEW      /* a sunken box the app paints into (uod_preview_rect)  */
};

enum {
    UOD_DEFAULT  = 1,   /* the button Enter presses                          */
    UOD_DISABLED = 2
};

typedef struct {
    int         kind;
    int         id;                 /* what uod_value/uod_text key on        */
    const char *text;               /* label, or the initial edit contents   */
    int         x, y, w, h;         /* dialog-relative                       */
    int         page;               /* tab index, or -1 for all pages        */
    unsigned    flags;
    const char *const *list;        /* LIST / COMBO entries                  */
    int         nlist;
    int         group;              /* RADIO: which set it belongs to        */
    int         lo, hi;             /* SPIN: the range                       */
} uod_item;

typedef struct {
    const char *title;
    const uod_item *item;
    int         n;
    const char *const *tab;         /* tab captions, or NULL for a plain box */
    int         ntab;
    int         w, h;               /* the dialog's size                     */
    int         help;               /* draw the "?" title-bar button         */
} uod_dlg;

/* ---- the live dialog ------------------------------------------------------- */
#define UOD_MAXITEM 64
#define UOD_MAXEDIT 8
#define UOD_EDITLEN 64

typedef struct {
    const uod_dlg *d;
    const uoc_look *look;
    int x, y;                       /* top-left on screen                    */
    int page;                       /* the active tab                        */
    int focus, hot, down;           /* item indices, -1 for none             */
    int pop;                        /* an open combo's item index, -1        */
    int pop_hot;
    int drag;                       /* the title bar being dragged           */
    int drag_dx, drag_dy;
    int val[UOD_MAXITEM];
    int edit_of[UOD_MAXITEM];       /* item -> text slot, -1                 */
    char text[UOD_MAXEDIT][UOD_EDITLEN];
    int caret;
    int result;                     /* the id that closed it, 0 while up     */
    int open;
} uod_ui;

/* The ids a message box reports.  Apps may use any other positive id. */
enum { UOD_ID_OK = 1, UOD_ID_CANCEL, UOD_ID_YES, UOD_ID_NO, UOD_ID_HELP };

/* Open `d` centred in a `sw` x `sh` frame.  Initial values come from the
 * items themselves (an EDIT's text, a CHECK's flags); override afterwards
 * with uod_set_value / uod_set_text. */
void uod_open(uod_ui *s, const uod_dlg *d, int sw, int sh);
void uod_close(uod_ui *s);
int  uod_is_open(const uod_ui *s);

/* 1 if the dialog consumed the event - which, while one is open, is always
 * true for input: that is what modal means. */
int  uod_handle(uod_ui *s, const unoui_event *e);
void uod_render(const uod_ui *s);

/* 0 while the dialog is still up, otherwise the id of the button that closed
 * it (UOD_ID_CANCEL when Esc or the close box did). */
int  uod_result(const uod_ui *s);

int  uod_value(const uod_ui *s, int id);
void uod_set_value(uod_ui *s, int id, int v);
const char *uod_text(const uod_ui *s, int id);
void uod_set_text(uod_ui *s, int id, const char *t);

/* Where a UOD_PREVIEW landed on screen, so the app can paint its own sample
 * inside it after uod_render(). */
int  uod_preview_rect(const uod_ui *s, int id, int *x, int *y, int *w, int *h);

/* ---- message boxes ---------------------------------------------------------
 * The one dialog that is not an app's data table, because "Save changes to
 * Document1?" has no home otherwise. */
enum { UOD_MB_OK = 0, UOD_MB_OKCANCEL, UOD_MB_YESNO, UOD_MB_YESNOCANCEL };

void uod_msgbox(uod_ui *s, const char *title, const char *text,
                int buttons, int sw, int sh);

#endif /* UODLG_H */
