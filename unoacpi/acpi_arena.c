/* acpi_arena.c - see acpi_arena.h.
 *
 * Block layout (all sizes are multiples of 8):
 *
 *     +--------+-------------------------+--------+
 *     |  head  |         payload         |  foot  |
 *     +--------+-------------------------+--------+
 *      8 bytes    total-16 bytes          8 bytes
 *
 * Both head and foot hold the same 64-bit tag word = (total | free_bit).  total
 * is always a multiple of 8, so its low 3 bits are free for flags; bit 0 = "this
 * block is free".  A fixed 8-byte tag (uint64_t on both 32- and 64-bit builds)
 * keeps the payload 8-byte aligned and the pointer arithmetic identical across
 * word sizes.  The duplicated foot tag is what makes O(1) backward coalescing
 * possible: a freed block reads its predecessor's foot to find its size.
 */
#include "acpi_arena.h"

#define TAG      8u                 /* bytes per boundary tag (head, foot)       */
#define MIN_PAY  8u                 /* smallest payload we ever hand out          */
#define MIN_BLK  (2u * TAG + MIN_PAY)   /* 24: smallest whole block               */

static uint8_t *g_base;             /* 8-aligned start of usable region           */
static size_t   g_len;              /* usable length, multiple of 8               */
static uint8_t *g_rover;            /* next-fit search cursor                      */
static size_t   g_used;             /* bytes in used blocks (incl. their tags)    */
static size_t   g_peak;             /* all-time high of g_used                     */
static int      g_ready;

static inline size_t align8(size_t n) { return (n + 7u) & ~(size_t)7u; }

static inline uint64_t tag_read(uint8_t *b) { return *(uint64_t *)b; }

/* Stamp both the head and the foot of a block of the given total size. */
static inline void tag_write(uint8_t *b, uint64_t total, int free)
{
    uint64_t w = total | (free ? 1u : 0u);
    *(uint64_t *)b = w;
    *(uint64_t *)(b + total - TAG) = w;
}

static inline uint64_t blk_total(uint8_t *b) { return tag_read(b) & ~(uint64_t)7u; }
static inline int      blk_free(uint8_t *b)  { return (int)(tag_read(b) & 1u); }

int acpi_arena_init(void *buf, size_t len)
{
    g_ready = 0;
    if (!buf || len < MIN_BLK + 8)
        return 0;

    uintptr_t raw = (uintptr_t)buf;
    uintptr_t aln = (raw + 7u) & ~(uintptr_t)7u;    /* align base up to 8         */
    size_t    adj = (size_t)(aln - raw);
    if (adj >= len)
        return 0;
    g_base = (uint8_t *)aln;
    g_len  = (len - adj) & ~(size_t)7u;             /* trim tail to 8-multiple    */
    if (g_len < MIN_BLK)
        return 0;

    tag_write(g_base, g_len, 1);                    /* one big free block         */
    g_rover = g_base;
    g_used  = 0;
    g_peak  = 0;
    g_ready = 1;
    return 1;
}

void acpi_arena_reset(void)
{
    if (!g_ready)
        return;
    tag_write(g_base, g_len, 1);
    g_rover = g_base;
    g_used  = 0;
    /* g_peak is deliberately kept as an all-time figure across resets. */
}

/* Next-fit: walk the block ring from the rover, visiting each block at most once
 * (the running byte count is bounded by g_len == sum of all block totals). */
static uint8_t *find_fit(uint64_t need)
{
    uint8_t *end = g_base + g_len;
    if (g_rover < g_base || g_rover >= end)
        g_rover = g_base;

    uint8_t *b = g_rover;
    size_t   scanned = 0;
    while (scanned < g_len) {
        uint64_t t = blk_total(b);
        if (t < MIN_BLK || b + t > end)             /* corruption guard           */
            return 0;
        if (blk_free(b) && t >= need)
            return b;
        scanned += (size_t)t;
        b += t;
        if (b >= end)
            b = g_base;
    }
    return 0;
}

void *acpi_arena_alloc(size_t n)
{
    if (!g_ready || n == 0)
        return 0;

    size_t   pay  = align8(n);
    if (pay < MIN_PAY)
        pay = MIN_PAY;
    uint64_t need = (uint64_t)pay + 2u * TAG;

    uint8_t *b = find_fit(need);
    if (!b)
        return 0;

    uint8_t *end = g_base + g_len;
    uint64_t t   = blk_total(b);

    if (t - need >= MIN_BLK) {                       /* split off the remainder    */
        tag_write(b, need, 0);
        uint8_t *rem = b + need;
        tag_write(rem, t - need, 1);
        g_used += (size_t)need;
        g_rover = rem;
    } else {                                         /* consume the whole block    */
        tag_write(b, t, 0);
        g_used += (size_t)t;
        g_rover = b + t;
        if (g_rover >= end)
            g_rover = g_base;
    }
    if (g_used > g_peak)
        g_peak = g_used;

    return b + TAG;
}

void acpi_arena_free(void *p)
{
    if (!p || !g_ready)
        return;

    uint8_t *b   = (uint8_t *)p - TAG;
    uint8_t *end = g_base + g_len;
    if (b < g_base || b >= end)                      /* not ours - ignore          */
        return;

    uint64_t t = blk_total(b);
    if (t < MIN_BLK || b + t > end)                  /* corruption guard           */
        return;
    if (blk_free(b))                                 /* double free - ignore       */
        return;

    if (g_used >= (size_t)t)
        g_used -= (size_t)t;

    /* Coalesce forward with the next block if it is free. */
    uint8_t *nb = b + t;
    if (nb < end && blk_free(nb))
        t += blk_total(nb);

    /* Coalesce backward with the previous block via its foot tag. */
    if (b > g_base) {
        uint64_t pf = *(uint64_t *)(b - TAG);
        if (pf & 1u) {
            uint64_t pt = pf & ~(uint64_t)7u;
            uint8_t *pb = b - pt;
            if (pb >= g_base) {
                b  = pb;
                t += pt;
            }
        }
    }

    tag_write(b, t, 1);
    g_rover = b;                                     /* reuse the hole next        */
}

int acpi_arena_ready(void) { return g_ready; }

void acpi_arena_stats(size_t *used, size_t *peak, size_t *total)
{
    if (used)  *used  = g_used;
    if (peak)  *peak  = g_peak;
    if (total) *total = g_len;
}
