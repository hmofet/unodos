#!/usr/bin/env python3
# ===========================================================================
# mkuno.py - build .UNO loadable app modules for UnoDOS/pc64.
#
# The pc64 kernel loads apps from storage at runtime (APPS\*.UNO), like the
# C64 port loads .PRG apps through its JMP table.  A .UNO is a flattened
# PE32+ DLL: the sections laid out at their RVAs, plus the two tables the
# tiny in-kernel loader needs - base relocations (u32 RVAs of u64 cells to
# rebase) and named import slots (resolved against the kernel's export
# table).  Imports are FUNCTIONS ONLY: each undefined symbol in the app
# object becomes a one-instruction thunk `jmp *slot(%rip)`; the slot lives
# in the .unoimp section as a 32-byte {char name[24]; u64 addr;} record the
# loader fills in.  No PE parsing, no import libs, no pseudo-relocs at
# runtime.
#
#   mkuno.py thunks  <syms.txt> <out.s>       generate the thunk assembly
#   mkuno.py convert <in.dll>   <out.uno>     flatten the linked DLL
#   mkuno.py pyapp   <in.py>    <out.uno> [desc.txt]   wrap Python source
#                                                       (MODF_PYAPP)
#
# .UNO layout (all little-endian):
#   u32 magic 'UNO1'   u16 abi   u16 flags
#   u32 entry_rva      u32 mem_size (SizeOfImage)   u32 file_size
#   u32 nreloc         u32 imp_rva   u32 imp_count
#   u64 pref_base      u32 crc32 (of everything after the header)  u32 desc_rva
#   image[file_size]   reloc_rva[nreloc] (u32 each)
#
# desc_rva (the header word formerly called `rsv`, always written 0) points at
# the app descriptor - the launcher metadata a module carries about itself, in
# its own `.unodesc` section.  See pc64/uno_appdesc.h.  Validation happens HERE,
# at build time, for the same reason the kExports import check does: a typo
# should be a build failure, not an app that installs itself wrongly.
# ===========================================================================
import struct, sys, zlib

MAGIC = 0x314F4E55          # 'UNO1'
ABI   = 1                   # UNO_ABI_VERSION
NAME_MAX = 23               # 24-byte name field, NUL-terminated
HDR_FMT = "<IHHIIIIIIQII"   # 48 bytes
IMAGE_REL_BASED_ABSOLUTE = 0
IMAGE_REL_BASED_DIR64    = 10
UNO_MODF_PYAPP = 0x0004     # source-container tier (no code/relocs/imports)

# ---- app descriptor (uno_appdesc.h) ---------------------------------------
DESC_MAGIC = 0x50504155     # 'UAPP'
DESC_VER   = 1
DESC_MAX   = 1024
DESC_CATS  = ("system", "net", "tools", "media", "games", "other")
DESC_FLAGS = ("singleton", "hidden", "game", "nosession")
DESC_KEYS  = ("id", "name", "short", "icon", "cat", "rank", "flags", "min",
              "needs")
ID_OK = set("abcdefghijklmnopqrstuvwxyz0123456789._-")


