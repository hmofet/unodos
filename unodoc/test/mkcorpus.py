#!/usr/bin/env python3
"""Seed unodoc/test/corpus/ with real Office 97 binary files.

Per docs/OFFICE97-PLAN.md §9 the corpus is GENERATED, never committed: this
script writes flat-ODF sources (plain XML, deterministic, diffable) and hands
them to LibreOffice headless, which saves them through its "MS Word 97" /
"MS Excel 97" / "MS PowerPoint 97" filters.  Those are real third-party CFB
containers written by someone who is not us, which is the whole point - a
round-trip gate that only ever reads our own writer's output proves nothing.

The set deliberately spans the shapes that break CFB parsers:
  small.*   payload under the 4096-byte cutoff -> the MINI stream and the
            mini FAT carry it
  large.*   hundreds of KB -> many FAT sectors, long chains
  pic.*     an embedded PNG -> a multi-megabyte stream in .ppt's Pictures /
            .doc's Data, enough to push the FAT past the header's 109 DIFAT
            slots on the presentation

Usage:  python3 mkcorpus.py [--force]
Needs:  soffice on PATH (WSL: apt install libreoffice-writer/calc/impress)
"""
import os, shutil, struct, subprocess, sys, zlib

HERE   = os.path.dirname(os.path.abspath(__file__))
SRC    = os.path.join(HERE, "gen", "src")
CORPUS = os.path.join(HERE, "corpus")
PROFILE = os.path.join(HERE, "gen", "loprofile")

NS = (
    'xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
    'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
    'xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" '
    'xmlns:draw="urn:oasis:names:tc:opendocument:xmlns:drawing:1.0" '
    'xmlns:svg="urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0" '
    'xmlns:xlink="http://www.w3.org/1999/xlink" '
    'xmlns:of="urn:oasis:names:tc:opendocument:xmlns:of:1.2" '
    'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
    # fo: carries font-weight, font-style, font-size, text-transform,
    # text-align and the indents - most of the formatting this file exists to
    # test. Leaving it undeclared does not fail: LibreOffice parses the
    # document, drops every attribute in the unknown namespace, and writes a
    # clean .doc with the formatting missing. See the note above FMT_RUNS.
    'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
    'xmlns:number="urn:oasis:names:tc:opendocument:xmlns:datastyle:1.0"'
)

def png(w, h, seed=1):
    """A noisy RGB PNG, written from scratch so it does not compress away."""
    raw = bytearray()
    rnd = seed
    for _y in range(h):
        raw.append(0)                       # filter type 0
        for _x in range(w):
            rnd = (rnd * 1103515245 + 12345) & 0xFFFFFFFF
            raw += bytes(((rnd >> 16) & 0xFF, (rnd >> 8) & 0xFF, rnd & 0xFF))
    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
            + chunk(b"IEND", b""))

def b64(data):
    import base64
    s = base64.b64encode(data).decode()
    return "\n".join(s[i:i + 76] for i in range(0, len(s), 76))

def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")

def escattr(s):
    """esc() plus the quote, for text going inside an XML attribute -
    formulas carry string literals and would otherwise close the attribute."""
    return esc(s).replace('"', "&quot;")

