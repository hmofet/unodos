/* cosmo64/log.c -- a debug log that survives the reboot back into Linux.
 *
 * The Cosmo has no exposed UART, and the Gemian 4.4 kernel that trixie runs is
 * built "# CONFIG_DEVMEM is not set" -- so there is no /dev/mem to read a plain
 * DRAM buffer back with afterwards. Until M3 gives us eMMC, every fact a boot
 * discovers has had to leave through the panel, one 32-bit word of bit-cells at
 * a time, and be decoded from a photograph.
 *
 * That kernel does, however, reserve a ramoops region and mount pstore:
 *
 *   ramoops: pstore:address is 0x54410000, size is 0xe0000,
 *            console_size is 0x40000, pmsg_size is 0x10000
 *   ramoops: attached 0xe0000@0x54410000, ecc: 0/0
 *
 * with record_size 0x1000 and ftrace_size 0x1000 (all device-read from
 * /sys/module/ramoops/parameters/). fs/pstore/ram.c lays the zones down in one
 * fixed order -- dump records, then console, then ftrace, then pmsg -- so
 *
 *   dump_mem_sz  = 0xE0000 - 0x40000 - 0x1000 - 0x10000 = 0x8F000
 *   console zone = 0x54410000 + 0x8F000 = 0x5449F000, 0x40000 bytes
 *
 * and the four zones then end at 0x544F0000 exactly, which is the reservation's
 * own end: the arithmetic checks itself.
 *
 * So: write this log into that zone in ramoops' own format and the region is no
 * longer just DRAM that happens to survive -- it is DRAM the kernel RESERVES
 * (nothing else may allocate it), deliberately preserves across reset, and
 * reads at its next boot. Reboot into trixie and the whole UnoDOS log is
 * sitting in /sys/fs/pstore/console-ramoops-0. No /dev/mem, no kernel module,
 * no photographs. See readlog.sh.
 *
 * The format is struct persistent_ram_buffer from fs/pstore/ram_core.c: a
 * 12-byte header of {sig, start, size} then the data as a ring. For a buffer
 * that has not wrapped, the kernel's own writer leaves start == size == the
 * byte count, and persistent_ram_save_old() reconstructs data[0..size) from
 * that -- so that is what a fresh log looks like here too. ECC is off (0/0),
 * so there is no parity region to maintain.
 *
 * The zone is mapped Normal non-cacheable (mmu.c), so a warm reset cannot
 * strand the tail of the log in the D-cache.
 *
 * *** AND IT DOES NOT REACH pstore ON THIS DEVICE, CAUSE STILL OPEN. Tested
 * 2026-09-01: UnoDOS ran, was reset into trixie, /sys/fs/pstore was EMPTY.
 *
 * The first diagnosis was wrong and this file said so for one commit: MTK's
 * ram_console at 0x54400000 came up empty too, and two reservations losing
 * their contents at once looked like the preloader wiping DRAM. The survey
 * below then measured it directly and disproved that -- 82 signatures were
 * still standing in the reservation when UnoDOS took over. DRAM IS preserved
 * across this reset.
 *
 * So the buffer survives and the kernel does not read it, which points at the
 * ZONE ADDRESS after all: MTK patches fs/pstore/ram.c, and if their zone order
 * differs from mainline's dump->console->ftrace->pmsg then 0x5449F000 is one
 * of the dump zones rather than the console, and a dump record whose header
 * does not parse as ramoops' "%lld.%lu-%c" is quietly dropped. The survey now
 * records where each RUN of signatures starts, which is the zone layout, so
 * the next boot settles it instead of another guess.
 *
 * Either way the log's durable home is the eMMC (msdc.c writes it into p38's
 * unused tail, readlog.sh dd's it back), which is proven working on hardware.
 * This code stays because it is free, it is the ONLY log available before
 * storage comes up -- exactly when storage is being debugged -- and it still
 * reaches the QEMU gate, where qharness.py reads the zone directly.
 */

#include "cosmo64.h"

#define PRAM_SIG 0x43474244u            /* "DBGC", PERSISTENT_RAM_SIG */

struct pram_buf {
    c64_u32 sig;
    c64_u32 start;                      /* atomic_t: write cursor            */
    c64_u32 size;                       /* atomic_t: bytes live, capped      */
    c64_u8 data[1];
};

#define PB ((volatile struct pram_buf *)C64_LOG_ZONE)
#define CAP (C64_LOG_SIZE - 12u)

/* the full barrier that orders a log write reaching the ramoops zone (Normal
 * non-cacheable) before the reset that a later reader will follow. A named
 * macro so the host window test (cosmo64/test/logwin_test.c) can compile this
 * file on x86 by defining it away -- the barrier is aarch64 asm and irrelevant
 * to the byte arithmetic the test exercises. */