def check_desc(img, va, vsz, path):
    """Validate the .unodesc block at RVA `va` and return its length.

    Refuses: a bad prologue, a second block in the section (two translation
    units in one module each declaring UNO_APP_DESC), an unknown category, an
    unknown flag, a malformed `min:`, and an `id:` with characters that cannot
    survive being a config key.  An unknown KEY is fine and always will be -
    that is the format's extension point."""
    if vsz < 8:
        sys.exit("mkuno: %s .unodesc is %d bytes, too small" % (path, vsz))
    magic, ver, ln = struct.unpack_from("<IHH", img, va)
    if magic != DESC_MAGIC:
        sys.exit("mkuno: %s .unodesc has bad magic 0x%08x "
                 "(use the UNO_APP_DESC macro)" % (path, magic))
    if ver != DESC_VER:
        sys.exit("mkuno: %s .unodesc version %d, this mkuno speaks %d"
                 % (path, ver, DESC_VER))
    if ln <= 8 or ln > DESC_MAX or ln > vsz:
        sys.exit("mkuno: %s .unodesc length %d out of range (8 < len <= %d)"
                 % (path, ln, min(DESC_MAX, vsz)))
    if struct.pack("<I", DESC_MAGIC) in bytes(img[va + 8: va + vsz]):
        sys.exit("mkuno: %s has more than one UNO_APP_DESC block - a module "
                 "describes itself once" % path)

    body = bytes(img[va + 8: va + ln]).split(b"\0")[0].decode("ascii", "replace")
    seen = set()
    for raw in body.split("\n"):
        line = raw.strip()
        if not line:
            continue
        if ":" not in line:
            sys.exit("mkuno: %s .unodesc line is not 'key: value': %r"
                     % (path, line))
        k, v = line.split(":", 1)
        k, v = k.strip(), v.strip()
        if k in seen:
            sys.exit("mkuno: %s .unodesc repeats key '%s'" % (path, k))
        seen.add(k)
        if k not in DESC_KEYS:
            print("mkuno: note: %s .unodesc has unknown key '%s' (ignored at "
                  "runtime, kept for forward compatibility)" % (path, k))
            continue
        if k == "id":
            if not v or len(v) > 15 or any(c not in ID_OK for c in v):
                sys.exit("mkuno: %s .unodesc id '%s' must be 1-15 chars of "
                         "[a-z0-9._-]" % (path, v))
        elif k == "icon":
            # two forms: a NAME from the kernel's emblem set (validated at
            # runtime - a name this build does not know falls back to the
            # generic emblem rather than failing), or `file:NAME.QOI`, art the
            # app ships beside its own .UNO. QOI because the shell has to draw
            # the icon before it would load the app's code, so the decoder is
            # in the kernel, and PNG's inflate is not going in there.
            if v.startswith("file:"):
                fn = v[5:]
                if not fn or "\\" in fn or "/" in fn:
                    sys.exit("mkuno: %s .unodesc icon file '%s' must be a bare "
                             "name beside the module" % (path, fn))
                if not fn.upper().endswith(".QOI"):
                    sys.exit("mkuno: %s .unodesc icon file '%s' must be .QOI - "
                             "it is the only format the kernel can decode "
                             "without loading a module" % (path, fn))
        elif k == "cat" and v not in DESC_CATS:
            sys.exit("mkuno: %s .unodesc unknown cat '%s' (one of %s)"
                     % (path, v, "/".join(DESC_CATS)))
        elif k == "flags":
            for f in [f.strip() for f in v.split(",") if f.strip()]:
                if f not in DESC_FLAGS:
                    sys.exit("mkuno: %s .unodesc unknown flag '%s' (one of %s)"
                             % (path, f, "/".join(DESC_FLAGS)))
        elif k == "rank":
            if not v.isdigit() or int(v) > 255:
                sys.exit("mkuno: %s .unodesc rank '%s' must be 0-255"
                         % (path, v))
        elif k == "min":
            wh = v.lower().split("x")
            if len(wh) != 2 or not all(p.isdigit() and 0 < int(p) < 8192
                                       for p in wh):
                sys.exit("mkuno: %s .unodesc min '%s' must be WxH" % (path, v))
    return ln


