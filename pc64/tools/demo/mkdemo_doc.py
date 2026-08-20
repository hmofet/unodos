#!/usr/bin/env python3
"""Build the demo's Word document: a one-page CV, as a real .doc.

The office scene used to open `fmt.doc`, which is unodoc's TEST FIXTURE - its
text is literally "a BOLDWORD z" and "CENTREPARA", because its job is to carry
one of every formatting property for a parser to check. It proves the loader
works and it tells a viewer nothing, so this generates a document a person
would recognise instead, using the same formatting the fixture tests: a name
in large bold, right-aligned contact details, section headings, italic dates
and justified body text.

Nothing here is a real person. "Robin Vale" is invented, and so is every
employer, school and date in it.

    python3 mkdemo_doc.py [--out DIR] [--force]

Needs `soffice` on PATH (the same dependency unodoc/test/mkcorpus.py has, and
the same one-way trip: .fodt is written here, LibreOffice converts it to the
binary .doc that UnoWord opens).

THE NAMESPACE TRAP, inherited from mkcorpus.py and worth repeating because it
fails silently: `fo:` carries font-weight, font-size, text-align and the
indents. Leave it undeclared and LibreOffice does not complain - it parses the
file, drops every fo: attribute, and writes a perfectly valid .doc with none
of the formatting in it. That is how the old fixture shipped for months
advertising formatting it did not contain.
"""
import argparse, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(HERE, "assets")
PROFILE = os.path.join(HERE, "out", "loprofile")

NS = (
    'xmlns:office="urn:oasis:names:tc:opendocument:xmlns:office:1.0" '
    'xmlns:text="urn:oasis:names:tc:opendocument:xmlns:text:1.0" '
    'xmlns:style="urn:oasis:names:tc:opendocument:xmlns:style:1.0" '
    'xmlns:fo="urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0"'
)

# (style name, text properties, paragraph properties)
STYLES = [
    ("NAME",    'fo:font-size="22pt" fo:font-weight="bold"', ""),
    ("CONTACT", 'fo:font-size="10pt"', 'fo:text-align="end"'),
    ("HEAD",    'fo:font-size="13pt" fo:font-weight="bold" '
                'fo:text-transform="uppercase"', 'fo:margin-top="0.4cm"'),
    ("ROLE",    'fo:font-weight="bold"', ""),
    ("WHEN",    'fo:font-style="italic" fo:font-size="10pt"', ""),
    ("BODY",    "", 'fo:text-align="justify"'),
    ("BULLET",  "", ""),
]

DOC = [
    ("NAME",    "Robin Vale"),
    ("CONTACT", "robin.vale@example.com  -  +1 555 0142  -  Portland, Oregon"),
    ("HEAD",    "Summary"),
    ("BODY",    "Systems engineer with eleven years on operating systems, "
                "device drivers and the unglamorous parts underneath them. "
                "Happiest where the documentation runs out."),
    ("HEAD",    "Experience"),
    ("ROLE",    "Principal Engineer, Kestrel Systems"),
    ("WHEN",    "2019 to present"),
    ("BULLET",  "Led the storage stack rewrite: one driver model across four "
                "controller families, and a test rig that boots each of them "
                "nightly."),
    ("BULLET",  "Cut cold boot from 14 seconds to 3 by moving device probing "
                "off the critical path."),
    ("ROLE",    "Senior Engineer, Halden Instruments"),
    ("WHEN",    "2014 to 2019"),
    ("BULLET",  "Wrote the firmware for a handheld analyser shipped in 30,000 "
                "units, in 64 kilobytes of flash."),
    ("HEAD",    "Education"),
    ("ROLE",    "BSc Computer Science, University of Leeds"),
    ("WHEN",    "2010 to 2013"),
    ("HEAD",    "Skills"),
    ("BODY",    "C, assembly for three architectures, Python, hardware "
                "bring-up, protocol design, and writing the document that "
                "explains it to whoever comes next."),
]


def fodt():
    styles = []
    for i, (_n, tprops, pprops) in enumerate(STYLES):
        styles.append(
            '<style:style style:name="P%d" style:family="paragraph">'
            '<style:paragraph-properties %s/>'
            '<style:text-properties %s/></style:style>' % (i, pprops, tprops))
    idx = dict((n, i) for i, (n, _t, _p) in enumerate(STYLES))
    body = []
    for style, text in DOC:
        body.append('<text:p text:style-name="P%d">%s</text:p>'
                    % (idx[style], text.replace("&", "&amp;").replace("<", "&lt;")))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<office:document %s office:version="1.2" '
            'office:mimetype="application/vnd.oasis.opendocument.text">'
            '<office:automatic-styles>%s</office:automatic-styles>'
            '<office:body><office:text>%s</office:text></office:body>'
            '</office:document>' % (NS, "".join(styles), "".join(body)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    dst = os.path.join(args.out, "resume.doc")
    if os.path.exists(dst) and not args.force:
        print("resume.doc exists (use --force to rebuild):", dst)
        return 0
    if not shutil.which("soffice"):
        print("soffice not on PATH - install libreoffice-writer")
        return 1

    src = os.path.join(args.out, "resume.fodt")
    with open(src, "w", encoding="utf-8", newline="\n") as f:
        f.write(fodt())
    os.makedirs(PROFILE, exist_ok=True)
    r = subprocess.run(["soffice", "-env:UserInstallation=file://" + PROFILE,
                        "--headless", "--convert-to", "doc", "--outdir",
                        args.out, src],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if r.returncode != 0 or not os.path.exists(dst):
        print(r.stdout.decode("utf-8", "replace"))
        return 1
    print("resume.doc  %d bytes  ->  %s" % (os.path.getsize(dst), dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
