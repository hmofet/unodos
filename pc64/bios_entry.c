/* bios_entry.c - the services a BIOS boot has to provide for itself.
 *
 * On the UEFI path the firmware answers three questions during init: how fast
 * is the TSC, where is the ACPI RSDP, and where is the SMBIOS table. All three
 * come from boot services or the EFI configuration table, and on a BIOS boot
 * none of that exists. This file answers them the way everything did before
 * UEFI, which is not a workaround - the legacy methods are the ORIGINAL ones,
 * and the configuration table is the special case.
 *
 * Nothing here touches the UEFI path. `uno_pc64_init()` calls these only when
 * it was entered from `uno_bios_main()`.
 */
#include "bootinfo.h"
#include "pc64_native.h"
#include <string.h>

/* Port I/O. pc64_native.c has the same pair as file-static inlines rather than
 * in a header, so they are duplicated here rather than exported - two lines of
 * inline asm is a smaller thing to own than a new cross-file dependency in the
 * one file that runs before anything else is up. */
static inline unsigned char n_inb(unsigned short port)
{
    unsigned char v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline void n_outb(unsigned short port, unsigned char v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* ---- TSC calibration against PIT channel 2 --------------------------------
 * The UEFI path calibrates against one firmware `Stall`. There is no Stall
 * here, and the CPUID leaf that reports the TSC frequency directly is absent on
 * everything older than Skylake - which is most of this port's target list. So
 * the method is the one that has worked since the 8253: gate channel 2 by hand,
 * count down a known number of 1.193182 MHz ticks, and see how far the TSC
 * moved.
 *
 * Channel 2 is the right channel and the only safe one. Channels 0 and 1 are
 * the system timer and the DRAM refresh; reprogramming either during boot is a
 * good way to hang a machine. Channel 2 drives the PC speaker, is gated by a
 * bit WE control (port 0x61 bit 0), and nothing else depends on it - the
 * speaker output is disabled here (bit 1 clear) so the calibration is silent.
 */
#define PIT_HZ      1193182u
#define CAL_TICKS   0x4000u          /* ~13.7 ms: long enough that the fixed
                                      * overhead of the port I/O is noise, short
                                      * enough not to be felt at boot */

unsigned long long uno_bios_calibrate_tsc(void)
{
    unsigned char p61 = n_inb(0x61);
    unsigned long long t0, t1;
    unsigned int lo, hi, last, cur;
    int wrapped = 0;

    /* speaker data off (bit 1), counter gate on (bit 0) */
    n_outb(0x61, (unsigned char)((p61 & ~0x02) | 0x01));
    n_outb(0x43, 0xB0);                      /* ch2, lobyte/hibyte, mode 0 */
    n_outb(0x42, (unsigned char)(CAL_TICKS & 0xFF));
    n_outb(0x42, (unsigned char)(CAL_TICKS >> 8));

    t0 = uno_native_rdtsc();
    last = CAL_TICKS;
    for (;;) {
        n_outb(0x43, 0x80);                  /* latch ch2 */
        lo = n_inb(0x42);
        hi = n_inb(0x42);
        cur = (hi << 8) | lo;
        /* Mode 0 counts DOWN to zero and then wraps to 0xFFFF. Watching for the
         * wrap rather than for "cur == 0" is what makes this reliable: the
         * counter can step past zero between two latches, and a test for zero
         * would then spin for a full 55 ms wrap - or forever on a machine where
         * the latch races. */
        if (cur > last) { wrapped = 1; break; }
        last = cur;
        if (cur == 0) break;
    }
    t1 = uno_native_rdtsc();
    n_outb(0x61, p61);                       /* leave the port as we found it */

    if (!wrapped && last != 0) return 0;     /* the PIT never moved: no answer */
    {
        unsigned long long cycles = t1 - t0;
        /* elapsed microseconds = ticks * 1e6 / PIT_HZ */
        unsigned long long us = (unsigned long long)CAL_TICKS * 1000000ull / PIT_HZ;
        if (!us) return 0;
        return cycles / us;                  /* cycles per microsecond */
    }
}

/* ---- legacy table discovery -----------------------------------------------
 * Both tables are found by scanning fixed low-memory windows for a signature,
 * which is how every pre-UEFI operating system did it and how every BIOS still
 * publishes them. The windows are architectural:
 *
 *   RSDP    the first KB of the EBDA (whose segment is at 0x40:0x0E), then the
 *           BIOS ROM area 0xE0000-0xFFFFF, both on 16-byte boundaries.
 *   SMBIOS  0xF0000-0xFFFFF on 16-byte boundaries.
 *
 * A checksum is verified in both cases rather than trusting the signature
 * alone: these windows contain arbitrary ROM data, and "RSD PTR " appearing by
 * chance inside a BIOS string table is not a fantasy - it is why the spec asks
 * for the checksum.
 */
static int sum_ok(const unsigned char *p, unsigned len)
{
    unsigned char s = 0;
    unsigned i;
    for (i = 0; i < len; i++) s = (unsigned char)(s + p[i]);
    return s == 0;
}

static void *scan(unsigned long long start, unsigned long long end,
                  const char *sig, unsigned siglen, unsigned sumlen)
{
    unsigned long long a;
    for (a = start; a + sumlen <= end; a += 16) {
        const unsigned char *p = (const unsigned char *)(unsigned long long)a;
        if (memcmp(p, sig, siglen) != 0) continue;
        if (sumlen && !sum_ok(p, sumlen)) continue;
        return (void *)p;
    }
    return 0;
}

void *uno_bios_find_rsdp(void)
{
    /* the EBDA segment word lives in the BIOS data area at 0x40:0x0E */
    unsigned short ebda_seg = *(volatile unsigned short *)0x40EU;
    void *p = 0;
    if (ebda_seg) {
        unsigned long long ebda = (unsigned long long)ebda_seg << 4;
        p = scan(ebda, ebda + 1024, "RSD PTR ", 8, 20);
    }
    if (!p) p = scan(0xE0000ull, 0x100000ull, "RSD PTR ", 8, 20);
    /* Only the first 20 bytes are checksummed above, which is the ACPI 1.0
     * structure. That is deliberate: a 2.0+ RSDP has a SECOND checksum over its
     * full length, and validating that here would reject an otherwise usable
     * table on firmware that gets the extended field wrong. The consumer reads
     * the revision and decides. */
    return p;
}

void *uno_bios_find_smbios(void)
{
    void *p = scan(0xF0000ull, 0x100000ull, "_SM3_", 5, 24);   /* 3.0 first */
    if (!p) p = scan(0xF0000ull, 0x100000ull, "_SM_", 4, 0);
    return p;
}

/* ---- the E820 map ---------------------------------------------------------
 * Only one consumer so far: the module loader needs a physical region it can
 * make executable, which on the UEFI path is an AllocatePages call. Returns the
 * base of the highest usable run of at least `bytes`, above 1 MB and below
 * 4 GiB - above 1 MB because the kernel and everything below it is already
 * spoken for, below 4 GiB because that is all the loader's page tables map.
 */
unsigned long long uno_bios_find_ram(const uno_bootinfo *bi, unsigned long long bytes)
{
    const uno_e820 *e;
    unsigned i;
    unsigned long long best = 0;
    if (!bi || !bi->mmap_addr || !bi->mmap_count) return 0;
    e = (const uno_e820 *)(unsigned long long)bi->mmap_addr;
    for (i = 0; i < bi->mmap_count; i++) {
        unsigned long long b = e[i].base, l = e[i].length, top;
        if (e[i].type != UNO_E820_USABLE) continue;
        if (b < 0x200000ull) {                  /* keep clear of the kernel */
            if (l <= 0x200000ull - b) continue;
            l -= 0x200000ull - b;
            b = 0x200000ull;
        }
        top = b + l;
        if (top > 0x100000000ull) top = 0x100000000ull;   /* mapped range only */
        if (top <= b || top - b < bytes) continue;
        /* take the TOP of the run: the bottom of high memory is where a kernel
         * that grows will grow into */
        if (top - bytes > best) best = top - bytes;
    }
    return best & ~0xFFFFull;
}