#ifndef C64_DSB
#define C64_DSB() __asm__ volatile("dsb sy" ::: "memory")
#endif

/* The boot story is the first thing written and the first thing a wrapping ring
 * drops. Capture the opening PRE bytes into a frozen buffer the wrap can never
 * reach, so the durable eMMC log (msdc.c) can carry it ahead of the recent
 * tail no matter how long the session runs. 16 KiB covers bring-up through the
 * first frames -- the DRAM survey, MMU, framebuffer, PMIC/SD, storage, the
 * module arena, USB and "entering uno_main" all fit with room to spare. It is
 * C64_EARLY because the earliest log lines are written MMU-off, before
 * mmu_init zeroes the ordinary .bss. */
#define PRE (16u * 1024u)
static c64_u8 g_pre[PRE] C64_EARLY;
static unsigned g_pre_len C64_EARLY;

static int g_live C64_EARLY;
/* bytes EVER written, monotonic: a reader that wants to follow the ring
 * (urc.c streams it to the dev PC) keeps an absolute position, and
 * total - size is the absolute position of the oldest byte still there */
static unsigned g_total C64_EARLY;

unsigned c64_log_total(void)
{
    return g_total;
}

void c64_log_init(void)
{
    PB->sig = PRAM_SIG;
    PB->start = 0;
    PB->size = 0;
    C64_DSB();
    g_live = 1;
    /* Banner: the zone holds the PREVIOUS Linux boot's console until this
     * runs, so a reader must be able to tell whose log it is reading. */
    c64_log("\n=== UnoDOS cosmo64 ===\n");
}

void c64_log_write(const char *s, unsigned n)
{
    if (!g_live)
        return;
    /* Freeze the opening PRE bytes as the boot preamble before they enter the
     * ring, so the ring's wrap can never take them. */
    if (g_pre_len < PRE) {
        unsigned c = n;
        if (c > PRE - g_pre_len)
            c = PRE - g_pre_len;
        for (unsigned i = 0; i < c; i++)
            g_pre[g_pre_len + i] = (c64_u8)s[i];
        g_pre_len += c;
    }
    c64_u32 start = PB->start, size = PB->size;
    for (unsigned i = 0; i < n; i++) {
        PB->data[start] = (c64_u8)s[i];
        if (++start >= CAP)
            start = 0;
        if (size < CAP)
            size++;
    }
    PB->start = start;
    PB->size = size;
    g_total += n;
    C64_DSB();
}

void c64_log(const char *s)
{
    unsigned n = 0;
    while (s[n])
        n++;
    c64_log_write(s, n);
}

/* ---- reading the ring back out ------------------------------------------ */
/* Logical order, exactly the reconstruction persistent_ram_save_old() does:
 * data[start..size) first, then data[0..start). For a ring that has not
 * wrapped, start == size and that degenerates to data[0..size). */

unsigned c64_log_bytes(void)
{
    return g_live ? PB->size : 0;
}

void c64_log_read(unsigned off, c64_u8 *dst, unsigned n)
{
    c64_u32 start = PB->start, size = PB->size;
    unsigned tail = size - start;              /* 0 unless the ring wrapped */
    for (unsigned i = 0; i < n; i++) {
        unsigned k = off + i;
        if (!g_live || k >= size) {
            dst[i] = 0;
            continue;
        }
        dst[i] = PB->data[k < tail ? start + k : k - tail];
    }
}

/* ---- the frozen boot preamble -------------------------------------------- */
/* The first PRE bytes ever logged, which the ring's wrap cannot reach. The
 * window model below writes these ahead of the recent tail so the durable eMMC
 * log always carries the boot story; a reader offset past what was captured
 * gets zero-filled. */
unsigned c64_log_preamble_len(void)
{
    return g_pre_len;
}

void c64_log_preamble_read(unsigned off, c64_u8 *dst, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        unsigned k = off + i;
        dst[i] = k < g_pre_len ? g_pre[k] : 0;
    }
}

/* ---- the durable-window model -------------------------------------------- *
 * A fixed-size window (the eMMC slot, msdc.c) has to hold the most useful view
 * of an arbitrarily long log. Two shapes: a short session's whole ring fits and
 * is shown verbatim (its head IS the boot story); a long session's ring has
 * wrapped the head out, so the window becomes [boot preamble][gap][recent tail]
 * -- the frozen preamble puts the boot story back at the front and the tail
 * fills the rest with the newest lines. Either way the boot story is present.
 *
 * This lives here, not in msdc.c, because it is pure log arithmetic with no
 * block I/O in it, which is what makes it testable off the device (host_logtest
 * drives exactly these two functions). msdc.c calls c64_log_window() for the
 * layout, then c64_log_window_byte() to fill each 512-byte block. */
