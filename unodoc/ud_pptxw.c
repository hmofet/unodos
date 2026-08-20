/* ===========================================================================
 * ud_pptxw.c - writing .pptx (PresentationML).
 *
 * The OOXML half of ud_pptw_save, over the same ud_pptw model.
 *
 * A DECK NEEDS MORE SCAFFOLDING THAN THE OTHER TWO.  PowerPoint will not open
 * a presentation whose slides have no layout, whose layout has no master, or
 * whose master has no theme - and it is not being pedantic: those parts are
 * where a slide's inherited text properties come from, so without them there
 * is nothing to render text WITH.  None of it carries any of the model's
 * content, so the four fixed parts are written verbatim from the constants
 * below and the slides are the only part that varies.
 * ======================================================================== */
#include "unodoc.h"
#include "unodoc_int.h"
#include "ud_ooxw_int.h"
#include "ud_ooxz.h"
#include <string.h>

/* ===========================================================================
 * .pptx
 *
 * A deck needs more scaffolding than the other two: PowerPoint will not open a
 * presentation whose slides have no layout, whose layout has no master, or
 * whose master has no theme.  None of that carries any of the model's content
 * - it is four fixed parts - so they are written once, identically, from the
 * string constants below.
 * ======================================================================== */
#define A_NS "xmlns:a=\"" ML "drawingml/2006/main\""
#define P_NS "xmlns:p=\"" ML "presentationml/2006/main\""

static const char THEME[] =
"<a:theme xmlns:a=\"" ML "drawingml/2006/main\" name=\"unodoc\">"
"<a:themeElements>"
"<a:clrScheme name=\"unodoc\">"
"<a:dk1><a:sysClr val=\"windowText\" lastClr=\"000000\"/></a:dk1>"
"<a:lt1><a:sysClr val=\"window\" lastClr=\"FFFFFF\"/></a:lt1>"
"<a:dk2><a:srgbClr val=\"44546A\"/></a:dk2>"
"<a:lt2><a:srgbClr val=\"E7E6E6\"/></a:lt2>"
"<a:accent1><a:srgbClr val=\"4472C4\"/></a:accent1>"
"<a:accent2><a:srgbClr val=\"ED7D31\"/></a:accent2>"
"<a:accent3><a:srgbClr val=\"A5A5A5\"/></a:accent3>"
"<a:accent4><a:srgbClr val=\"FFC000\"/></a:accent4>"
"<a:accent5><a:srgbClr val=\"5B9BD5\"/></a:accent5>"
"<a:accent6><a:srgbClr val=\"70AD47\"/></a:accent6>"
"<a:hlink><a:srgbClr val=\"0563C1\"/></a:hlink>"
"<a:folHlink><a:srgbClr val=\"954F72\"/></a:folHlink>"
"</a:clrScheme>"
"<a:fontScheme name=\"unodoc\">"
"<a:majorFont><a:latin typeface=\"Calibri Light\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:majorFont>"
"<a:minorFont><a:latin typeface=\"Calibri\"/><a:ea typeface=\"\"/><a:cs typeface=\"\"/></a:minorFont>"
"</a:fontScheme>"
"<a:fmtScheme name=\"unodoc\">"
"<a:fillStyleLst>"
"<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
"<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
"<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
"</a:fillStyleLst>"
"<a:lnStyleLst>"
"<a:ln w=\"6350\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
"<a:ln w=\"12700\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
"<a:ln w=\"19050\"><a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill></a:ln>"
"</a:lnStyleLst>"
"<a:effectStyleLst>"
"<a:effectStyle><a:effectLst/></a:effectStyle>"
"<a:effectStyle><a:effectLst/></a:effectStyle>"
"<a:effectStyle><a:effectLst/></a:effectStyle>"
"</a:effectStyleLst>"
"<a:bgFillStyleLst>"
"<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
"<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
"<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>"
"</a:bgFillStyleLst>"
"</a:fmtScheme>"
"</a:themeElements>"
"</a:theme>";

static const char MASTER[] =
"<p:sldMaster " P_NS " " A_NS " xmlns:r=\"" NS_R "\">"
"<p:cSld><p:spTree>"
"<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
"<p:grpSpPr/>"
"</p:spTree></p:cSld>"
"<p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\" accent1=\"accent1\""
" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\""
" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/>"
"<p:sldLayoutIdLst><p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst>"
"</p:sldMaster>";