# ---- a document whose formatting describes itself ---------------------------
# Each run's TEXT says what its formatting should be, so a gate can look the
# marker up in the extracted text and assert on the properties found there -
# nothing keyed on an offset we computed ourselves.
#
# THE BUG THIS FILE HID FOR TWO WEEKS, recorded so nobody re-derives it.
# Every property below except underline and strikethrough is an `fo:` attribute,
# and NS did not declare the fo: namespace.  LibreOffice does not complain
# about that: it parses the document, silently drops every attribute in the
# unknown namespace, and writes a perfectly valid .doc with the formatting
# simply absent.  The generated fmt.doc therefore contained no bold, no italic,
# no size, no caps and no alignment - only the two style:* properties, which
# were declared and came through.
#
# That was misread twice.  First as "LibreOffice turns automatic styles into
# Word character styles", then as "unodoc cannot read the STSH yet", and the
# gate was disabled to match.  Neither was true.  unodoc reads direct
# formatting AND resolves the style hierarchy (phase 4b/4b'), and a
# round-trip through our own writer reports bold, italic and alignment
# correctly.  The document was empty of the things it claimed to carry, and
# the reader was right to say so.
#
# The lesson is not about namespaces.  It is that a fixture asserting nothing
# is indistinguishable from a fixture that passes: this file was generated,
# committed, and cited in two conclusions before anyone checked whether it
# contained what its own text said it did.  Assert against the SOURCE'S
# INTENT, and when the answer disagrees, suspect the fixture too.
#
# MEASURED after the fix (2026-08-17), reading the regenerated file back:
# bold, italic, underline, strike, caps, the larger size, and centre / right /
# justify alignment and SPACEPARA's spacing all arrive.  TWO still do not:
# INDENTPARA's fo:margin-left and FIRSTPARA's fo:text-indent read as 0, and
# adding style:parent-style-name did not change that, so it is a separate
# question about how LibreOffice exports paragraph indents rather than more of
# the same namespace bug.  fmt_fixture() below is still not wired to a gate;
# whoever wires it should expect those two to fail and find out why first.
FMT_RUNS = [
    ("PLAINWORD",  "",                       {}),
    ("BOLDWORD",   'fo:font-weight="bold"',  {"bold": 1}),
    ("ITALICWORD", 'fo:font-style="italic"', {"italic": 1}),
    ("ULINEWORD",  'style:text-underline-style="solid" '
                   'style:text-underline-width="auto"', {"underline": 1}),
    ("BIGWORD",    'fo:font-size="18pt"',    {"size": 36}),
    ("STRIKEWORD", 'style:text-line-through-style="solid"', {"strike": 1}),
    ("CAPSWORD",   'fo:text-transform="uppercase"', {"caps": 1}),
]

FMT_PARAS = [
    ("LEFTPARA",   "",                                   {"align": 0}),
    ("CENTREPARA", 'fo:text-align="center"',             {"align": 1}),
    ("RIGHTPARA",  'fo:text-align="end"',                {"align": 2}),
    ("JUSTPARA",   'fo:text-align="justify"',            {"align": 3}),
    ("INDENTPARA", 'fo:margin-left="2cm"',               {"left": 1134}),
    ("FIRSTPARA",  'fo:text-indent="1cm"',               {"first": 567}),
    ("SPACEPARA",  'fo:margin-top="0.5cm"',              {"before": 283}),
]

def fmt_fodt():
    styles = []
    for i, (_m, props, _e) in enumerate(FMT_RUNS):
        styles.append('<style:style style:name="TT%d" style:family="text">'
                      '<style:text-properties %s/></style:style>' % (i, props))
    for i, (_m, props, _e) in enumerate(FMT_PARAS):
        styles.append('<style:style style:name="PP%d" style:family="paragraph">'
                      '<style:paragraph-properties %s/></style:style>' % (i, props))
    body = []
    for i, (marker, _p, _e) in enumerate(FMT_RUNS):
        body.append('<text:p>a <text:span text:style-name="TT%d">%s</text:span>'
                    ' z</text:p>' % (i, marker))
    for i, (marker, _p, _e) in enumerate(FMT_PARAS):
        body.append('<text:p text:style-name="PP%d">%s</text:p>' % (i, marker))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<office:document %s office:version="1.2" '
            'office:mimetype="application/vnd.oasis.opendocument.text">'
            '<office:automatic-styles>%s</office:automatic-styles>'
            '<office:body><office:text>%s</office:text></office:body>'
            '</office:document>' % (NS, "".join(styles), "".join(body)))

def fmt_fixture():
    """marker -> the properties the reader must report at that marker."""
    lines = []
    for marker, _p, exp in FMT_RUNS:
        for k in sorted(exp):
            lines.append("chp\t%s\t%s\t%d" % (marker, k, exp[k]))
    for marker, _p, exp in FMT_PARAS:
        for k in sorted(exp):
            lines.append("pap\t%s\t%s\t%d" % (marker, k, exp[k]))
    return "\n".join(lines) + "\n"

# ---- flat-ODF source documents --------------------------------------------
def fodt(paras, image=None):
    body = "".join("<text:p>%s</text:p>" % esc(p) for p in paras)
    if image:
        body += ('<text:p><draw:frame svg:width="8cm" svg:height="6cm">'
                 '<draw:image><office:binary-data>%s</office:binary-data>'
                 '</draw:image></draw:frame></text:p>' % b64(image))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<office:document %s office:version="1.2" '
            'office:mimetype="application/vnd.oasis.opendocument.text">'
            '<office:body><office:text>%s</office:text></office:body>'
            '</office:document>' % (NS, body))

