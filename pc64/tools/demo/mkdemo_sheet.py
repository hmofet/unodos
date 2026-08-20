#!/usr/bin/env python3
"""Build the demo's spreadsheet: a small hardware budget, as a real .xls.

The office scene opened `formulas.xls`, which is unodoc's TEST FIXTURE: a
column of one-cell expressions with operands beside them, because its job is
to make the decompiler rebuild every formula shape BIFF8 can store. It proves
the reader works and it tells a viewer nothing.

This is the same idea as mkdemo_doc.py's CV. A quantity times a unit price on
every line, a SUM over the lines, tax off the subtotal and a total that adds
the two - so clicking any of the last three shows a formula in the formula bar
that a person can read and check in their head.

    python3 mkdemo_sheet.py [--out DIR] [--force]

Needs `soffice` on PATH. The cell shapes below are unodoc/test/mkcorpus.py's
(office:value-type + table:formula in `of:` syntax, with a cached value): the
value is what a reader shows before it recalculates, and a cell with a formula
and no cached value reads as empty everywhere until something recomputes it.
"""
import argparse, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, "assets")
PROFILE = os.path.join(HERE, "out", "loprofile")

NS = (
    'xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
    'xmlns:table="urn:oasis:names:tc:opendocument:xmlns:table:1.0" '
    'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
    'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
    'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0" '
    'xmlns:of="urn:oasis:names:tc:opendocument:xmlns:of:1.2"'
)

ITEMS = [
    ("Development boards", 6, 89.00),
    ("Logic analyser", 1, 415.00),
    ("Oscilloscope probes", 4, 62.50),
    ("USB serial adapters", 8, 17.25),
    ("Cables and connectors", 1, 143.80),
]
TAX = 0.13


def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def cell(kind, val, formula=None, style=None):
    attr = ' table:formula="of:%s"' % esc(formula) if formula else ""
    if style:
        attr += ' table:style-name="%s"' % style
    if kind == "str":
        return ('<table:table-cell%s office:value-type="string">'
                '<text:p>%s</text:p></table:table-cell>' % (attr, esc(val)))
    if kind == "empty":
        return "<table:table-cell%s/>" % attr
    return ('<table:table-cell%s office:value-type="float" office:value="%r">'
            '<text:p>%r</text:p></table:table-cell>' % (attr, val, val))


def sheet_xml():
    rows = []
    rows.append([cell("str", "Q3 hardware budget", None, "HEAD"),
                 cell("empty", None), cell("empty", None), cell("empty", None)])
    rows.append([cell("str", "Item", None, "HEAD"), cell("str", "Qty", None, "HEAD"),
                 cell("str", "Unit", None, "HEAD"), cell("str", "Line total", None, "HEAD")])
    first = 3                                  # 1-based row of the first item
    for i, (name, qty, unit) in enumerate(ITEMS):
        r = first + i
        rows.append([cell("str", name), cell("num", float(qty)),
                     cell("num", unit),
                     cell("num", qty * unit, "[.B%d]*[.C%d]" % (r, r))])
    last = first + len(ITEMS) - 1
    sub = sum(q * u for _n, q, u in ITEMS)
    rows.append([cell("empty", None)] * 4)
    sub_row = last + 2
    rows.append([cell("str", "Subtotal", None, "HEAD"), cell("empty", None),
                 cell("empty", None),
                 cell("num", sub, "SUM([.D%d:.D%d])" % (first, last))])
    rows.append([cell("str", "Tax at 13%", None, "HEAD"), cell("empty", None),
                 cell("empty", None),
                 cell("num", sub * TAX, "[.D%d]*%s" % (sub_row, TAX))])
    rows.append([cell("str", "Total", None, "HEAD"), cell("empty", None),
                 cell("empty", None),
                 cell("num", sub * (1 + TAX),
                      "[.D%d]+[.D%d]" % (sub_row, sub_row + 1))])
    body = "".join("<table:table-row>%s</table:table-row>" % "".join(r)
                   for r in rows)
    styles = ('<style:style style:name="HEAD" style:family="table-cell">'
              '<style:text-properties fo:font-weight="bold"/></style:style>')
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<office:document %s office:version="1.2" '
            'office:mimetype="application/vnd.oasis.opendocument.spreadsheet">'
            '<office:automatic-styles>%s</office:automatic-styles>'
            '<office:body><office:spreadsheet>'
            '<table:table table:name="Budget">%s</table:table>'
            '</office:spreadsheet></office:body></office:document>'
            % (NS, styles, body))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    dst = os.path.join(args.out, "budget.xls")
    if os.path.exists(dst) and not args.force:
        print("budget.xls exists (use --force to rebuild):", dst)
        return 0
    if not shutil.which("soffice"):
        print("soffice not on PATH - install libreoffice-calc")
        return 1

    src = os.path.join(args.out, "budget.fods")
    with open(src, "w", encoding="utf-8", newline="\n") as f:
        f.write(sheet_xml())
    os.makedirs(PROFILE, exist_ok=True)
    r = subprocess.run(["soffice", "-env:UserInstallation=file://" + PROFILE,
                        "--headless", "--convert-to", "xls", "--outdir",
                        args.out, src],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0 or not os.path.exists(dst):
        print(r.stdout.decode("utf-8", "replace"))
        return 1
    print("budget.xls  %d bytes  ->  %s" % (os.path.getsize(dst), dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