static const char LAYOUT[] =
"<p:sldLayout " P_NS " " A_NS " xmlns:r=\"" NS_R "\" type=\"titleAndBody\" preserve=\"1\">"
"<p:cSld name=\"Title and Content\"><p:spTree>"
"<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
"<p:grpSpPr/>"
"</p:spTree></p:cSld>"
"<p:clrMapOvr><a:overrideClrMapping bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\""
" accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\""
" accent5=\"accent5\" accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/></p:clrMapOvr>"
"</p:sldLayout>";

/* One text shape.  `idx` is the shape id, which must be unique within a slide
 * and must not be 1 (the group shape holds that).  EMUs: 914400 to the inch. */
static void ppt_shape(zbuf *b, int idx, const char *name, const char *text,
                      long x, long y, long cx, long cy, int sz, int bold)
{
    const char *p = text;
    zs(b, "<p:sp><p:nvSpPr><p:cNvPr id=\""); zint(b, idx);
    zs(b, "\" name=\""); zxml(b, name, 1);
    zs(b, "\"/><p:cNvSpPr txBox=\"1\"/><p:nvPr/></p:nvSpPr>");
    zs(b, "<p:spPr><a:xfrm><a:off x=\""); zint(b, x);
    zs(b, "\" y=\""); zint(b, y);
    zs(b, "\"/><a:ext cx=\""); zint(b, cx);
    zs(b, "\" cy=\""); zint(b, cy);
    zs(b, "\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></p:spPr>");
    zs(b, "<p:txBody><a:bodyPr/><a:lstStyle/>");
    /* A newline in the model is a paragraph break on the slide, which is what
       a body of bullet lines is. */
    for (;;) {
        const char *nl = p;
        while (*nl && *nl != '\n' && *nl != '\r') nl++;
        zs(b, "<a:p><a:r><a:rPr lang=\"en-US\" sz=\""); zint(b, sz);
        zs(b, "\" b=\""); zint(b, bold ? 1 : 0);
        zs(b, "\" dirty=\"0\"/><a:t>");
        {
            long n = nl - p;
            char *tmp = (char *)ud_alloc((unsigned long)n + 1);
            if (tmp) {
                memcpy(tmp, p, (unsigned long)n);
                tmp[n] = 0;
                zxml(b, tmp, 0);
                ud_free(tmp);
            }
        }
        zs(b, "</a:t></a:r></a:p>");
        if (!*nl) break;
        p = nl + 1;
        if (nl[0] == '\r' && nl[1] == '\n') p++;
        if (!*p) break;
    }
    zs(b, "</p:txBody></p:sp>");
}