# ---- a richer spreadsheet builder, with a fixture ---------------------------
# A cell is a tuple (kind, value[, formula]).  kinds: num str bool err empty.
# When a formula is given, LibreOffice computes it on save and BIFF8 stores
# the CACHED result - which is exactly what phase 2 reads back, so the fixture
# records the result we expect, never our own output (that would be circular).
def cell_xml(c):
    kind = c[0]
    if kind == "covered":
        return "<table:covered-table-cell/>"
    val     = c[1]
    formula = c[2] if len(c) > 2 else None
    span    = c[3] if len(c) > 3 else None       # (rows, cols) for a merge
    attr = ' table:formula="of:%s"' % escattr(formula) if formula else ""
    if span:
        attr += (' table:number-rows-spanned="%d" '
                 'table:number-columns-spanned="%d"' % span)
    if kind == "date":
        return ('<table:table-cell%s table:style-name="CD1" '
                'office:value-type="date" '
                'office:date-value="%s"><text:p>%s</text:p>'
                '</table:table-cell>' % (attr, val, val))
    if kind == "empty":
        return "<table:table-cell%s/>" % attr
    if kind == "num":
        return ('<table:table-cell%s office:value-type="float" '
                'office:value="%r"><text:p>%r</text:p></table:table-cell>'
                % (attr, val, val))
    if kind == "bool":
        return ('<table:table-cell%s office:value-type="boolean" '
                'office:boolean-value="%s"><text:p>%s</text:p>'
                '</table:table-cell>'
                % (attr, "true" if val else "false",
                   "TRUE" if val else "FALSE"))
    return ('<table:table-cell%s office:value-type="string">'
            '<text:p>%s</text:p></table:table-cell>' % (attr, esc(val)))

DATE_STYLE = (
    '<office:automatic-styles>'
    '<number:date-style style:name="ND1">'
    '<number:day number:style="long"/><number:text>-</number:text>'
    '<number:month number:style="long"/><number:text>-</number:text>'
    '<number:year number:style="long"/>'
    '</number:date-style>'
    '<style:style style:name="CD1" style:family="table-cell" '
    'style:data-style-name="ND1"/>'
    '</office:automatic-styles>')

def fods_sheets(sheets, named=None):
    """sheets = [(name, rows)] where rows = [[cell, ...], ...];
    named = [(name, odf_range)] for the defined names PtgName renders."""
    out = []
    for name, rows in sheets:
        body = []
        for row in rows:
            body.append("<table:table-row>%s</table:table-row>"
                        % "".join(cell_xml(c) for c in row))
        out.append('<table:table table:name="%s">%s</table:table>'
                   % (esc(name), "".join(body)))
    if named:
        out.append("<table:named-expressions>%s</table:named-expressions>"
                   % "".join(
                       '<table:named-range table:name="%s" '
                       'table:base-cell-address="%s" '
                       'table:cell-range-address="%s"/>'
                       % (esc(n), esc(rng.split(":")[0]), esc(rng))
                       for n, rng in named))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<office:document %s office:version="1.2" '
            'office:mimetype="application/vnd.oasis.opendocument.spreadsheet">'
            '%s<office:body><office:spreadsheet>%s</office:spreadsheet>'
            '</office:body></office:document>' % (NS, DATE_STYLE, "".join(out)))

def fixture(sheets, expect, fml=None):
    """expect = {(sheet, row, col): (KIND, value)} overriding the literal
    cells; everything else is taken from the source document itself.
    fml = {(sheet, row, col): "=SUM(A1:B2)"} - the formula text we expect the
    decompiler to rebuild, written in Excel A1 syntax here and in ODF syntax
    in the source, so neither side is derived from the other."""
    lines = []
    for key in sorted(fml or {}):
        lines.append("formula\t%d\t%d\t%d\t%s" % (key + (fml[key],)))
    for si, (_name, rows) in enumerate(sheets):
        for ri, row in enumerate(rows):
            for ci, c in enumerate(row):
                key = (si, ri, ci)
                # A formula cell's VALUE is whatever the engine computed on
                # save, not the placeholder in the source, so the source
                # cannot state it - the `formula` line is the assertion that
                # matters for those.
                if len(c) > 2 and c[2] and key not in expect:
                    continue
                if key in expect:
                    kind, val = expect[key]
                elif c[0] == "num":
                    kind, val = "NUM", "%.15g" % c[1]
                elif c[0] == "str":
                    kind, val = "STR", c[1]
                elif c[0] == "bool":
                    kind, val = "BOOL", "1" if c[1] else "0"
                else:
                    continue          # empty cells carry no expectation
                lines.append("cell\t%d\t%d\t%d\t%s\t%s" % (si, ri, ci, kind, val))
    return "\n".join(lines) + "\n"

