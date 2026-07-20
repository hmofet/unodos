#!/usr/bin/env python3
"""Generate mp3_tables.h - the ISO/IEC 11172-3 constant tables Layer III needs.

WHY THIS SCRIPT EXISTS
----------------------
A Layer III decoder cannot invent its constants. The Huffman codebooks, the
512-tap polyphase synthesis window and the scalefactor-band boundaries are
specification data: they are fixed by ISO/IEC 11172-3 and every decoder in the
world contains the same numbers. What a decoder CAN be written from scratch is
everything else - the frame parser, bit reservoir, requantiser, stereo modes,
IMDCT and filterbank - and in UnoDOS that is exactly what dec_mp3.c is.

So this script sources only the constants, from PDMP3 by Krister Lagerstrom,
which is released into the PUBLIC DOMAIN (unlicense.org). No attribution is
required and none is legally owed; it is credited here because saying where
numbers came from is simply honest.

The Huffman codebooks are NOT copied in PDMP3's representation. PDMP3 stores
them as a pre-walked binary tree with byte offsets - an implementation choice.
This script walks those trees to recover the CANONICAL codebook the standard
actually specifies (code, length, value-pair) and re-emits it in the compact
per-length form dec_mp3.c wants. The output is therefore the ISO table, not
another program's data structure.

  python3 tools/mkmp3tables.py         regenerate mp3_tables.h

The generated header is committed, so ordinary builds never run this or touch
the network.
"""
import os, re, sys, urllib.request

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)

SRC_URL = "https://raw.githubusercontent.com/technosaurus/PDMP3/master/pdmp3.c"
CACHE = "tools/pdmp3_reference.c"
OUT = "mp3_tables.h"
MAXLEN = 19                     # longest Layer III Huffman code


def source():
    if not os.path.exists(CACHE):
        print("fetching the public-domain reference for the ISO tables...")
        urllib.request.urlretrieve(SRC_URL, CACHE)
    return open(CACHE, encoding="utf-8", errors="replace").read()


def nums(text, pattern, cast=int, base=0):
    m = re.search(pattern, text, re.S)
    if not m:
        sys.exit("could not locate %s in the reference" % pattern)
    body = m.group(1)
    body = re.sub(r"//[^\n]*", "", body)          # the reference comments rows
    out = []
    for tok in re.findall(r"[-+]?(?:0[xX][0-9a-fA-F]+|[0-9]*\.?[0-9]+(?:[eE][-+]?\d+)?)", body):
        out.append(cast(tok, base) if cast is int else cast(tok))
    return out


# ---- Huffman ---------------------------------------------------------------
def walk(tree, treelen):
    """Enumerate (code, length, x, y) for every leaf.

    Node encoding (PDMP3): a cell whose high byte is zero is a LEAF holding
    (x<<4)|y. Otherwise it is an internal node; bit 1 follows the low byte and
    bit 0 the high byte, each chained while the step is >= 250 (their escape
    for offsets that don't fit in a byte)."""
    out = []

    def step(point, byte_hi):
        v = tree[point]
        off = (v >> 8) if byte_hi else (v & 0xFF)
        while off >= 250:
            point += off
            v = tree[point]
            off = (v >> 8) if byte_hi else (v & 0xFF)
        return point + off

    def rec(point, code, length):
        if length > MAXLEN or point >= treelen:
            return
        v = tree[point]
        if (v & 0xFF00) == 0:                      # leaf
            out.append((code, length, (v >> 4) & 0xF, v & 0xF))
            return
        rec(step(point, True),  (code << 1),     length + 1)   # bit 0
        rec(step(point, False), (code << 1) | 1, length + 1)   # bit 1

    rec(0, 0, 0)
    return out


def compact(entries, tabno):
    """Group the codebook by code length, each group sorted by code value.

    Note these are NOT canonical Huffman codes: within a single length the code
    values are not consecutive (table 5 length 6 is the first counter-example),
    so the usual first-code/offset trick does not apply. Sorting by code within
    each length still allows a binary search, which is what dec_mp3.c does -
    a few comparisons per symbol, no tree and no allocation."""
    bylen = {}
    for code, ln, x, y in entries:
        bylen.setdefault(ln, []).append((code, x, y))
    cnt = [0] * (MAXLEN + 2)
    idx = [0] * (MAXLEN + 2)
    codes, syms = [], []
    for ln in sorted(bylen):
        rows = sorted(bylen[ln])
        cnt[ln] = len(rows)
        idx[ln] = len(codes)
        for code, x, y in rows:
            codes.append(code)
            syms.append((x << 4) | y)
    if len(codes) > 65535:
        sys.exit("table %d too large for the index type" % tabno)
    return cnt, idx, codes, syms


