#!/usr/bin/env python3
"""Generate aac_tables.h - the ISO/IEC 13818-7 / 14496-3 constant tables an
AAC-LC decoder needs.

WHY THIS SCRIPT EXISTS
----------------------
Like MP3 (see mkmp3tables.py), an AAC decoder cannot invent its constants.
The 11 spectral Huffman codebooks, the scalefactor codebook and the
scalefactor-band offset tables per sampling rate are specification data:
fixed by the standard, identical in every decoder on earth. Everything else
in um_aac.c - the ADTS/MP4 container layer, the raw_data_block parser,
section and scalefactor decoding, pulse, TNS, M/S, intensity, PNS, the
IMDCT and the filterbank - is this project's own code, written from scratch
against the spec.

WHERE THE CONSTANTS COME FROM (read AUDIO.md's AAC section for the history)
---------------------------------------------------------------------------
MP3's tables had a public-domain source (PDMP3). AAC has none; the only
permissively licensed implementation is OpenCORE aacdec, Apache License 2.0,
Copyright (C) 1998-2009 PacketVideo. That licence permits reuse WITH
attribution, and the project decided (reversing the earlier refusal recorded
in AUDIO.md) to accept an Apache-2.0-derived table file. The obligations are
met like this:

  - the generated aac_tables.h reproduces the PacketVideo copyright and the
    Apache-2.0 notice (the full licence text is unomedia/LICENSE.APACHE-2.0);
  - this script documents the exact upstream files and commit;
  - only CONSTANTS derive from OpenCORE - no decode code does.

EXACT PROVENANCE
----------------
Repository: https://github.com/aosp-mirror/platform_external_opencore
Commit:     61bf9af643abf0011dcf82ae8a436aeb7e8aae97  (branch master)
Files (under codecs_v2/audio/aac/dec/src/):
  hcbtables_binary.cpp        the 12 packed Huffman lookup tables
                              (huff_tab1..huff_tab11, huff_tab_scl)
  decode_huff_cw_binary.cpp   the partition logic that indexes them
                              (transcribed below as BOOKS, used to UNDO the
                              packing - see next section)
  sfb.cpp                     scalefactor-band offset tables per rate
                              (sfb_96_1024 .. sfb_8_128) and the per-rate
                              band counts (samp_rate_info); the rate-index ->
                              table mapping follows infoinit.cpp

HOW THE PACKING IS UNDONE
-------------------------
OpenCORE stores each codebook as a flat array indexed by a left-justified
codeword prefix: every codeword is left-shifted to the book's maximum length,
the shifted values are sorted and partitioned into runs decoded by an if/else
ladder (decode_huff_cw_binary.cpp), and each array entry packs
(symbol << 16) | code_length. That is an implementation-specific layout. To
recover the CANONICAL codebook the standard specifies - (codeword, length,
symbol) triples - this script enumerates every possible max-length bit input,
runs it through the transcribed partition ladder, and reads back the symbol
and true length; the codeword is the input's top `length` bits. The result is
verified three ways before anything is written:

  - every (length, codeword) pair must map to exactly one symbol,
  - the symbol set must be exactly 0..N-1 with the ISO codebook's N
    (81/81/81/81/81/81/64/64/169/169/289 spectral, 121 scalefactor),
  - the code must be complete and prefix-free: Kraft sum == 1 exactly.

The output representation is ours (the same per-length grouped, binary-
searchable layout mp3_tables.h uses); the VALUES are OpenCORE's, which is
the point - they are the ISO tables.

  python3 tools/mkaactables.py         regenerate aac_tables.h

The generated header is committed, so ordinary builds never run this or
touch the network. Upstream sources are cached in tools/opencore_ref/.
"""
import os, re, sys, urllib.request
from fractions import Fraction

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(HERE)

COMMIT = "61bf9af643abf0011dcf82ae8a436aeb7e8aae97"
BASE = ("https://raw.githubusercontent.com/aosp-mirror/"
        "platform_external_opencore/%s/codecs_v2/audio/aac/dec/src/" % COMMIT)
