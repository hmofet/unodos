/* ===========================================================================
 * The builder seam between ud_ppt.c (which owns the presentation model) and
 * ud_pptx.c (which fills the same model from PresentationML).  Internal; not
 * part of the contract.
 *
 * The binary reader finds a slide's text by walking the persist chain into the
 * PowerPoint Document stream and collecting text atoms LAZILY, on the first
 * ud_ppt_slide_text() call - which is right there, because a deck's Document
 * stream is one blob already in memory.  The OOXML reader has the opposite
 * shape: each slide is a separate part that has to be inflated, so its text is
 * built as the deck is opened and handed over here.
 * ======================================================================== */
#ifndef UD_PPT_INT_H
#define UD_PPT_INT_H

/* An empty presentation with no Document stream behind it. */
ud_ppt *ud_ppt_blank(void);

/* Append a slide whose text is already known.  The presentation TAKES the
 * string and frees it at close; NULL is a slide with no text, which is a real
 * thing (a picture-only slide) and not an error. */
int ud_ppt_b_slide(ud_ppt *p, char *text);

#endif /* UD_PPT_INT_H */
