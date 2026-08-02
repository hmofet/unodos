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

def fods_sheets(sheets):
    """sheets = [(name, rows)] where rows = [[cell, ...], ...]"""
    out = []
    for name, rows in sheets:
        body = []
        for row in rows:
            body.append("<table:table-row>%s</table:table-row>"
                        % "".join(cell_xml(c) for c in row))
        out.append('<table:table table:name="%s">%s</table:table>'
                   % (esc(name), "".join(body)))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<office:document %s office:version="1.2" '
            'office:mimetype="application/vnd.oasis.opendocument.spreadsheet">'
            '%s<office:body><office:spreadsheet>%s</office:spreadsheet>'
            '</office:body></office:document>' % (NS, DATE_STYLE, "".join(out)))

def fixture(sheets, expect):
    """expect = {(sheet, row, col): (KIND, value)} overriding the literal
    cells; everything else is taken from the source document itself."""
    lines = []
    for si, (_name, rows) in enumerate(sheets):
        for ri, row in enumerate(rows):
            for ci, c in enumerate(row):
                key = (si, ri, ci)
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

# target basename -> (sheets, expectation overrides).  Every spreadsheet in
# the corpus carries a fixture derived from the SOURCE document, never from
# unodoc's own output.
def FIXTURES():
    return {
        "small.xls": (SMALL_SHEETS, {}),
        "large.xls": (LARGE_SHEETS, {}),
        "cells.xls": (CELLS_SHEETS, CELLS_EXPECT),
        "sst.xls":   (sst_sheets(), {}),
    }

SOURCES = [
    # (source filename, builder, target extension)
    ("small.fodt", lambda: fodt(["Hello unodoc.", "Second paragraph."]), "doc"),
    ("large.fodt", lambda: fodt([("%04d " % i) + LOREM * 3 for i in range(900)]), "doc"),
    ("pic.fodt",   lambda: fodt(["A document with a picture."], png(320, 240, 7)), "doc"),
    ("small.fods", lambda: fods_sheets(SMALL_SHEETS), "xls"),
    ("large.fods", lambda: fods_sheets(LARGE_SHEETS), "xls"),
    ("cells.fods", lambda: fods_sheets(CELLS_SHEETS), "xls"),
    ("sst.fods",   lambda: fods_sheets(sst_sheets()), "xls"),
    ("small.fodp", lambda: fodp([("Slide one", ["alpha", "beta"]),
                                 ("Slide two", ["gamma"]),
                                 ("Slide three", [])]), "ppt"),
    ("pic.fodp",   lambda: fodp([("Pictures", ["with a big bitmap"]),
                                 ("Plain", ["no bitmap here"])],
                                png(900, 700, 11)), "ppt"),
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
    for base, (sheets, expect) in fx.items():
        if not os.path.exists(os.path.join(CORPUS, base)):
            continue
        with open(os.path.join(CORPUS, base + ".expect.tsv"), "w",
                  encoding="utf-8") as f:
            f.write(fixture(sheets, expect))
    print("corpus: %d file(s) generated, %d present, %d fixture(s)"
          % (made, len([f for f in os.listdir(CORPUS)
                        if not f.endswith(".expect.tsv")]), len(fx)))

if __name__ == "__main__":
    main()