# The strings that stress the SST: long, numerous and distinct, so the table
# runs to hundreds of KB and is split across dozens of CONTINUE records.  A
# split lands mid-string constantly, and mixing pure-ASCII with accented text
# means the 8-bit/UTF-16 flag genuinely differs either side of a boundary -
# the single most common BIFF8 bug (OFFICE97-PLAN §4 phase 2).
ACCENTS = "éàüñçÉÀÖß®"

def sst_strings(n=3000):
    out = []
    rnd = 20260801
    for i in range(n):
        rnd = (rnd * 1103515245 + 12345) & 0xFFFFFFFF
        ln = 40 + (rnd >> 16) % 200
        rnd = (rnd * 1103515245 + 12345) & 0xFFFFFFFF
        accent = (rnd >> 16) % 3 == 0
        body = ("s%05d-" % i) + "".join(
            chr(ord("a") + ((i * 7 + k * 13) % 26)) for k in range(ln))
        if accent:
            body += ACCENTS[i % len(ACCENTS)] * 3
        out.append(body)
    return out

def fodp(slides, image=None):
    pages = []
    for i, (title, lines) in enumerate(slides):
        frames = ('<draw:frame svg:width="22cm" svg:height="3cm" svg:x="2cm" '
                  'svg:y="1cm"><draw:text-box><text:p>%s</text:p>'
                  '</draw:text-box></draw:frame>' % esc(title))
        if lines:
            frames += ('<draw:frame svg:width="22cm" svg:height="10cm" '
                       'svg:x="2cm" svg:y="5cm"><draw:text-box>%s'
                       '</draw:text-box></draw:frame>'
                       % "".join("<text:p>%s</text:p>" % esc(l) for l in lines))
        if image and i == 0:
            frames += ('<draw:frame svg:width="12cm" svg:height="9cm" '
                       'svg:x="10cm" svg:y="8cm"><draw:image>'
                       '<office:binary-data>%s</office:binary-data>'
                       '</draw:image></draw:frame>' % b64(image))
        pages.append('<draw:page draw:name="p%d">%s</draw:page>' % (i, frames))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<office:document %s office:version="1.2" '
            'office:mimetype="application/vnd.oasis.opendocument.presentation">'
            '<office:body><office:presentation>%s</office:presentation>'
            '</office:body></office:document>' % (NS, "".join(pages)))

# ---- LibreOffice ------------------------------------------------------------
def soffice(args):
    env = dict(os.environ)
    env["HOME"] = PROFILE                    # keep LO out of the real profile
    env["SAL_USE_VCLPLUGIN"] = "svp"
    os.makedirs(PROFILE, exist_ok=True)
    return subprocess.run(["soffice", "--headless", "--norestore"] + args,
                          capture_output=True, text=True, env=env, timeout=600)

def convert(src, fmt, outdir):
    r = soffice(["--convert-to", fmt, "--outdir", outdir, src])
    base = os.path.splitext(os.path.basename(src))[0] + "." + fmt.split(":")[0]
    out = os.path.join(outdir, base)
    if not os.path.exists(out):
        raise SystemExit("soffice failed on %s:\n%s\n%s" % (src, r.stdout, r.stderr))
    return out

# ---- the corpus -------------------------------------------------------------
LOREM = ("The quick brown fox jumps over the lazy dog while unodoc walks the "
         "sector chain and refuses to believe a single number the file tells it. ")

import datetime

def serial_1900(iso):
    """Excel's 1900-system day serial. Dates after 1900-02-28 sit one day
    high because 1900 is treated as a leap year (the Lotus bug Excel keeps
    on purpose), which the 1899-12-30 epoch absorbs."""
    y, m, d = (int(v) for v in iso.split("-"))
    return (datetime.date(y, m, d) - datetime.date(1899, 12, 30)).days

