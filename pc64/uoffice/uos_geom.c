/* ===========================================================================
 * uos_geom.c - autoshape geometry, colour schemes, layouts and page setups:
 * the static tables UnoShow is built out of.  (OFFICE97-PLAN §7 phase 11.)
 *
 * Every path is a polygon in a 1000 x 1000 box, so ONE description serves the
 * editing zoom, a sorter thumbnail and a full-screen show.  The three shapes a
 * polygon describes badly - ellipse, round rectangle, plain line - are tagged
 * by uos_geom_kind() instead of being approximated with vertices, because a
 * 64-gon circle looks like a 64-gon at show size and costs 64 divisions at
 * thumbnail size.
 *
 * The trigonometry for the pentagon and the star is PRECOMPUTED here rather
 * than called for at runtime.  A .UNO module can import sinf/cosf - PYRT does
 * - but a shape table that is constant has no business being computed 60
 * times a second, and integers keep the host gate and the OS byte-identical.
 * ======================================================================== */
#include "uoshow.h"

/* ---- page setups ------------------------------------------------------------ */
static const struct { const char *name; short w, h; } kPage[UOS_PS_COUNT] = {
    { "On-screen Show",  720,  540 },     /* 10 x 7.5 in                     */
    { "Letter Paper",    720,  540 },     /* 10 x 7.5 in on 8.5 x 11         */
    { "A4 Paper",        756,  532 },     /* 10.5 x 7.4 in                   */
    { "35mm Slides",     828,  540 },     /* 11.25 x 7.5 in                  */
    { "Overhead",        720,  540 },
    { "Banner",         4176,  144 },     /* 58 x 2 in                       */
    { "Custom",          720,  540 }
};
const char *uos_pagesetup_name(int ps)
{ return (ps >= 0 && ps < UOS_PS_COUNT) ? kPage[ps].name : ""; }
void uos_pagesetup_size(int ps, int *w, int *h)
{
    int i = (ps >= 0 && ps < UOS_PS_COUNT) ? ps : UOS_PS_SCREEN;
    if (w) *w = kPage[i].w;
    if (h) *h = kPage[i].h;
}

/* ---- colour schemes ---------------------------------------------------------
 * Eight schemes of our own in PowerPoint's eight-role shape.  They are not
 * traced from Microsoft's: the SPEC asks for the MECHANISM (eight roles, the
 * first eight swatches in every dropdown, Apply to All re-colours a deck) and
 * that is what a scheme table has to prove. */
#define RGB(r,g,b) FB_RGB(r,g,b)
static const uos_scheme kSchemes[] = {
 { "Default",   { RGB(255,255,255), RGB(0,0,0),       RGB(128,128,128),
                  RGB(0,0,0),       RGB(153,153,255), RGB(204,0,0),
                  RGB(0,0,255),     RGB(128,0,128) } },
 { "Blackboard",{ RGB(0,51,51),     RGB(255,255,255), RGB(0,102,102),
                  RGB(153,255,204), RGB(0,102,102),   RGB(255,204,0),
                  RGB(153,204,255), RGB(204,153,255) } },
 { "Midnight",  { RGB(0,0,64),      RGB(255,255,255), RGB(0,0,128),
                  RGB(255,204,102), RGB(51,51,128),   RGB(255,102,102),
                  RGB(153,204,255), RGB(204,153,255) } },
 { "Parchment", { RGB(255,247,222), RGB(51,34,0),     RGB(179,153,102),
                  RGB(102,51,0),    RGB(230,204,153), RGB(153,51,0),
                  RGB(0,51,153),    RGB(102,0,102) } },
 { "Slate",     { RGB(70,80,90),    RGB(240,240,240), RGB(40,48,56),
                  RGB(255,255,255), RGB(100,115,130), RGB(255,170,0),
                  RGB(140,200,255), RGB(200,160,255) } },
 { "Fog",       { RGB(238,238,238), RGB(34,34,34),    RGB(170,170,170),
                  RGB(0,51,102),    RGB(204,214,224), RGB(204,51,0),
                  RGB(0,0,204),     RGB(102,0,153) } },
 { "Forest",    { RGB(240,248,238), RGB(20,48,20),    RGB(150,180,150),
                  RGB(0,80,0),      RGB(190,220,180), RGB(180,90,0),
                  RGB(0,80,160),    RGB(120,0,120) } },
 { "Ink",       { RGB(255,255,255), RGB(0,0,0),       RGB(102,102,102),
                  RGB(0,0,0),       RGB(224,224,224), RGB(0,0,0),
                  RGB(0,0,153),     RGB(102,0,102) } }
};
#define NSCHEMES ((int)(sizeof kSchemes / sizeof kSchemes[0]))