unsigned char *ud_pptxw_save(ud_pptw *w, long *len)
{
    zwrite z;
    zbuf b;
    int i, nsl;

    if (!w) { ud_set_error("pptx write: no presentation"); return 0; }
    nsl = ud_pptw_nslides(w);
    if (nsl < 1) { ud_set_error("pptx write: presentation has no slides"); return 0; }
    memset(&z, 0, sizeof z);

    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<Types xmlns=\"" NS_CT "\">"
           "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
           "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
           "<Override PartName=\"/ppt/presentation.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml\"/>"
           "<Override PartName=\"/ppt/slideMasters/slideMaster1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml\"/>"
           "<Override PartName=\"/ppt/slideLayouts/slideLayout1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml\"/>"
           "<Override PartName=\"/ppt/theme/theme1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.theme+xml\"/>");
    for (i = 0; i < nsl; i++) {
        zs(&b, "<Override PartName=\"/ppt/slides/slide"); zint(&b, i + 1);
        zs(&b, ".xml\" ContentType=\"application/vnd.openxmlformats-officedocument.presentationml.slide+xml\"/>");
    }
    zs(&b, "</Types>");
    zw_part(&z, "[Content_Types].xml", &b);

    root_rels(&z, "ppt/presentation.xml");

    /* ---- ppt/presentation.xml ----
       Slide size is 4:3 at 10 x 7.5 inches, the size a deck with no geometry
       of its own is least surprising at. */
    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<p:presentation " P_NS " " A_NS " xmlns:r=\"" NS_R "\">");
    zs(&b, "<p:sldMasterIdLst><p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst>");
    zs(&b, "<p:sldIdLst>");
    for (i = 0; i < nsl; i++) {
        zs(&b, "<p:sldId id=\""); zint(&b, 256 + i);
        zs(&b, "\" r:id=\"rId"); zint(&b, i + 2); zs(&b, "\"/>");
    }
    zs(&b, "</p:sldIdLst>");
    zs(&b, "<p:sldSz cx=\"9144000\" cy=\"6858000\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/>");
    zs(&b, "</p:presentation>");
    zw_part(&z, "ppt/presentation.xml", &b);

    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<Relationships xmlns=\"" NS_PR "\">");
    zs(&b, "<Relationship Id=\"rId1\" Type=\"" RT "slideMaster\" Target=\"slideMasters/slideMaster1.xml\"/>");
    for (i = 0; i < nsl; i++) {
        zs(&b, "<Relationship Id=\"rId"); zint(&b, i + 2);
        zs(&b, "\" Type=\"" RT "slide\" Target=\"slides/slide");
        zint(&b, i + 1); zs(&b, ".xml\"/>");
    }
    zs(&b, "<Relationship Id=\"rId"); zint(&b, nsl + 2);
    zs(&b, "\" Type=\"" RT "theme\" Target=\"theme/theme1.xml\"/>");
    zs(&b, "</Relationships>");
    zw_part(&z, "ppt/_rels/presentation.xml.rels", &b);

    memset(&b, 0, sizeof b); xml_head(&b); zs(&b, THEME);
    zw_part(&z, "ppt/theme/theme1.xml", &b);

    memset(&b, 0, sizeof b); xml_head(&b); zs(&b, MASTER);
    zw_part(&z, "ppt/slideMasters/slideMaster1.xml", &b);

    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<Relationships xmlns=\"" NS_PR "\">"
           "<Relationship Id=\"rId1\" Type=\"" RT "slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
           "<Relationship Id=\"rId2\" Type=\"" RT "theme\" Target=\"../theme/theme1.xml\"/>"
           "</Relationships>");
    zw_part(&z, "ppt/slideMasters/_rels/slideMaster1.xml.rels", &b);

    memset(&b, 0, sizeof b); xml_head(&b); zs(&b, LAYOUT);
    zw_part(&z, "ppt/slideLayouts/slideLayout1.xml", &b);

    memset(&b, 0, sizeof b);
    xml_head(&b);
    zs(&b, "<Relationships xmlns=\"" NS_PR "\">"
           "<Relationship Id=\"rId1\" Type=\"" RT "slideMaster\" Target=\"../slideMasters/slideMaster1.xml\"/>"
           "</Relationships>");
    zw_part(&z, "ppt/slideLayouts/_rels/slideLayout1.xml.rels", &b);

    for (i = 0; i < nsl; i++) {
        const char *title = ud_pptw_title_at(w, i);
        const char *body  = ud_pptw_body_at(w, i);
        char name[48];

        memset(&b, 0, sizeof b);
        xml_head(&b);
        zs(&b, "<p:sld " P_NS " " A_NS " xmlns:r=\"" NS_R "\"><p:cSld><p:spTree>");
        zs(&b, "<p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr/>");
        if (title && title[0])
            ppt_shape(&b, 2, "Title", title, 685800, 457200, 7772400, 1143000, 4400, 1);
        if (body && body[0])
            ppt_shape(&b, 3, "Body", body, 685800, 1828800, 7772400, 4114800, 2400, 0);
        zs(&b, "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>");

        zw_name(name, "ppt/slides/slide", i + 1, ".xml");
        zw_part(&z, name, &b);

        memset(&b, 0, sizeof b);
        xml_head(&b);
        zs(&b, "<Relationships xmlns=\"" NS_PR "\">"
               "<Relationship Id=\"rId1\" Type=\"" RT "slideLayout\" Target=\"../slideLayouts/slideLayout1.xml\"/>"
               "</Relationships>");
        zw_name(name, "ppt/slides/_rels/slide", i + 1, ".xml.rels");
        zw_part(&z, name, &b);
    }

    return zw_finish(&z, len);
}