# The battery: one cell of every kind BIFF8 can store, plus formulas whose
# cached results cover all four result encodings (number, boolean, error,
# string) including the empty-string case that must NOT swallow the next
# STRING record.
CELLS_SHEETS = [
    ("Values", [
        [("str", "kind"), ("str", "a"), ("str", "b"), ("str", "c")],
        # numbers, including the extremes an RK record cannot hold
        [("num", 0.0), ("num", -1.0), ("num", 1.5), ("num", 0.1),
         ("num", 1e-10), ("num", 1e20), ("num", 12345678901234.0),
         ("num", -0.25)],
        # strings: plain, CP-1252 accents (exact), and text with no CP-1252
        # form at all (folds to '?' per UNODOC.md), and one long one
        [("str", "plain"), ("str", "café naïve Über"),
         ("str", "日本語"), ("str", "z" * 300)],
        [("bool", True), ("bool", False)],
        # inputs for SUM: small integers, so the expected total is exact and
        # cannot depend on summation order
        [("num", 1.0), ("num", 2.0), ("num", 3.0), ("num", 4.0), ("num", 5.0)],
        [("num", 0, "=1+2"),
         ("num", 0, "=1/0"),
         ("str", "", '=CONCATENATE("a";"b")'),
         ("bool", False, "=1>0"),
         ("num", 0, "=SUM([.A5:.E5])"),
         ("num", 0, "=NA()"),
         ("str", "", '=""')],
        [("date", "2026-08-01"), ("date", "1997-01-16")],
    ]),
    ("Second", [
        [("str", "merged", None, (2, 3)), ("covered",), ("covered",)],
        [("covered",), ("covered",), ("covered",)],
        [("str", "after")],
    ]),
]

CELLS_EXPECT = {
    (0, 2, 2): ("STR", "???"),          # no CP-1252 form for any of the three
    # LibreOffice's ODF->BIFF8 export turns LITERAL booleans into plain
    # numbers - verified 2026-08-01 by converting cells.xls back to flat ODF,
    # which contains no boolean-typed cell at all.  So this is LibreOffice's
    # loss, not unodoc's; the BOOLERR record path is proven by the =1>0
    # formula below, which does come back as a real boolean.
    (0, 3, 0): ("NUM", "1"),
    (0, 3, 1): ("NUM", "0"),
    (0, 5, 0): ("NUM", "3"),
    (0, 5, 1): ("ERR", "#DIV/0!"),
    (0, 5, 2): ("STR", "ab"),
    (0, 5, 3): ("BOOL", "1"),
    (0, 5, 4): ("NUM", "15"),
    (0, 5, 5): ("ERR", "#N/A"),
    (0, 5, 6): ("STR", ""),             # the empty-string result encoding
    (0, 6, 0): ("NUM", str(serial_1900("2026-08-01"))),
    (0, 6, 1): ("NUM", str(serial_1900("1997-01-16"))),
}

SMALL_SHEETS = [("Sheet1", [
    [("str", "name"), ("str", "qty"), ("str", "price")],
    [("str", "widget"),   ("num", 3.0),  ("num", 1.5)],
    [("str", "sprocket"), ("num", 12.0), ("num", 0.25)],
])]

LARGE_SHEETS = [("Sheet1", [
    [("str", "r%d" % i), ("num", float(i)), ("num", i * 1.5),
     ("str", "cell %d" % i)] for i in range(4000)
])]

def sst_sheets():
    return [("SST", [[("str", s)] for s in sst_strings()])]