FILES = ["hcbtables_binary.cpp", "decode_huff_cw_binary.cpp", "sfb.cpp"]
CACHEDIR = "tools/opencore_ref"
OUT = "aac_tables.h"
MAXLEN = 19                     # longest AAC Huffman code (scalefactor book)

# ---------------------------------------------------------------------------
# The partition ladders, transcribed from decode_huff_cw_binary.cpp.
#
# Each book reads `nbits` bits (the book's maximum code length). `direct` are
# the special-cased shortest codes: (shift, value, symbol, length) means
# "if (cw >> shift) == value, the symbol is `symbol` and only `length` bits
# were consumed". `parts` are the ladder rungs in order: (shift, limit, sub,
# add) means "else if (cw >> shift) <= limit: index = (cw >> shift)-sub+add".
# The indexed entry packs (symbol << 16) | length.
# ---------------------------------------------------------------------------
BOOKS = {
    1:  dict(nbits=11, direct=[(10, 0, 40, 1)],
             parts=[(6, 23, 16, 0), (4, 119, 96, 8), (2, 503, 480, 32),
                    (0, 2047, 2016, 56)]),
    2:  dict(nbits=9,  direct=[(6, 0, 40, 3)],
             parts=[(3, 49, 8, 0), (2, 114, 100, 42), (1, 248, 230, 57),
                    (0, 511, 498, 76)]),
    3:  dict(nbits=16, direct=[(15, 0, 0, 1)],
             parts=[(10, 57, 32, 0), (7, 500, 464, 26), (6, 1016, 1002, 63),
                    (4, 4092, 4068, 78), (0, 65535, 65488, 103)]),
    4:  dict(nbits=12, direct=[],
             parts=[(7, 25, 0, 0), (4, 246, 208, 26), (2, 1017, 988, 65),
                    (0, 4095, 4072, 95)]),
    5:  dict(nbits=13, direct=[(12, 0, 40, 1)],
             parts=[(8, 27, 16, 0), (5, 243, 224, 12), (3, 1011, 976, 32),
                    (2, 2041, 2024, 68), (0, 8191, 8168, 86)]),
    6:  dict(nbits=11, direct=[],
             parts=[(7, 8, 0, 0), (4, 116, 72, 9), (2, 506, 468, 54),
                    (0, 2047, 2028, 93)]),
    7:  dict(nbits=12, direct=[(11, 0, 0, 1)],
             parts=[(6, 55, 32, 0), (4, 243, 224, 24), (2, 1018, 976, 44),
                    (0, 4095, 4076, 87)]),
    8:  dict(nbits=10, direct=[],
             parts=[(5, 20, 0, 0), (3, 117, 84, 21), (2, 250, 236, 55),
                    (0, 1023, 1004, 70)]),
    9:  dict(nbits=15, direct=[],
             parts=[(11, 12, 0, 0), (8, 114, 104, 13), (6, 486, 460, 24),
                    (5, 993, 974, 51), (4, 2018, 1988, 71),
                    (3, 4075, 4038, 102), (2, 8183, 8152, 140),
                    (0, 32767, 32736, 172)]),
    10: dict(nbits=12, direct=[],
             parts=[(6, 41, 0, 0), (5, 100, 84, 42), (4, 226, 202, 59),
                    (3, 484, 454, 84), (2, 1010, 970, 115),
                    (1, 2043, 2022, 156), (0, 4095, 4088, 178)]),
    11: dict(nbits=12, direct=[],
             parts=[(6, 26, 0, 0), (5, 69, 54, 27), (4, 198, 140, 43),
                    (3, 452, 398, 102), (2, 1000, 906, 157),
                    (1, 2044, 2002, 252), (0, 4095, 4090, 295)]),
    0:  dict(nbits=19, direct=[(18, 0, 60, 1)],   # 0 = the scalefactor book
             parts=[(13, 59, 32, 0), (10, 505, 480, 28), (7, 4089, 4048, 54),
                    (5, 16377, 16360, 96), (3, 65526, 65512, 114),
                    (1, 262120, 262108, 129), (0, 524287, 524242, 142)]),
}