int uos_schemes(void) { return NSCHEMES; }
const uos_scheme *uos_scheme_at(int i)
{ return (i >= 0 && i < NSCHEMES) ? &kSchemes[i] : &kSchemes[0]; }

static const char *const kRole[UOS_NSCHEME] = {
    "Background", "Text and lines", "Shadows", "Title text",
    "Fills", "Accent", "Accent and hyperlink", "Accent and followed hyperlink"
};
const char *uos_scheme_role(int r)
{ return (r >= 0 && r < UOS_NSCHEME) ? kRole[r] : ""; }

/* ---- the 24 AutoLayouts, in the New Slide grid's order --------------------- */
static const char *const kLayout[UOS_AL_COUNT] = {
    "Title Slide", "Bulleted List", "2 Column Text", "Table",
    "Text & Chart", "Chart & Text", "Organization Chart", "Chart",
    "Text & Clip Art", "Clip Art & Text", "Title Only", "Blank",
    "Text & Object", "Object & Text", "Large Object", "Text & Media Clip",
    "Object over Text", "Text over Object", "4 Objects", "2 Objects & Text",
    "2 Column Text & Object", "Text & 2 Objects", "Title & Object",
    "Vertical Text"
};
const char *uos_layout_name(int l)
{ return (l >= 0 && l < UOS_AL_COUNT) ? kLayout[l] : ""; }

/* ---- geometry ---------------------------------------------------------------- */
static const char *const kGeomName[UOS_G_COUNT] = {
    "Rectangle", "Rounded Rectangle", "Oval", "Isosceles Triangle",
    "Right Triangle", "Diamond", "Parallelogram", "Trapezoid",
    "Pentagon", "Hexagon", "Octagon", "Cross",
    "5-Point Star", "Right Arrow", "Left Arrow", "Up Arrow",
    "Down Arrow", "Chevron", "Rounded Callout", "Line"
};
const char *uos_geom_name(int g)
{ return (g >= 0 && g < UOS_G_COUNT) ? kGeomName[g] : ""; }

int uos_geom_kind(int g)
{
    switch (g) {
    case UOS_G_ELLIPSE:   return UOS_GK_ELLIPSE;
    case UOS_G_ROUNDRECT: return UOS_GK_ROUNDRECT;
    case UOS_G_LINE:      return UOS_GK_LINE;
    default:              return UOS_GK_POLY;
    }
}

/* The default adjustment of every shape that has one.  A shape with no
 * adjustment ignores it, so 500 is a safe universal default and the app can
 * hand the same value to any geometry. */
static int adj_or(int adj, int dflt)
{
    if (adj <= 0 || adj >= UOS_GEOM_BOX) return dflt;
    return adj;
}

/* Regular pentagon and 5-point star, circumscribed in the box.  Vertices at
 * -90 + 72k degrees (star inner vertices at -54 + 72k, radius 0.382r). */
static const short kPentagon[10] = { 500,0,  976,191,  794,905,  206,905,  24,191 };
static const short kStar[20] = {
    500,  0,   612,345,   976,191,   682,559,   794,905,
    500,691,   206,905,   318,559,    24,191,   388,345
};

static int emit(short *xy, int maxpt, const short *src, int n)
{
    int i;
    if (n > maxpt) n = maxpt;
    for (i = 0; i < n * 2; i++) xy[i] = src[i];
    return n;
}