# ---- the formula battery ----------------------------------------------------
# Each entry is (odf_formula, expected_excel_text).  The two are written
# independently on purpose: the source says it in ODF syntax (semicolon
# arguments, [.A1] references) and the fixture says it in Excel A1 syntax, so
# a match means the decompiler genuinely rebuilt the expression rather than
# echoing anything we handed it.
FML = [
    # precedence, and the parentheses the author actually typed
    ("=1+2*3",            "=1+2*3"),
    ("=(1+2)*3",          "=(1+2)*3"),
    ("=1-2-3",            "=1-2-3"),
    ("=1-(2-3)",          "=1-(2-3)"),      # equal precedence on the RIGHT
    ("=8/4/2",            "=8/4/2"),
    ("=8/(4/2)",          "=8/(4/2)"),
    ("=2^3^2",            "=2^3^2"),        # Excel's ^ is LEFT associative
    ("=2^(3^2)",          "=2^(3^2)"),
    ("=(1+2)&\"x\"",      "=(1+2)&\"x\""),
    ("=1+2&\"x\"",        "=1+2&\"x\""),    # & binds looser than +
    ("=50%",              "=50%"),
    ("=-[.B1]",           "=-B1"),
    ("=-[.B1]+3",         "=-B1+3"),
    # references in all four relative/absolute combinations
    ("=[.B1]",            "=B1"),
    ("=[.$B$1]",          "=$B$1"),
    ("=[.B$1]",           "=B$1"),
    ("=[.$B1]",           "=$B1"),
    ("=SUM([.B1:.C3])",   "=SUM(B1:C3)"),
    ("=SUM([.$B$1:.$C$3])", "=SUM($B$1:$C$3)"),
    # functions: fixed arity, variable arity, nesting, zero args
    ("=PI()",             "=PI()"),
    ("=ROUND(1.5;0)",     "=ROUND(1.5,0)"),
    ("=IF([.B1]>0;\"y\";\"n\")", "=IF(B1>0,\"y\",\"n\")"),
    ("=SUM(MAX(1;2);MIN(3;4))",  "=SUM(MAX(1,2),MIN(3,4))"),
    ("=CONCATENATE(\"a\";\"b\";\"c\")", "=CONCATENATE(\"a\",\"b\",\"c\")"),
    # literals: a doubled quote, a non-integer, a big one, an error
    ("=\"a\"\"b\"",       "=\"a\"\"b\""),
    ("=[.B1]*1.5",        "=B1*1.5"),
    ("=[.B1]*0.1",        "=B1*0.1"),
    ("=[.B1]*1000000",    "=B1*1000000"),
    # LibreOffice compiles TRUE() down to a single PtgBool constant - the
    # token stream for this cell is literally "1d 01", verified 2026-08-01
    # with an instrumented build - so the faithful Excel text is the bare
    # constant.  LibreOffice writes "TRUE()" when it reads the file back only
    # because ODF has no bare boolean literal; that is its syntax, not ours.
    ("=TRUE()",           "=TRUE"),
    ("=[.B1]<>[.C1]",     "=B1<>C1"),
    ("=[.B1]<=[.C1]",     "=B1<=C1"),
    # a 3-D reference into the other sheet
    ("=[$Other.A1]",      "=Other!A1"),
    ("=SUM([$Other.A1:.B2])", "=SUM(Other!A1:B2)"),
    # a defined name
    ("=SUM(theRange)",    "=SUM(theRange)"),
    # an array constant, which lives in rgbExtra after the token stream
    ("=SUM({1;2|3;4})",   "=SUM({1,2;3,4})"),
]

def formula_sheets():
    """Column A holds the battery; B and C hold operands so the references
    point at something real.  A separate block of identical fill-down
    formulas gives LibreOffice the chance to emit a SHRFMLA."""
    rows = []
    for i, (odf, _want) in enumerate(FML):
        row = [("num", 0.0, odf)]
        if i == 0:
            row += [("num", 2.0), ("num", 4.0)]
        rows.append(row)
    # A genuine fill-down: the SAME expression in every row, differing only
    # by the relative reference tracking the row.  This is the shape Excel
    # stores once as a SHRFMLA whose members carry nothing but a PtgExp.
    for i in range(12):
        rows.append([("num", 0.0, "=[.B%d]*2" % (len(FML) + i + 1))])
    return [("Formulas", rows), ("Other", [[("num", 7.0), ("num", 8.0)],
                                           [("num", 9.0), ("num", 10.0)]])]

def formula_expect():
    out = {}
    for i, (_odf, want) in enumerate(FML):
        out[(0, i, 0)] = want
    for i in range(12):
        out[(0, len(FML) + i, 0)] = "=B%d*2" % (len(FML) + i + 1)
    return out

FORMULA_NAMED = [("theRange", "$Formulas.$B$1:$Formulas.$C$3")]

# target basename -> (sheets, expectation overrides).  Every spreadsheet in
# the corpus carries a fixture derived from the SOURCE document, never from
# unodoc's own output.
def FIXTURES():
    # The SAME expectation serves both formats: small.xls and small.xlsx are
    # one spreadsheet saved twice, so two parsers reading to the same table is
    # the oracle that they agree. A difference is a bug in exactly one of them.
    fx = {
        "small.xls":    (SMALL_SHEETS, {}, None),
        "large.xls":    (LARGE_SHEETS, {}, None),
        "cells.xls":    (CELLS_SHEETS, CELLS_EXPECT, None),
        "sst.xls":      (sst_sheets(), {}, None),
        "formulas.xls": (formula_sheets(), {}, formula_expect()),
    }
    for name in list(fx):
        fx[name[:-4] + ".xlsx"] = fx[name]
    return fx