const char c64_log_gap[] = "\n--- [older log wrapped away] ---\n";

unsigned c64_log_window(unsigned cap, unsigned *pre_out, unsigned *gap_out,
                        unsigned *tailfrom_out, unsigned *taillen_out)
{
    unsigned size = c64_log_bytes();             /* live in the ring (<= CAP) */
    unsigned total = c64_log_total();            /* ever written, monotonic   */
    unsigned pre = 0, gap = 0, taillen, tailfrom;
    if (total <= cap) {
        taillen = size;                          /* whole ring; head present  */
        tailfrom = 0;
    } else {
        pre = g_pre_len;
        gap = (unsigned)(sizeof c64_log_gap - 1u);
        taillen = cap - pre - gap;               /* the rest is the newest    */
        tailfrom = size - taillen;               /* last `taillen` ring bytes */
    }
    if (pre_out)      *pre_out = pre;
    if (gap_out)      *gap_out = gap;
    if (tailfrom_out) *tailfrom_out = tailfrom;
    if (taillen_out)  *taillen_out = taillen;
    return pre + gap + taillen;                  /* total window bytes         */
}

c64_u8 c64_log_window_byte(unsigned g, unsigned pre, unsigned gap,
                           unsigned tailfrom)
{
    c64_u8 b;
    if (g < pre) {
        c64_log_preamble_read(g, &b, 1);
        return b;
    }
    if (g < pre + gap)
        return (c64_u8)c64_log_gap[g - pre];
    c64_log_read(tailfrom + (g - pre - gap), &b, 1);
    return b;
}

/* ---- did this DRAM survive the last reset? ------------------------------- */
/* The kernel stamps PERSISTENT_RAM_SIG at the base of every ramoops zone, so
 * the reservation should be full of them when UnoDOS takes over from a Linux
 * boot. Counting them is a free, direct answer to the question the missing
 * pstore log raised: 0 means the preloader wiped DRAM on the way in, and no
 * DRAM-based log can ever survive this reset path. Run BEFORE c64_log_init,
 * which overwrites the console zone's own signature. */
static unsigned g_sigs_found C64_EARLY;
static c64_u64 g_run_start[8] C64_EARLY;
static unsigned g_runs C64_EARLY;

unsigned c64_log_survey(void)
{
    /* Record where each RUN of signatures begins, not just how many there are.
     * The kernel stamps one at the base of every zone, and the dump zones are
     * a solid 4 KiB-spaced block, so the run boundaries ARE the zone layout.
     * That is the outstanding question: DRAM demonstrably survives (82
     * signatures, 2026-09-01), yet pstore showed nothing after a UnoDOS run,
     * which points at this build writing to the wrong zone -- i.e. MTK
     * ordering ram.c's zones differently from mainline. This answers it
     * without guessing. Must run BEFORE c64_log_init overwrites one. */
    unsigned n = 0, prev = 0;
    g_runs = 0;
    for (c64_u64 a = 0x54410000ull; a < 0x544F0000ull; a += 0x1000ull) {
        unsigned here = (*(volatile c64_u32 *)a == PRAM_SIG);
        if (here) {
            n++;
            if (!prev && g_runs < 8)
                g_run_start[g_runs++] = a;
        }
        prev = here;
    }
    g_sigs_found = n;
    return n;
}

void c64_log_survey_report(void)
{
    if (!g_sigs_found) {
        c64_log("dram: NO ramoops signatures in the reservation -- DRAM did "
                "NOT survive this reset\n");
        return;
    }
    c64_logf("dram: %d ramoops signatures survived the reset -- DRAM IS "
             "preserved across it\n", (int)g_sigs_found);
    for (unsigned i = 0; i < g_runs; i++)
        c64_logf("dram: signature run %d starts at %016x%s\n", (int)i,
                 g_run_start[i],
                 g_run_start[i] == C64_LOG_ZONE ? "  <- where we log" : "");
}

/* ---- the formatter ------------------------------------------------------- */
/* Freestanding and header-free on purpose: log.c links into the m0 payload,
 * which carries none of pc64's libc. %s %c %d %i %u %x %X %p %%, an optional
 * zero/space pad width, and l/ll (addresses are the whole point). */