TABNAME = {0: "huff_tab_scl"}
for _i in range(1, 12):
    TABNAME[_i] = "huff_tab%d" % _i

# ISO codebook sizes: books 1-4 are quads over 3 values (3^4), 5-6 pairs over
# 9 values (9^2), 7-8 pairs over 8 (8^2), 9-10 pairs over 13 (13^2), 11 pairs
# over 17 (17^2, value 16 = ESC), scalefactor book 121 values (-60..+60).
NSYMS = {1: 81, 2: 81, 3: 81, 4: 81, 5: 81, 6: 81, 7: 64, 8: 64,
         9: 169, 10: 169, 11: 289, 0: 121}

# rate index (AudioSpecificConfig, 0..12) -> which sfb tables, per
# infoinit.cpp (96/88.2 share, 48/44.1 share, 32 long is its own but shares
# the 48 short table, etc; 7350 uses the 8000 tables).
SFB_MAP = [
    ("sfb_96_1024", "sfb_64_128"),   # 0  96000
    ("sfb_96_1024", "sfb_64_128"),   # 1  88200
    ("sfb_64_1024", "sfb_64_128"),   # 2  64000
    ("sfb_48_1024", "sfb_48_128"),   # 3  48000
    ("sfb_48_1024", "sfb_48_128"),   # 4  44100
    ("sfb_32_1024", "sfb_48_128"),   # 5  32000
    ("sfb_24_1024", "sfb_24_128"),   # 6  24000
    ("sfb_24_1024", "sfb_24_128"),   # 7  22050
    ("sfb_16_1024", "sfb_16_128"),   # 8  16000
    ("sfb_16_1024", "sfb_16_128"),   # 9  12000
    ("sfb_16_1024", "sfb_16_128"),   # 10 11025
    ("sfb_8_1024",  "sfb_8_128"),    # 11 8000
    ("sfb_8_1024",  "sfb_8_128"),    # 12 7350
]


def source(name):
    os.makedirs(CACHEDIR, exist_ok=True)
    path = os.path.join(CACHEDIR, name)
    if not os.path.exists(path):
        print("fetching %s @ %s ..." % (name, COMMIT[:12]))
        urllib.request.urlretrieve(BASE + name, path)
    return open(path, encoding="utf-8", errors="replace").read()


def parse_array(text, name):
    m = re.search(r"%s\s*\[\s*\d*\s*\]\s*=\s*\{(.*?)\};" % name, text, re.S)
    if not m:
        sys.exit("could not locate %s in the reference" % name)
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    body = re.sub(r"//[^\n]*", "", body)
    return [int(tok, 0) for tok in
            re.findall(r"[-+]?(?:0[xX][0-9a-fA-F]+|\d+)", body)]


def recover_book(book, table):
    """Undo the packing: enumerate every nbits input through the transcribed
    partition ladder and collect the canonical (length, codeword) -> symbol
    map."""
    spec = BOOKS[book]
    nbits = spec["nbits"]
    seen = {}
    for cw in range(1 << nbits):
        sym = ln = None
        for shift, val, s, l in spec["direct"]:
            if (cw >> shift) == val:
                sym, ln = s, l
                break
        if sym is None:
            lo = 0 if not spec["direct"] else None
            for shift, limit, sub, add in spec["parts"]:
                v = cw >> shift
                if v <= limit:
                    idx = v - sub + add
                    if idx < 0 or idx >= len(table):
                        sys.exit("book %d: index %d out of range" % (book, idx))
                    ent = table[idx]
                    sym, ln = ent >> 16, ent & 0xFFFF
                    break
            del lo
        if sym is None or ln is None or not 1 <= ln <= nbits:
            sys.exit("book %d: input %d unresolved" % (book, cw))
        code = cw >> (nbits - ln)
        key = (ln, code)
        if key in seen and seen[key] != sym:
            sys.exit("book %d: (len %d, code %d) maps to both %d and %d"
                     % (book, ln, code, seen[key], sym))
        seen[key] = sym
    # verify: symbol set, completeness (Kraft == 1), prefix-freedom
    syms = sorted(set(seen.values()))
    if syms != list(range(NSYMS[book])):
        sys.exit("book %d: recovered %d symbols, expected %d"
                 % (book, len(syms), NSYMS[book]))
    if sum(Fraction(1, 1 << ln) for (ln, _c) in seen) != 1:
        sys.exit("book %d: code is not complete (Kraft sum != 1)" % book)
    codes = sorted(seen)                       # (len, code) ascending
    for i in range(len(codes) - 1):
        l1, c1 = codes[i]
        for l2, c2 in codes[i + 1:]:
            if l2 > l1 and (c2 >> (l2 - l1)) == c1:
                sys.exit("book %d: code is not prefix-free" % book)
            if l2 == l1:
                break
    return seen