SOURCES = [
    # (source filename, builder, target extension)
    ("small.fodt", lambda: fodt(["Hello unodoc.", "Second paragraph."]), "doc"),
    ("large.fodt", lambda: fodt([("%04d " % i) + LOREM * 3 for i in range(900)]), "doc"),
    ("pic.fodt",   lambda: fodt(["A document with a picture."], png(320, 240, 7)), "doc"),
    ("fmt.fodt",   fmt_fodt, "doc"),
    ("small.fods", lambda: fods_sheets(SMALL_SHEETS), "xls"),
    ("large.fods", lambda: fods_sheets(LARGE_SHEETS), "xls"),
    ("cells.fods", lambda: fods_sheets(CELLS_SHEETS), "xls"),
    ("sst.fods",   lambda: fods_sheets(sst_sheets()), "xls"),
    ("formulas.fods", lambda: fods_sheets(formula_sheets(), FORMULA_NAMED), "xls"),
    ("small.fodp", lambda: fodp([("Slide one", ["alpha", "beta"]),
                                 ("Slide two", ["gamma"]),
                                 ("Slide three", [])]), "ppt"),
    ("pic.fodp",   lambda: fodp([("Pictures", ["with a big bitmap"]),
                                 ("Plain", ["no bitmap here"])],
                                png(900, 700, 11)), "ppt"),

    # ---- the 2007 formats -------------------------------------------------
    # The same sources again, saved through LibreOffice's OOXML filters. Same
    # documents, different container and different markup - which is what
    # makes the pair a real test rather than two tests: every fixture above is
    # reused verbatim for the .xlsx twin, so the two readers are checked
    # against each other as well as against the file.
    ("small.fodt", lambda: fodt(["Hello unodoc.", "Second paragraph."]), "docx"),
    ("large.fodt", lambda: fodt([("%04d " % i) + LOREM * 3 for i in range(900)]), "docx"),
    ("fmt.fodt",   fmt_fodt, "docx"),
    ("small.fods", lambda: fods_sheets(SMALL_SHEETS), "xlsx"),
    ("large.fods", lambda: fods_sheets(LARGE_SHEETS), "xlsx"),
    ("cells.fods", lambda: fods_sheets(CELLS_SHEETS), "xlsx"),
    ("sst.fods",   lambda: fods_sheets(sst_sheets()), "xlsx"),
    ("formulas.fods", lambda: fods_sheets(formula_sheets(), FORMULA_NAMED), "xlsx"),
    ("small.fodp", lambda: fodp([("Slide one", ["alpha", "beta"]),
                                 ("Slide two", ["gamma"]),
                                 ("Slide three", [])]), "pptx"),
]

def main():
    force = "--force" in sys.argv
    if force and os.path.isdir(CORPUS):
        shutil.rmtree(CORPUS)
    os.makedirs(SRC, exist_ok=True)
    os.makedirs(CORPUS, exist_ok=True)
    if not shutil.which("soffice"):
        raise SystemExit("soffice not on PATH - see this file's docstring")

    made = 0
    for name, build, ext in SOURCES:
        target = os.path.join(CORPUS, os.path.splitext(name)[0] + "." + ext)
        if os.path.exists(target) and not force:
            continue
        path = os.path.join(SRC, name)
        with open(path, "w", encoding="utf-8") as f:
            f.write(build())
        out = convert(path, ext, CORPUS)
        print("  %-12s %8d bytes" % (os.path.basename(out),
                                     os.path.getsize(out)))
        made += 1

    # fixtures are cheap and derived from the sources, so always rewrite them
    fx = FIXTURES()
    for base, (sheets, expect, fml) in fx.items():
        if not os.path.exists(os.path.join(CORPUS, base)):
            continue
        with open(os.path.join(CORPUS, base + ".expect.tsv"), "w",
                  encoding="utf-8") as f:
            f.write(fixture(sheets, expect, fml))
    print("corpus: %d file(s) generated, %d present, %d fixture(s)"
          % (made, len([f for f in os.listdir(CORPUS)
                        if not f.endswith(".expect.tsv")]), len(fx)))

if __name__ == "__main__":
    main()