def pyapp(py_path, uno_path, desc_path=None):
    """Wrap a .py file's source bytes into a UNO_MODF_PYAPP container: the
    48-byte header (flags=PYAPP, file_size=len(src), crc32(src)) followed by
    the raw source.  No image, no relocs, no imports - PYRT.UNO compiles the
    payload at load time.  Source is normalised to LF so an on-disk CRLF
    checkout can't shift the crc away from what the on-device writer produces.

    With `desc_path`, an app descriptor is appended AFTER the source and
    `desc_rva` points at it.  After, not inside, for two reasons: the payload
    is Python that PYRT compiles verbatim, so nothing may be spliced into it,
    and `file_size` still covers only the source, so the crc and the loader's
    bounds check are untouched.  The loader tolerates trailing bytes - it tests
    `48 + file_size > n`, not `==` - which is what makes the room legal.

    Beside the .py rather than inside it is also the only option here: DUUM.PY
    is generated upstream and vendored verbatim, so a magic comment in the
    source would be overwritten by the next sync."""
    src = open(py_path, "rb").read().replace(b"\r\n", b"\n")
    blk, desc_rva = b"", 0
    if desc_path:
        body = open(desc_path, "rb").read().replace(b"\r\n", b"\n")
        if not body.endswith(b"\n"):
            body += b"\n"
        body += b"\0"                       # the reader stops at the first NUL
        blk = struct.pack("<IHH", DESC_MAGIC, DESC_VER, 8 + len(body)) + body
        check_desc(bytearray(blk), 0, len(blk), uno_path)   # the C modules' gate
        desc_rva = len(src)
    hdr = struct.pack(HDR_FMT, MAGIC, ABI, UNO_MODF_PYAPP,
                      0, len(src), len(src),   # entry=0, mem_size, file_size
                      0, 0, 0,                 # nreloc, imp_rva, imp_count
                      0,                       # pref_base
                      zlib.crc32(src) & 0xFFFFFFFF, desc_rva)
    open(uno_path, "wb").write(hdr + src + blk)
    print("mkuno: %s  PYAPP src=%d bytes%s"
          % (uno_path, len(src), (", desc %d bytes" % len(blk)) if blk else ""))


def gen_thunks(syms_path, out_path):
    syms = [s.strip() for s in open(syms_path) if s.strip()]
    for s in syms:
        if len(s) > NAME_MAX:
            sys.exit("mkuno: import name too long (>%d): %s" % (NAME_MAX, s))
    lines = ["/* generated by mkuno.py - do not edit */"]
    for s in sorted(syms):
        lines += [
            '\t.text',
            '\t.globl %s' % s,
            '%s:' % s,
            '\tjmp *.Luslot_%s(%%rip)' % s,
            '\t.section .unoimp,"w"',
            '\t.asciz "%s"' % s,
            '\t.space %d' % (NAME_MAX - len(s)),
            '.Luslot_%s:' % s,
            '\t.quad 0',
        ]
    open(out_path, "w").write("\n".join(lines) + "\n")
    print("mkuno: %d import thunks -> %s" % (len(syms), out_path))


