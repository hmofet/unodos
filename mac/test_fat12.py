#!/usr/bin/env python3
"""Host-side test for the Mac port's FAT12 core (the PC-interchange check).

Extracts the FAT12 section from unodos.c, compiles it with gcc against
toolbox shims, runs a write workload through the C core on the RAM image,
then INDEPENDENTLY parses the resulting image with this script's own
FAT12 reader (no shared code) and verifies the files byte-for-byte.
"""
import re, subprocess, sys, struct, pathlib

SRC = pathlib.Path(__file__).with_name("unodos.c").read_text(encoding="utf-8")

start = SRC.index(" * PC-compatible floppy: FAT12 read/write")
start = SRC.rindex("/*", 0, start)
end = SRC.index(" * Files app - File Manager directory listing")
end = SRC.rindex("/*", 0, end)
section = SRC[start:end]
# host shims replace the toolbox surface
section = section.replace("ParamBlockRec pb;", "int pb_unused;") \
    .replace("memset(&pb, 0, sizeof(pb));", "(void)pb_unused;") \

# stub out the .Sony device body (host has no Mac driver)
section = re.sub(
    r"static Boolean fat_dev_sony\(.*?\n\}\n",
    "static Boolean fat_dev_sony(Boolean w, short l, unsigned char *b)"
    "{ (void)w; (void)l; (void)b; return false; }\n",
    section, flags=re.S)

harness = """
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int Boolean;
typedef char *Ptr;
#define true 1
#define false 0
#define NBUF 4096
#define NewPtr(n) ((Ptr)malloc((size_t)(n)))
#define DisposePtr(p) free(p)
#define UNO_AUTOTEST 1

%SECTION%

int main(void)
{
    static unsigned char big[3000];
    unsigned char back[4096];
    long i, got;
    if (!fat12_mount()) { puts("FAIL mount"); return 1; }
    /* multi-cluster file + small file + overwrite */
    for (i = 0; i < 3000; i++) big[i] = (unsigned char)(i * 7 + 3);
    if (!fat12_write("BIG.BIN", big, 3000)) { puts("FAIL write big"); return 1; }
    if (!fat12_write("HELLO.TXT", (const unsigned char *)"hello fat12", 11)) { puts("FAIL write small"); return 1; }
    if (!fat12_write("HELLO.TXT", (const unsigned char *)"overwritten!", 12)) { puts("FAIL overwrite"); return 1; }
    fat12_list();
    printf("count=%d\\n", gFatCount);
    for (i = 0; i < gFatCount; i++)
        printf("entry %s %ld\\n", gFatNames[i], gFatSizes[i]);
    got = fat12_read("BIG.BIN", back, sizeof back);
    if (got != 3000 || memcmp(back, big, 3000)) { puts("FAIL read big"); return 1; }
    got = fat12_read("HELLO.TXT", back, sizeof back);
    if (got != 12 || memcmp(back, "overwritten!", 12)) { puts("FAIL read small"); return 1; }
    {
        FILE *f = fopen("fat12_test.img", "wb");
        fwrite(gFatRam, 1, 64 * 512, f);
        fclose(f);
    }
    puts("C-CORE-OK");
    return 0;
}
"""
pathlib.Path("/tmp/fat_test.c").write_text(harness.replace("%SECTION%", section))
r = subprocess.run(["gcc", "-o", "/tmp/fat_test", "/tmp/fat_test.c", "-Wall"],
                   capture_output=True, text=True)
if r.returncode:
    print(r.stderr[:3000]); sys.exit(1)
r = subprocess.run(["/tmp/fat_test"], capture_output=True, text=True)
print(r.stdout, r.stderr)
if "C-CORE-OK" not in r.stdout:
    sys.exit(1)

# ---- independent FAT12 parser (no shared code with the C core) ----
img = pathlib.Path("fat12_test.img").read_bytes()
bps, spc, rsvd, nfats, roote, tot, fsz = (
    struct.unpack_from("<H", img, 11)[0], img[13],
    struct.unpack_from("<H", img, 14)[0], img[16],
    struct.unpack_from("<H", img, 17)[0],
    struct.unpack_from("<H", img, 19)[0],
    struct.unpack_from("<H", img, 22)[0])
assert bps == 512 and img[510:512] == b"\x55\xAA", "boot sector"
fat = img[rsvd*512:(rsvd+fsz)*512]
fat2 = img[(rsvd+fsz)*512:(rsvd+2*fsz)*512]
assert fat == fat2, "FAT copies differ"
rootlba = rsvd + nfats*fsz
datalba = rootlba + (roote*32 + 511)//512

def fatent(cl):
    off = cl + cl//2
    v = fat[off] | (fat[off+1] << 8)
    return (v >> 4) if cl & 1 else (v & 0xFFF)

def read_file(name83):
    root = img[rootlba*512:datalba*512]
    for o in range(0, roote*32, 32):
        e = root[o:o+32]
        if e[0] in (0, 0xE5): continue
        if e[:11] == name83:
            cl = struct.unpack_from("<H", e, 26)[0]
            size = struct.unpack_from("<I", e, 28)[0]
            data = b""
            while 2 <= cl < 0xFF8:
                lba = datalba + (cl-2)*spc
                data += img[lba*512:(lba+spc)*512]
                cl = fatent(cl)
            return data[:size]
    return None

big = bytes((i*7+3) & 0xFF for i in range(3000))
assert read_file(b"BIG     BIN") == big, "independent read of BIG.BIN"
assert read_file(b"HELLO   TXT") == b"overwritten!", "independent read of HELLO.TXT"
print("INDEPENDENT-PARSER-OK: the image is real FAT12, PC-readable")
