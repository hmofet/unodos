/* ===========================================================================
 * ud_docxw.c - writing .docx (WordprocessingML).
 *
 * The OOXML half of ud_docw_save, over the same ud_docw model: paragraphs of
 * text with bold, italic and alignment, which is what the model holds.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_ooxw_int.h"
#include "ud_ooxz.h"
#include <string.h>

/* ===========================================================================
 * .docx
 * ======================================================================== */
static const char *JC[] = { "left", "center", "right", "both" };

unsigned char *ud_docxw_save(ud_docw *w, long *len)
{
    zwrite z;
    zbuf b;
    int i, np;

    if (!w) { ud_set_error("docx write: no document"); return 0; }
    np = ud_docw_nparas(w);
    memset(&z, 0, sizeof z);

    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<Types xmlns=\"" NS_CT "\">"
           "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
           "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
           "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
           "</Types>");
    zw_part(&z, "[Content_Types].xml", &b);

    root_rels(&z, "word/document.xml");

    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<w:document xmlns:w=\"" ML "wordprocessingml/2006/main\"><w:body>");
    for (i = 0; i < np; i++) {
        int bold = 0, italic = 0, align = 0;
        const char *t = ud_docw_para_at(w, i, &bold, &italic, &align);
        if (!t) continue;
        zs(&b, "<w:p>");
        if (align > 0 && align < 4) {
            zs(&b, "<w:pPr><w:jc w:val=\""); zs(&b, JC[align]); zs(&b, "\"/></w:pPr>");
        }
        zs(&b, "<w:r><w:rPr>");
        if (bold)   zs(&b, "<w:b/>");
        if (italic) zs(&b, "<w:i/>");
        /* The size is stated on every run rather than inherited.  The .doc
           writer supplies it from the single Normal style it writes, and a
           .docx could carry a styles part saying the same thing - but then the
           size only arrives for a consumer that resolves the style chain, and
           the same model would mean 10pt in one format and "unspecified" in
           the other.  Two bytes per run buys one answer. */
        zs(&b, "<w:sz w:val=\"20\"/><w:szCs w:val=\"20\"/>");
        zs(&b, "</w:rPr>");
        zs(&b, "<w:t xml:space=\"preserve\">");
        zxml(&b, t, 0);
        zs(&b, "</w:t></w:r></w:p>");
    }
    /* A body with no sectPr opens, but Word writes one and some readers use
       it to decide the page size, so an empty one costs nothing and answers
       the question. */
    zs(&b, "<w:sectPr/></w:body></w:document>");
    zw_part(&z, "word/document.xml", &b);

    return zw_finish(&z, len);
}
