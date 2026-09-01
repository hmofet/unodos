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
 * *** AND IT DOES NOT WORK ON THIS DEVICE. Tested 2026-09-01: UnoDOS ran, was
 * reset into trixie, and /sys/fs/pstore was EMPTY. The cause is not the zone
 * address -- MTK's own ram_console at 0x54400000, a SEPARATE reservation, came
 * up equally empty (/proc/last_kmsg held 74 bytes of freshly generated header
 * and no prior log). Two independent reservations losing their contents at
 * once is DRAM being wiped, not a layout mistake: the preloader re-initialises
 * DRAM on this reset path, and ram_console/pstore only ever worked for the
 * abnormal resets MTK carries mrdump for.
 *
 * So the log's durable home is the eMMC (msdc.c writes it into p38's unused
 * tail, and readlog.sh dd's it back). This code stays because it is free, it
 * is the ONLY log available before storage comes up -- which is exactly when
 * storage is being debugged -- and it still reaches the QEMU gate, where
 * qharness.py reads the zone directly. c64_log_survey() re-tests the DRAM
 * question on every boot and says so in the log.
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

static int g_live;

void c64_log_init(void)
{
    PB->sig = PRAM_SIG;
    PB->start = 0;
    PB->size = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    g_live = 1;
    /* Banner: the zone holds the PREVIOUS Linux boot's console until this
     * runs, so a reader must be able to tell whose log it is reading. */
    c64_log("\n=== UnoDOS cosmo64 ===\n");
}

void c64_log_write(const char *s, unsigned n)
{
    if (!g_live)
        return;
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
    __asm__ volatile("dsb sy" ::: "memory");
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

/* ---- did this DRAM survive the last reset? ------------------------------- */
/* The kernel stamps PERSISTENT_RAM_SIG at the base of every ramoops zone, so
 * the reservation should be full of them when UnoDOS takes over from a Linux
 * boot. Counting them is a free, direct answer to the question the missing
 * pstore log raised: 0 means the preloader wiped DRAM on the way in, and no
 * DRAM-based log can ever survive this reset path. Run BEFORE c64_log_init,
 * which overwrites the console zone's own signature. */
static unsigned g_sigs_found;

unsigned c64_log_survey(void)
{
    unsigned n = 0;
    for (c64_u64 a = 0x54410000ull; a < 0x544F0000ull; a += 0x1000ull)
        if (*(volatile c64_u32 *)a == PRAM_SIG)
            n++;
    g_sigs_found = n;
    return n;
}

void c64_log_survey_report(void)
{
    if (g_sigs_found)
        c64_logf("dram: %d ramoops signatures survived the reset -- DRAM is "
                 "preserved across it\n", (int)g_sigs_found);
    else
        c64_log("dram: NO ramoops signatures in the reservation -- the "
                "preloader wipes DRAM, so the eMMC log is the only one\n");
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

void c64_logf(const char *fmt, ...)
{
    char out[256];
    unsigned o = 0;
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);

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
    __builtin_va_end(ap);
    c64_log_write(out, o);
}

/* ---- the fault handler's tail (cpu.s) ------------------------------------ */
/* Called LAST, after the crash record and the painted bit-cells are already
 * down, and on its own stack -- a fault that ate the stack must not cost us
 * the forensics that asm already secured. */
c64_u8 c64_fault_stack[0x2000];

void c64_log_crash(c64_u64 vec, c64_u64 esr, c64_u64 elr, c64_u64 far,
                   c64_u64 el)
{
    c64_logf("\n*** FAULT vec=%d ESR=%08x EC=%x ELR=%016x (image+%x)\n"
             "           FAR=%016x CurrentEL=%x\n",
             (int)vec, esr, esr >> 26, elr, elr - 0x40080000ull, far, el);
    /* And get it off DRAM, which this device's reset path wipes. This is the
     * whole point of the eMMC sink: the log of a boot that died is the one
     * worth having. The block driver polls with bounded timeouts, so a dead
     * controller costs seconds, not a hang -- and the crash record and the
     * painted bit-cells are already down regardless. */
    c64_log_flush();
}
