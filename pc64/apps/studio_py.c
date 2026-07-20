/* ===========================================================================
 * studio_py.c - Studio's Python back end.
 *
 * A Python app doesn't compile to native code; it "compiles" by being wrapped
 * in a UNO_MODF_PYAPP container (the 48-byte UnoModHdr + the raw source bytes),
 * which the shell hands to PYRT.UNO to interpret.  This is the exact container
 * `tools/mkuno.py pyapp` writes off-device, so a Studio-built NAME.UNO is
 * byte-identical to the toolchain's - same header fields, same zlib crc32.
 *
 * Kept tiny and dependency-free (Studio is a freestanding .UNO): just a crc and
 * a header serialiser writing little-endian fields by hand.
 * ======================================================================== */

#define PY_MAGIC 0x314F4E55u        /* 'UNO1' */
#define PY_ABI   1                  /* UNO_ABI_VERSION */
#define PY_MODF_PYAPP 0x0004u       /* source-container tier */
#define PY_HDR   48                 /* sizeof(UnoModHdr) */

/* zlib crc32 (poly 0xEDB88320) - matches pc64_modload.c's mod_crc32 and the
 * host mkuno.py, so the container the loader validates is bit-for-bit ours. */
static unsigned int py_crc32(const unsigned char *p, int n)
{
    unsigned int c = 0xFFFFFFFFu; int i, k;
    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

static void put_u32(unsigned char *p, unsigned int v)
{ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put_u16(unsigned char *p, unsigned short v)
{ p[0]=v; p[1]=v>>8; }

/* Return 1 if `name` ends in ".py"/".PY" (case-insensitive). */
int studio_is_py(const char *name)
{
    int L = 0, a, b, c;
    while (name[L]) L++;
    if (L < 3) return 0;
    a = name[L-3], b = name[L-2], c = name[L-1];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (c >= 'A' && c <= 'Z') c += 32;
    return a == '.' && b == 'p' && c == 'y';
}

/* Wrap `src[0..len)` into a PYAPP container in `out` (cap bytes).  Returns the
 * total container size, or -1 if it wouldn't fit.  No image/relocs/imports:
 * entry=0, mem_size=file_size=len, nreloc=imp_rva=imp_count=pref_base=rsv=0. */
int studio_py_pack(const unsigned char *src, int len, unsigned char *out, int cap)
{
    int i;
    if (len < 0 || PY_HDR + len > cap) return -1;
    for (i = 0; i < PY_HDR; i++) out[i] = 0;
    put_u32(out + 0,  PY_MAGIC);
    put_u16(out + 4,  PY_ABI);
    put_u16(out + 6,  PY_MODF_PYAPP);
    put_u32(out + 8,  0);                     /* entry */
    put_u32(out + 12, (unsigned)len);         /* mem_size */
    put_u32(out + 16, (unsigned)len);         /* file_size */
    put_u32(out + 20, 0);                     /* nreloc */
    put_u32(out + 24, 0);                     /* imp_rva */
    put_u32(out + 28, 0);                     /* imp_count */
    /* pref_base u64 at 32..39 already zeroed */
    put_u32(out + 40, py_crc32(src, len));    /* crc of payload */
    put_u32(out + 44, 0);                     /* rsv */
    for (i = 0; i < len; i++) out[PY_HDR + i] = src[i];
    return PY_HDR + len;
}