def compact(seen):
    """Group by code length, sorted by code value, for binary search - the
    same layout mp3_tables.h uses."""
    bylen = {}
    for (ln, code), sym in seen.items():
        bylen.setdefault(ln, []).append((code, sym))
    cnt = [0] * (MAXLEN + 1)
    idx = [0] * (MAXLEN + 1)
    codes, syms = [], []
    for ln in sorted(bylen):
        rows = sorted(bylen[ln])
        cnt[ln] = len(rows)
        idx[ln] = len(codes)
        for code, sym in rows:
            codes.append(code)
            syms.append(sym)
    return cnt, idx, codes, syms


def main():
    hcb = source("hcbtables_binary.cpp")
    source("decode_huff_cw_binary.cpp")        # cached for provenance
    sfbsrc = source("sfb.cpp")

    books = {}
    for b in sorted(BOOKS):
        table = parse_array(hcb, TABNAME[b])
        books[b] = compact(recover_book(b, table))

    sfb = {}
    for name in sorted(set(n for pair in SFB_MAP for n in pair)):
        vals = parse_array(sfbsrc, name)
        if vals[-1] not in (1024, 128) or vals != sorted(vals):
            sys.exit("%s does not look like a band-offset table" % name)
        sfb[name] = vals
    # cross-check band counts against sfb.cpp's samp_rate_info comments
    info = parse_array(sfbsrc, "samp_rate_info")
    rates = info[0::3]
    for i, (ln_t, sh_t) in enumerate(SFB_MAP[:12]):
        if len(sfb[ln_t]) != info[i * 3 + 1] or len(sfb[sh_t]) != info[i * 3 + 2]:
            sys.exit("rate %d: band counts disagree with samp_rate_info"
                     % rates[i])

    with open(OUT, "w") as f:
        f.write('''\
/* Generated by tools/mkaactables.py - do not edit.
 *
 * ISO/IEC 13818-7 / 14496-3 (AAC) constant tables: the 11 spectral Huffman
 * codebooks, the scalefactor codebook, and the scalefactor-band offset
 * tables per sampling rate. These are specification data - fixed by the
 * standard, identical in every decoder - and the only part of um_aac.c that
 * is not this project's own work.
 *
 * The table VALUES derive from OpenCORE aacdec (files hcbtables_binary.cpp,
 * decode_huff_cw_binary.cpp and sfb.cpp under codecs_v2/audio/aac/dec/src/,
 * commit 61bf9af643abf0011dcf82ae8a436aeb7e8aae97 of
 * https://github.com/aosp-mirror/platform_external_opencore), unpacked from
 * that implementation's prefix-partition layout back to the canonical
 * (codeword, length, symbol) form the standard specifies and re-grouped by
 * code length; mkaactables.py documents and verifies the derivation.
 * Everything else in um_aac.c - container parsing, bitstream decode, TNS,
 * stereo tools, PNS, IMDCT, filterbank - is written from scratch here and is
 * NOT derived from OpenCORE.
 *
 * As the source's licence requires, its notice is reproduced:
 *
 *   Copyright (C) 1998-2009 PacketVideo
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 *   implied. See the License for the specific language governing
 *   permissions and limitations under the License.
 *
 * (The full licence text is unomedia/LICENSE.APACHE-2.0.)
 *
 * Layout (same scheme as mp3_tables.h): each codebook is grouped by code
 * length, sorted by code value within a length:
 *   cnt[L]  how many codes have length L
 *   idx[L]  where that length's run starts in codes[]/syms[]
 * Decoding accumulates bits into `code` and binary-searches the current
 * length's run. Symbols are the ISO codebook indices; um_aac.c decomposes
 * them into value pairs/quads arithmetically.
 */
#ifndef UNOMEDIA_AAC_TABLES_H
#define UNOMEDIA_AAC_TABLES_H

#define AAC_HUFF_MAXLEN %d

typedef struct {
    const unsigned char  *cnt;      /* [AAC_HUFF_MAXLEN+1]                  */
    const unsigned short *idx;      /* [AAC_HUFF_MAXLEN+1]                  */
    const unsigned int   *codes;    /* sorted within each length            */
    const unsigned short *syms;     /* ISO codebook index, parallel        */
} aac_htab;

''' % MAXLEN)

        def emit_book(tag, t):
            cnt, idx, codes, syms = t
            f.write("static const unsigned char  ac%s[] = {%s};\n"
                    % (tag, ",".join(str(v) for v in cnt)))
            f.write("static const unsigned short ai%s[] = {%s};\n"
                    % (tag, ",".join(str(v) for v in idx)))
            f.write("static const unsigned int   ak%s[] = {%s};\n"
                    % (tag, ",".join(str(v) for v in codes)))
            f.write("static const unsigned short as%s[] = {%s};\n\n"
                    % (tag, ",".join(str(v) for v in syms)))

        for b in range(1, 12):
            emit_book(str(b), books[b])
        emit_book("scl", books[0])

        f.write("/* spectral codebooks 1..11 ([0] unused) */\n")
        f.write("static const aac_htab kAacSpec[12] = {\n")
        f.write("    { 0, 0, 0, 0 },\n")
        for b in range(1, 12):
            f.write("    { ac%d, ai%d, ak%d, as%d },\n" % (b, b, b, b))
        f.write("};\n\n")
        f.write("/* the scalefactor codebook (symbol - 60 = dpcm delta) */\n")
        f.write("static const aac_htab kAacScl = { acscl, aiscl, akscl, "
                "asscl };\n\n")

        f.write("/* scalefactor-band END offsets (exclusive tops; band b\n"
                " * spans [tab[b-1], tab[b]) with tab[-1] = 0) */\n")
        for name in sorted(sfb):
            vals = sfb[name]
            f.write("static const unsigned short k_%s[%d] = {%s};\n"
                    % (name, len(vals), ",".join(str(v) for v in vals)))

        f.write("\ntypedef struct {\n"
                "    unsigned char nlong, nshort;    /* bands per window   */\n"
                "    const unsigned short *swb_long;\n"
                "    const unsigned short *swb_short;\n"
                "} aac_sfb_info;\n\n")
        f.write("/* indexed by the AudioSpecificConfig sampling-frequency\n"
                " * index, 0..12 (96000..7350) */\n")
        f.write("static const aac_sfb_info kAacSfb[13] = {\n")
        for i, (ln_t, sh_t) in enumerate(SFB_MAP):
            f.write("    { %d, %d, k_%s, k_%s },\n"
                    % (len(sfb[ln_t]), len(sfb[sh_t]), ln_t, sh_t))
        f.write("};\n\n#endif\n")

    total = sum(len(books[b][2]) for b in books)
    print("wrote %s: 12 Huffman codebooks (%d codewords), %d sfb tables"
          % (OUT, total, len(sfb)))


if __name__ == "__main__":
    main()
