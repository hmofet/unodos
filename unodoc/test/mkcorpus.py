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
    'xmlns:xlink="http://www.w3.org/1999/xlink"'
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

def fods(rows):
    out = []
    for r in rows:
        cells = []
        for v in r:
            if isinstance(v, (int, float)):
                cells.append('<table:table-cell office:value-type="float" '
                             'office:value="%s"><text:p>%s</text:p>'
                             '</table:table-cell>' % (v, v))
            else:
                cells.append('<table:table-cell office:value-type="string">'
                             '<text:p>%s</text:p></table:table-cell>' % esc(v))
        out.append("<table:table-row>%s</table:table-row>" % "".join(cells))
    return ('<?xml version="1.0" encoding="UTF-8"?>\n'
            '<office:document %s office:version="1.2" '
            'office:mimetype="application/vnd.oasis.opendocument.spreadsheet">'
            '<office:body><office:spreadsheet>'
            '<table:table table:name="Sheet1">%s</table:table>'
            '</office:spreadsheet></office:body></office:document>'
            % (NS, "".join(out)))

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

SOURCES = [
    # (source filename, builder, target extension)
    ("small.fodt", lambda: fodt(["Hello unodoc.", "Second paragraph."]), "doc"),
    ("large.fodt", lambda: fodt([("%04d " % i) + LOREM * 3 for i in range(900)]), "doc"),
    ("pic.fodt",   lambda: fodt(["A document with a picture."], png(320, 240, 7)), "doc"),
    ("small.fods", lambda: fods([["name", "qty", "price"],
                                 ["widget", 3, 1.5], ["sprocket", 12, 0.25]]), "xls"),
    ("large.fods", lambda: fods([["r%d" % i, i, i * 1.5, "cell %d" % i]
                                 for i in range(4000)]), "xls"),
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
    print("corpus: %d file(s) generated, %d present"
          % (made, len(os.listdir(CORPUS))))

if __name__ == "__main__":
    main()
