/* ===========================================================================
 * The builder seam between ud_doc.c (which owns the document model) and
 * ud_docx.c (which fills the same model from WordprocessingML).  Internal;
 * not part of the contract.
 *
 * WHY IT EXISTS AT ALL.  The two formats disagree about where formatting
 * lives, not about what it is:
 *
 *   .doc  keeps it as EXCEPTION RUNS in 512-byte FKP pages, indexed by the
 *         byte offset of the text they apply to.
 *   .docx keeps it as the tree it is - a run element carrying its own
 *         properties - so it is already known when the text is appended.
 *
 * ud_docx.c could have written FKP pages and Word sprm codes for the binary
 * lookup to decode straight back, which is two encoders' worth of bugs for no
 * gain.  Instead the document may carry an explicit array of formatting runs,
 * and ud_doc_chp_at/ud_doc_pap_at consult it when it is present.  One
 * document model; two ways of getting the formatting into it.
 * ======================================================================== */
#ifndef UD_DOC_INT_H
#define UD_DOC_INT_H

/* An empty document with no container behind it, for a reader that has the
 * text rather than a CFB stream to fetch it from. */
ud_doc *ud_doc_blank(void);

/* Hand over the body text.  The document TAKES the buffer (it frees it at
 * close), and it must be CP-1252 in Word's own convention: '\r' ends a
 * paragraph, '\t' is a tab, '\v' is a line break inside a paragraph.  Every
 * consumer reads it through ud_doc_plain(), which is written against exactly
 * that. */
void ud_doc_b_text(ud_doc *d, char *text, long len);

/* Record the formatting of [cp0,cp1).  Runs are added in increasing cp order
 * (both readers walk the document forwards) and are looked up by binary
 * search; a position no run covers reports the defaults, which is what an
 * unformatted stretch of text should say. */
int ud_doc_b_chp(ud_doc *d, long cp0, long cp1, const ud_chp *chp);
int ud_doc_b_pap(ud_doc *d, long cp0, long cp1, const ud_pap *pap);

#endif /* UD_DOC_INT_H */