def convert(dll_path, uno_path, flags=0):
    d = open(dll_path, "rb").read()
    if d[:2] != b"MZ":
        sys.exit("mkuno: not a PE file")
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        sys.exit("mkuno: bad PE signature")
    machine, nsect, _, _, _, opt_sz, _ = struct.unpack_from("<HHIIIHH", d, pe + 4)
    if machine != 0x8664:
        sys.exit("mkuno: not x86-64")
    opt = pe + 24
    if struct.unpack_from("<H", d, opt)[0] != 0x20B:
        sys.exit("mkuno: not PE32+")
    entry      = struct.unpack_from("<I", d, opt + 16)[0]
    image_base = struct.unpack_from("<Q", d, opt + 24)[0]
    size_image = struct.unpack_from("<I", d, opt + 56)[0]
    nddir      = struct.unpack_from("<I", d, opt + 108)[0]

    def ddir(i):
        if i >= nddir:
            return (0, 0)
        return struct.unpack_from("<II", d, opt + 112 + 8 * i)

    # a decoupled module must carry NO firmware-loader baggage
    for i, what in ((9, "TLS directory"), (13, "delay imports")):
        rva, sz = ddir(i)
        if rva or sz:
            sys.exit("mkuno: DLL has a %s - imports must go through thunks" % what)

    # ---- flatten sections at their RVAs ----------------------------------
    img = bytearray(size_image)
    imp_rva = imp_size = 0
    desc_rva = desc_size = 0
    shoff = opt + opt_sz
    for i in range(nsect):
        name, vsz, va, rsz, roff = struct.unpack_from("<8sIIII", d, shoff + 40 * i)
        name = name.rstrip(b"\0").decode()
        n = min(vsz, rsz)
        if n:
            img[va:va + n] = d[roff:roff + n]
        if name == ".unoimp":
            imp_rva, imp_size = va, vsz
        if name == ".unodesc":
            desc_rva, desc_size = va, min(vsz, rsz)
    if imp_size % 32:
        sys.exit("mkuno: .unoimp size %d not a multiple of 32" % imp_size)
    imp_count = imp_size // 32

    # mingw ld always emits an .idata with one null descriptor for DLLs;
    # tolerate that, but any REAL import descriptor means a symbol bypassed
    # the thunks - refuse the module.
    irva, isz = ddir(1)
    p = irva
    while p and p + 20 <= irva + isz:
        if any(struct.unpack_from("<5I", img, p)):
            sys.exit("mkuno: DLL has real PE imports - must go through thunks")
        p += 20

    # ---- base relocations -------------------------------------------------
    relocs = []
    rrva, rsz = ddir(5)
    p, end = rrva, rrva + rsz
    while p < end:
        page, bsz = struct.unpack_from("<II", img, p)
        if bsz < 8:
            break
        for off in struct.unpack_from("<%dH" % ((bsz - 8) // 2), img, p + 8):
            t, o = off >> 12, off & 0xFFF
            if t == IMAGE_REL_BASED_ABSOLUTE:
                continue
            if t != IMAGE_REL_BASED_DIR64:
                sys.exit("mkuno: unsupported reloc type %d" % t)
            relocs.append(page + o)
        p += bsz
    relocs.sort()

    # the loaded image never needs its own PE headers or .reloc bytes; the
    # reloc section usually sits last so the zero-trim below drops it.
    if rsz:
        img[rrva:rrva + rsz] = b"\0" * rsz

    # ---- trim trailing zeros (bss + zeroed .reloc stay virtual) ----------
    file_size = len(img)
    while file_size > 0 and img[file_size - 1] == 0:
        file_size -= 1
    file_size = (file_size + 7) & ~7

    # ---- the app descriptor ----------------------------------------------
    desc_len = 0
    if desc_rva:
        desc_len = check_desc(img, desc_rva, desc_size, uno_path)
        # The block MUST be inside the trimmed file image: the shell reads it
        # straight off disk at 48 + desc_rva without loading the module, and a
        # descriptor left in bss is a descriptor nothing can read.
        if desc_rva + desc_len > file_size:
            sys.exit("mkuno: %s .unodesc at RVA 0x%x+%d is past the trimmed "
                     "image (%d bytes) and could not be read from disk"
                     % (uno_path, desc_rva, desc_len, file_size))

    payload = bytes(img[:file_size]) + struct.pack("<%dI" % len(relocs), *relocs)
    hdr = struct.pack(HDR_FMT, MAGIC, ABI, flags, entry, size_image, file_size,
                      len(relocs), imp_rva, imp_count, image_base,
                      zlib.crc32(payload) & 0xFFFFFFFF, desc_rva)
    open(uno_path, "wb").write(hdr + payload)
    names = []
    for i in range(imp_count):
        nm = bytes(img[imp_rva + 32 * i: imp_rva + 32 * i + NAME_MAX + 1])
        names.append(nm.split(b"\0")[0].decode())
    print("mkuno: %s  entry=0x%x mem=%dK file=%dK relocs=%d imports=%d (%s)%s"
          % (uno_path, entry, size_image // 1024, (48 + len(payload)) // 1024,
             len(relocs), imp_count, " ".join(names) if imp_count else "-",
             "  desc=%dB" % desc_len if desc_len else "  NO DESCRIPTOR"))


if __name__ == "__main__":
    if len(sys.argv) == 4 and sys.argv[1] == "thunks":
        gen_thunks(sys.argv[2], sys.argv[3])
    elif len(sys.argv) in (4, 5) and sys.argv[1] == "pyapp":
        pyapp(sys.argv[2], sys.argv[3],
              sys.argv[4] if len(sys.argv) == 5 else None)
    elif len(sys.argv) in (4, 5) and sys.argv[1] == "convert":
        # optional 4th arg: header flags (1 = unoui-class module)
        convert(sys.argv[2], sys.argv[3],
                int(sys.argv[4], 0) if len(sys.argv) == 5 else 0)
    else:
        sys.exit(__doc__ or "usage: mkuno.py thunks|convert <in> <out> [flags]")