static unsigned emit_num(char *out, c64_u64 v, unsigned base, int upper)
{
    char tmp[24];
    unsigned n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (!v)
        tmp[n++] = '0';
    while (v) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    for (unsigned i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

/* The core takes a va_list so that pc64's own diagnostics can be routed here:
 * xhci.c speaks uno_dbg_log(fmt, ...), which is a no-op macro in a production
 * build, and c64_dbg_log() below is what the force-included c64_usbglue.h
 * points that macro at instead. */
void c64_logv(const char *fmt, __builtin_va_list ap)
{
    char out[256];
    unsigned o = 0;

    for (const char *f = fmt; *f && o < sizeof out - 24; f++) {
        if (*f != '%') {
            out[o++] = *f;
            continue;
        }
        f++;
        char pad = ' ';
        unsigned width = 0;
        if (*f == '0') {
            pad = '0';
            f++;
        }
        while (*f >= '0' && *f <= '9')
            width = width * 10 + (unsigned)(*f++ - '0');
        int lng = 0;
        while (*f == 'l') {
            lng++;
            f++;
        }
        (void)lng;                       /* LLP64: long and long long are 64  */

        char num[24];
        unsigned n = 0;
        int neg = 0;
        switch (*f) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            while (*s && o < sizeof out - 1)
                out[o++] = *s++;
            continue;
        }
        case 'c':
            out[o++] = (char)__builtin_va_arg(ap, int);
            continue;
        case 'd':
        case 'i': {
            long long v = lng ? __builtin_va_arg(ap, long long)
                              : (long long)__builtin_va_arg(ap, int);
            if (v < 0) {
                neg = 1;
                v = -v;
            }
            n = emit_num(num, (c64_u64)v, 10, 0);
            break;
        }
        case 'u': {
            c64_u64 v = lng ? __builtin_va_arg(ap, c64_u64)
                            : (c64_u64)__builtin_va_arg(ap, unsigned);
            n = emit_num(num, v, 10, 0);
            break;
        }
        case 'x':
        case 'X': {
            c64_u64 v = lng ? __builtin_va_arg(ap, c64_u64)
                            : (c64_u64)__builtin_va_arg(ap, unsigned);
            n = emit_num(num, v, 16, *f == 'X');
            break;
        }
        case 'p': {
            c64_u64 v = (c64_u64)__builtin_va_arg(ap, void *);
            out[o++] = '0';
            out[o++] = 'x';
            n = emit_num(num, v, 16, 0);
            width = 0;
            break;
        }
        case '%':
        default:
            out[o++] = *f ? *f : '%';
            continue;
        }
        if (neg && pad == '0')
            out[o++] = '-';
        for (unsigned i = n + (unsigned)neg; i < width && o < sizeof out - 1; i++)
            out[o++] = pad;
        if (neg && pad != '0')
            out[o++] = '-';
        for (unsigned i = 0; i < n && o < sizeof out - 1; i++)
            out[o++] = num[i];
    }
    c64_log_write(out, o);
}

void c64_logf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    c64_logv(fmt, ap);
    __builtin_va_end(ap);
}

/* uno_dbg_log's contract: one line per call, no trailing newline in the
 * format. Prefixed so a reader can tell pc64's driver talking from this
 * layer's own lines. */
void c64_dbg_log(const char *fmt, ...)
{
    __builtin_va_list ap;
    c64_log("pc64: ");
    __builtin_va_start(ap, fmt);
    c64_logv(fmt, ap);
    __builtin_va_end(ap);
    c64_log("\n");
}

/* ---- the fault handler's tail (cpu.s) ------------------------------------ */
/* Called LAST, after the crash record and the painted bit-cells are already
 * down, and on its own stack -- a fault that ate the stack must not cost us
 * the forensics that asm already secured. */
/* The vectors' own stack: live from cpu_early_init(), which entry.s calls
 * before anything else, so it MUST be in the early range. */
c64_u8 c64_fault_stack[0x2000] C64_EARLY;

void c64_log_crash(c64_u64 vec, c64_u64 esr, c64_u64 elr, c64_u64 far,
                   c64_u64 el)
{
    c64_logf("\n*** FAULT vec=%d ESR=%08x EC=%x ELR=%016x (image+%x)\n"
             "           FAR=%016x CurrentEL=%x\n",
             (int)vec, esr, esr >> 26, elr, elr - 0x40080000ull, far, el);
    /* And get it onto the eMMC, the one channel a later Linux boot can read
     * (the DRAM copy survives the reset but never reaches pstore -- see the
     * header). This is the whole point of the eMMC sink: the log of a boot
     * that died is the one worth having. The block driver polls with bounded
     * timeouts, so a dead controller costs seconds, not a hang -- and the
     * crash record and the painted bit-cells are already down regardless. */
    c64_log_flush();
}