def main():
    text = source()

    body = re.search(r"g_huffman_table\[\]\s*=\s*\{(.*?)\n\};", text, re.S).group(1)
    tree = nums(text, r"g_huffman_table\[\]\s*=\s*\{(.*?)\n\};", int, 16)

    # Derive each sub-table's offset by ACCUMULATING the declared lengths in
    # the commented layout (`g_huffman_table_13[511] = {`), rather than
    # trusting the byte offsets in g_huffman_main. Those offsets disagree with
    # the data for table 33: it is listed at +2261, which lands inside the
    # shared tree of tables 24-31, and walking from there yields a single leaf
    # instead of 16 - so count1 table B silently decoded as garbage.
    starts, cur = {}, 0
    for name, ln in re.findall(r"g_huffman_table_(\d+)\[(\d+)\]", body):
        starts[int(name)] = (cur, int(ln))
        cur += int(ln)
    if cur != len(tree):
        sys.exit("sub-table lengths sum to %d but the array holds %d values"
                 % (cur, len(tree)))

    meta = re.findall(r"\{\s*(?:g_huffman_table\s*(?:\+\s*(\d+))?|NULL)\s*,\s*"
                      r"(\d+)\s*,\s*(\d+)\s*\}", text)
    if len(meta) < 34:
        sys.exit("expected 34 Huffman table descriptors, found %d" % len(meta))
    meta = meta[:34]

    tabs = []
    for i, (_off, treelen, linbits) in enumerate(meta):
        treelen = int(treelen)
        if treelen == 0:
            tabs.append(None)
            continue
        # tables 17-23 share sub-table 16 and 25-31 share 24, differing only in
        # linbits: take the highest declared sub-table at or below this index
        key = max(k for k in starts if k <= i)
        off, ln = starts[key]
        entries = walk(tree[off:off + ln], ln)
        tabs.append(compact(entries, i) + (int(linbits),))

    sfb = nums(text, r"g_sf_band_indices\[3[^\]]*\]\s*=\s*\{(.*?)\n\s*\};", int, 10)
    if len(sfb) != 3 * (23 + 14):
        sys.exit("scalefactor-band table came out as %d values" % len(sfb))

    synth = nums(text, r"g_synth_dtbl\[512\]\s*=\s*\{(.*?)\n\};", float)
    if len(synth) != 512:
        sys.exit("synthesis window came out as %d values" % len(synth))

    with open(OUT, "w") as f:
        f.write('''/* Generated by tools/mkmp3tables.py - do not edit.
 *
 * ISO/IEC 11172-3 (MPEG-1 Layer III) constant tables: Huffman codebooks, the
 * polyphase synthesis window, and the scalefactor-band boundaries. These are
 * specification data - fixed by the standard, identical in every decoder - and
 * the only part of dec_mp3.c that is not written from scratch. Recovered from
 * the PUBLIC DOMAIN PDMP3 reference (unlicense.org); no attribution is owed.
 *
 * The Huffman codebooks are grouped by code length, sorted by code value:
 *   cnt[L]  how many codes have length L
 *   idx[L]  where that length's run starts in codes[]/syms[]
 * Layer III's codes are NOT canonical - within one length the values are not
 * consecutive - so decoding accumulates bits into `code` and binary-searches
 * that length's run. Symbols pack the value pair as (x<<4)|y.
 */
#ifndef PC64_MP3_TABLES_H
#define PC64_MP3_TABLES_H

#define MP3_HUFF_MAXLEN %d

typedef struct {
    const unsigned char  *cnt;      /* [MP3_HUFF_MAXLEN+1]                  */
    const unsigned short *idx;      /* [MP3_HUFF_MAXLEN+1]                  */
    const unsigned short *codes;    /* sorted within each length            */
    const unsigned char  *syms;     /* (x<<4)|y, parallel to codes[]        */
    unsigned char linbits;
} mp3_htab;

''' % MAXLEN)

        for i, t in enumerate(tabs):
            if not t:
                continue
            cnt, idx, codes, syms, lin = t
            f.write("static const unsigned char  hc%d[]  = {%s};\n"
                    % (i, ",".join(str(c) for c in cnt[:MAXLEN + 1])))
            f.write("static const unsigned short hi%d[]  = {%s};\n"
                    % (i, ",".join(str(c) for c in idx[:MAXLEN + 1])))
            f.write("static const unsigned short hk%d[]  = {%s};\n"
                    % (i, ",".join(str(c) for c in codes)))
            f.write("static const unsigned char  hs%d[]  = {%s};\n\n"
                    % (i, ",".join(str(s) for s in syms)))

        f.write("static const mp3_htab kHuff[34] = {\n")
        for i, t in enumerate(tabs):
            if not t:
                f.write("    { 0, 0, 0, 0, 0 },\n")
            else:
                f.write("    { hc%d, hi%d, hk%d, hs%d, %d },\n"
                        % (i, i, i, i, t[4]))
        f.write("};\n\n")

        f.write("/* scalefactor band boundaries, per MPEG-1 sampling rate\n"
                " * (0 = 44100, 1 = 48000, 2 = 32000) */\n")
        f.write("static const unsigned short kSfbLong[3][23] = {\n")
        for r in range(3):
            row = sfb[r * 37:r * 37 + 23]
            f.write("    {%s},\n" % ",".join(str(v) for v in row))
        f.write("};\n")
        f.write("static const unsigned short kSfbShort[3][14] = {\n")
        for r in range(3):
            row = sfb[r * 37 + 23:r * 37 + 37]
            f.write("    {%s},\n" % ",".join(str(v) for v in row))
        f.write("};\n\n")

        f.write("/* the 512-tap polyphase synthesis window (ISO Table B.3) */\n")
        f.write("static const float kSynthWin[512] = {\n")
        for i in range(0, 512, 6):
            f.write("    " + ",".join("%.10ff" % v for v in synth[i:i + 6]) + ",\n")
        f.write("};\n\n#endif\n")

    used = sum(1 for t in tabs if t)
    print("wrote %s: %d Huffman tables, %d sfb rows, %d window taps"
          % (OUT, used, 3, len(synth)))


if __name__ == "__main__":
    main()