int uos_geom_path(int g, int adj, short *xy, int maxpt)
{
    const int B = UOS_GEOM_BOX;
    short t[32];
    int a, b, n = 0;
    if (!xy || maxpt < 3) return 0;

    switch (g) {
    case UOS_G_RECT:
    case UOS_G_ROUNDRECT:            /* the polygon fallback, if ever wanted */
    case UOS_G_ELLIPSE:
    case UOS_G_LINE:
        t[0]=0; t[1]=0; t[2]=B; t[3]=0; t[4]=B; t[5]=B; t[6]=0; t[7]=B;
        n = 4; break;

    case UOS_G_TRIANGLE:
        a = adj_or(adj, 500);
        t[0]=(short)a; t[1]=0; t[2]=B; t[3]=B; t[4]=0; t[5]=B;
        n = 3; break;

    case UOS_G_RTRIANGLE:
        t[0]=0; t[1]=0; t[2]=B; t[3]=B; t[4]=0; t[5]=B;
        n = 3; break;

    case UOS_G_DIAMOND:
        t[0]=500; t[1]=0; t[2]=B; t[3]=500; t[4]=500; t[5]=B; t[6]=0; t[7]=500;
        n = 4; break;

    case UOS_G_PARALLELOGRAM:
        a = adj_or(adj, 250);
        t[0]=(short)a; t[1]=0; t[2]=B; t[3]=0;
        t[4]=(short)(B-a); t[5]=B; t[6]=0; t[7]=B;
        n = 4; break;

    case UOS_G_TRAPEZOID:
        a = adj_or(adj, 250);
        t[0]=(short)a; t[1]=0; t[2]=(short)(B-a); t[3]=0;
        t[4]=B; t[5]=B; t[6]=0; t[7]=B;
        n = 4; break;

    case UOS_G_PENTAGON:
        return emit(xy, maxpt, kPentagon, 5);

    case UOS_G_HEXAGON:
        a = adj_or(adj, 250);
        t[0]=(short)a; t[1]=0; t[2]=(short)(B-a); t[3]=0;
        t[4]=B; t[5]=500; t[6]=(short)(B-a); t[7]=B;
        t[8]=(short)a; t[9]=B; t[10]=0; t[11]=500;
        n = 6; break;

    case UOS_G_OCTAGON:
        a = adj_or(adj, 300);
        t[0]=(short)a; t[1]=0;  t[2]=(short)(B-a); t[3]=0;
        t[4]=B; t[5]=(short)a;  t[6]=B; t[7]=(short)(B-a);
        t[8]=(short)(B-a); t[9]=B; t[10]=(short)a; t[11]=B;
        t[12]=0; t[13]=(short)(B-a); t[14]=0; t[15]=(short)a;
        n = 8; break;

    case UOS_G_CROSS:
        a = (B - adj_or(adj, 300)) / 2;      /* arm start */
        b = B - a;                            /* arm end   */
        t[0]=(short)a; t[1]=0;   t[2]=(short)b; t[3]=0;
        t[4]=(short)b; t[5]=(short)a;  t[6]=B; t[7]=(short)a;
        t[8]=B; t[9]=(short)b;   t[10]=(short)b; t[11]=(short)b;
        t[12]=(short)b; t[13]=B; t[14]=(short)a; t[15]=B;
        t[16]=(short)a; t[17]=(short)b; t[18]=0; t[19]=(short)b;
        t[20]=0; t[21]=(short)a; t[22]=(short)a; t[23]=(short)a;
        n = 12; break;

    case UOS_G_STAR5:
        return emit(xy, maxpt, kStar, 10);

    case UOS_G_ARROW_R:
    case UOS_G_ARROW_L:
    case UOS_G_ARROW_U:
    case UOS_G_ARROW_D: {
        int hw = 400;                         /* head width               */
        int i;
        a = (B - adj_or(adj, 400)) / 2;       /* shaft top                */
        b = B - a;                            /* shaft bottom             */
        /* Drawn pointing right, then reflected/transposed - one arrow, four
         * directions, so a fix to the shape is a fix to all four. */
        t[0]=0; t[1]=(short)a;  t[2]=(short)(B-hw); t[3]=(short)a;
        t[4]=(short)(B-hw); t[5]=0;  t[6]=B; t[7]=500;
        t[8]=(short)(B-hw); t[9]=B;  t[10]=(short)(B-hw); t[11]=(short)b;
        t[12]=0; t[13]=(short)b;
        n = 7;
        for (i = 0; i < n; i++) {
            short px = t[i*2], py = t[i*2+1];
            switch (g) {
            case UOS_G_ARROW_L: t[i*2] = (short)(B - px); break;
            case UOS_G_ARROW_D: t[i*2] = py; t[i*2+1] = px; break;
            case UOS_G_ARROW_U: t[i*2] = py; t[i*2+1] = (short)(B - px); break;
            default: break;
            }
        }
        break;
    }

    case UOS_G_CHEVRON:
        a = adj_or(adj, 300);
        t[0]=0; t[1]=0;  t[2]=(short)(B-a); t[3]=0;
        t[4]=B; t[5]=500;  t[6]=(short)(B-a); t[7]=B;
        t[8]=0; t[9]=B;  t[10]=(short)a; t[11]=500;
        n = 6; break;

    case UOS_G_CALLOUT:
        a = adj_or(adj, 400);                 /* where the tail leaves     */
        t[0]=0; t[1]=0;  t[2]=B; t[3]=0;  t[4]=B; t[5]=700;
        t[6]=(short)(a+200); t[7]=700;  t[8]=(short)a; t[9]=B;
        t[10]=(short)a; t[11]=700;  t[12]=0; t[13]=700;
        n = 7; break;

    default:
        t[0]=0; t[1]=0; t[2]=B; t[3]=0; t[4]=B; t[5]=B; t[6]=0; t[7]=B;
        n = 4; break;
    }
    return emit(xy, maxpt, t, n);
}
