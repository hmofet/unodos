/* ===========================================================================
 * UnoDOS/pc64 DEBUG BUILD core - see uno_debug.h for the contract.
 *
 * The one-sentence version: pc64 previously installed no IDT, so every fault
 * triple-faulted the CPU and the machine silently rebooted with nothing to
 * read.  This file hooks the CPU exception vectors (the firmware's IDT while
 * attached, our own full IDT once detached), captures registers + a stack
 * backtrace + the kernel log tail + the boot environment block, persists the
 * lot (warm-reset-surviving RAM stash first, the FAT32 boot volume when it is
 * safe to touch, QEMU debugcon when built with -DUNO_DBGCON), and resets.
 * A watchdog (firmware timer event while attached, LAPIC timer once
 * detached) catches the freeze class the fault path can't see.
 *
 * Compiled with -mgeneral-regs-only: no SSE/x87 anywhere in this file, so
 * the interrupt paths that RETURN (watchdog tick, spurious) don't have to
 * save vector state.  Paths that persist a report never return (reset), so
 * clobbering the interrupted context there is fine.
 * ======================================================================== */
#ifdef UNO_DEBUG

#include "uefi.h"
#include "fat.h"
#include "pc64_fs.h"
#include "pc64_native.h"
#include "uno_debug.h"
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

int  snprintf(char *buf, size_t n, const char *fmt, ...);
int  vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap);
void uno_heap_stats(unsigned long *used, unsigned long *free_,
                    unsigned long *largest);                /* pc64_libc.c   */
void uno_pc64_ptr_status(int *nsimple, int *nabs, int *blocked);
int  uno_pc64_time(int *y, int *mo, int *d, int *h, int *mi, int *s);
int  uno_pc64_detached(void);
void uno_pc64_dbg_display(unsigned long long *base, int *w, int *h,
                          int *stride, int *pixfmt, int *useblt,
                          int *dtw, int *dth, int *outw, int *outh);
void uno_pc64_dbg_invalidate(void);     /* force a full VRAM rewrite next frame */
int  uno_pc64_dbg_blt_bench(unsigned long long *cycles, unsigned long *bytes);
void uno_pc64_dbg_bench_cleanup(void);
void uno_i2c_hid_status(int *nbars, int *nctrl, int *present, int *addr, int *parsed);
void uno_usb_hid_status(int *nkbd, int *nmouse);
void uno_ps2_status(int *kbd, int *aux, int *auxport, int *auxid);

/* generated symbol table (tools/mksyms.py -> build/dbg_syms.c; the stub in
 * tools/dbg_syms_stub.c has n = 0 and reports fall back to raw RVAs) */
extern const unsigned int uno_dbgsym_n;
extern const unsigned int uno_dbgsym_rva[];
extern const unsigned int uno_dbgsym_off[];
extern const char         uno_dbgsym_str[];

extern char __ImageBase[];              /* mingw linker-provided PE base */

/* ---- tiny port I/O (no dependency on uefi_main's statics) ---------------- */
static inline void d_outb(unsigned short port, unsigned char v)
{ __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port)); }

#ifdef UNO_DBGCON
static void d_dbgcon(const char *s) { while (*s) d_outb(0x402, (unsigned char)*s++); }
#else
static void d_dbgcon(const char *s) { (void)s; }
#endif

static inline unsigned long long d_rdmsr(unsigned int msr)
{
    unsigned int lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((unsigned long long)hi << 32) | lo;
}
static inline void d_wrmsr(unsigned int msr, unsigned long long v)
{
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"((unsigned int)v),
                      "d"((unsigned int)(v >> 32)));
}

/* ===========================================================================
 * The RAM stash - a fixed-physical-address block that a CF9 warm reset does
 * not clear on most machines.  Everything in it is best-effort (the next
 * boot's firmware may scribble); a CRC gates every read.  Layout:
 *   header | pending crash report | kernel log ring
 * The log ring is written CONTINUOUSLY, so even a triple fault (which writes
 * no report at all) leaves the log tail for the next boot to salvage.
 * ======================================================================== */
#define STASH_MAGIC   0x47424455u       /* 'UDBG' */
#define STASH_VER     1
#define STASH_PAGES   16                /* 64 KiB */
#define CRASH_MAX     40960
#define LOG_RING      23040
#define STASH_HDR     1024

typedef struct {
    uint32_t magic, ver;
    uint32_t clean;                     /* 1 = last boot shut down on purpose */
    uint32_t boot_count;
    uint32_t crash_len;                 /* pending report bytes (0 = none)    */
    uint32_t crash_kind;                /* 'C' exception  'H' hang  'P' panic */
    uint32_t crash_crc;
    uint32_t log_widx, log_wrapped;
    uint32_t log_crc_dummy;             /* ring is not CRCed - torn tails OK  */
    char     last_check[64];            /* most recent uno_dbg_check tag      */
    uint64_t last_check_ms;
    char     pad[STASH_HDR - 64 - 8 - 10 * 4];
} DbgStashHdr;

typedef struct {
    DbgStashHdr h;
    char crash[CRASH_MAX];
    char log[LOG_RING];
} DbgStash;

/* physical addresses tried in order; the next boot scans all of them */
static const unsigned long long kStashAddr[] =
    { 0x01F00000ull, 0x03F00000ull, 0x07F00000ull };
#define NSTASH ((int)(sizeof kStashAddr / sizeof kStashAddr[0]))

static DbgStash *g_stash;               /* the live stash (RAM or fallback)  */
static DbgStash  g_stash_bss;           /* fallback when no fixed addr free  */
static EFI_SYSTEM_TABLE *g_st;
static unsigned long long g_tsc0, g_tsc_per_ms;
static char g_env[4096];                /* the boot environment block        */
static int  g_env_len;
static char g_report[CRASH_MAX];        /* built here, then persisted        */
static volatile int g_in_trap;          /* nested-fault guard                */
static volatile int g_in_disk;          /* crash-writer disk reentry guard   */
static int  g_crash_vol = -1;           /* fat.c volume for CRASH\ (cached)  */
static int  g_crash_seen;               /* CRASH\ report count at boot       */
static int  g_hud_on = 1;
volatile unsigned int g_dbg_spurious;   /* stray interrupt count (detached)  */
int uno_dbg_oom_every;                  /* fail every Nth malloc (0 = off)   */
static unsigned int g_oom_ctr, g_oom_injected;
static unsigned int g_notes;            /* uno_dbg_note count                */
static volatile unsigned long long g_heartbeat_ms;
static int g_wd_timeout_s = 20;         /* stall threshold                   */
static volatile int g_wd_armed;
static const char *g_build_id =
#ifdef UNO_BUILD_ID
    UNO_BUILD_ID;
#else
    "debug-unknown";
#endif

static uint32_t d_crc32(const void *p, unsigned long n)
{
    const unsigned char *s = p;
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        int k; c ^= *s++;
        for (k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

unsigned long long uno_dbg_uptime_ms(void)
{
    if (!g_tsc_per_ms) return 0;
    return (uno_native_rdtsc() - g_tsc0) / g_tsc_per_ms;
}
const char *uno_dbg_build_id(void) { return g_build_id; }
int uno_dbg_crash_count(void) { return g_crash_seen; }

/* ===========================================================================
 * kernel log ring
 * ======================================================================== */
static void log_raw(const char *s, int n)
{
    DbgStash *st = g_stash;
    int i;
    if (!st) return;
    for (i = 0; i < n; i++) {
        st->log[st->h.log_widx] = s[i];
        if (++st->h.log_widx >= LOG_RING) { st->h.log_widx = 0; st->h.log_wrapped = 1; }
    }
}

void uno_dbg_log(const char *fmt, ...)
{
    char line[256];
    int n;
    unsigned long long ms = uno_dbg_uptime_ms();
    va_list ap;
    n = snprintf(line, sizeof line, "[%6u.%03u] ",
                 (unsigned)(ms / 1000), (unsigned)(ms % 1000));
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof line - (unsigned)n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n'; line[n] = 0;
    log_raw(line, n);
    d_dbgcon(line);
}

void uno_dbg_note(const char *fmt, ...)
{
    char line[224];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    g_notes++;
    uno_dbg_log("NOTE: %s", line);
}

void uno_dbg_check(const char *tag)
{
    DbgStash *st = g_stash;
    int i;
    if (!st) return;
    for (i = 0; i < 63 && tag[i]; i++) st->h.last_check[i] = tag[i];
    st->h.last_check[i] = 0;
    st->h.last_check_ms = uno_dbg_uptime_ms();
}

static volatile unsigned g_hb_feeds;    /* how many times the loop fed us     */

void uno_dbg_heartbeat(void)
{
    static unsigned long long next_log;
    g_heartbeat_ms = uno_dbg_uptime_ms();
    g_hb_feeds++;
    /* refresh the boot log every 30 s so a machine that misbehaves WITHOUT
     * crashing still leaves an up-to-date trace on disk (the operator's only
     * exit from such a run is a power-off, which captures nothing else). */
    if (g_env_len && g_heartbeat_ms > next_log) {
        next_log = g_heartbeat_ms + 30000;
        uno_dbg_write_bootlog();
    }
}

/* ===========================================================================
 * symbolization - RVA -> "name+0xoff" via the generated table
 * ======================================================================== */
static unsigned long g_image_size;

static void image_extent(void)
{
    /* PE: e_lfanew at +0x3C -> "PE\0\0" -> COFF(20) -> OptionalHeader;
     * SizeOfImage at OptionalHeader+0x38 */
    unsigned char *b = (unsigned char *)__ImageBase;
    uint32_t peoff = *(uint32_t *)(b + 0x3C);
    if (peoff < 0x10000 && b[peoff] == 'P' && b[peoff + 1] == 'E')
        g_image_size = *(uint32_t *)(b + peoff + 24 + 0x38);
    if (!g_image_size) g_image_size = 8u << 20;     /* generous fallback */
}

static int in_image(unsigned long long a)
{
    return a >= (unsigned long long)(uintptr_t)__ImageBase &&
           a <  (unsigned long long)(uintptr_t)__ImageBase + g_image_size;
}

/* writes "name+0x12 [rva 0x3456]" or "[rva 0x3456]" or "outside image" */
static int sym_name(char *out, int max, unsigned long long addr)
{
    unsigned int rva, lo = 0, hi = uno_dbgsym_n;
    if (!in_image(addr))
        return snprintf(out, (size_t)max, "(outside image: %llx)", addr);
    rva = (unsigned int)(addr - (uintptr_t)__ImageBase);
    if (!uno_dbgsym_n)
        return snprintf(out, (size_t)max, "[rva 0x%x]", rva);
    while (lo + 1 < hi) {               /* last entry with rva[] <= rva */
        unsigned int mid = (lo + hi) / 2;
        if (uno_dbgsym_rva[mid] <= rva) lo = mid; else hi = mid;
    }
    if (uno_dbgsym_rva[lo] > rva)
        return snprintf(out, (size_t)max, "[rva 0x%x]", rva);
    return snprintf(out, (size_t)max, "%s+0x%x [rva 0x%x]",
                    uno_dbgsym_str + uno_dbgsym_off[lo],
                    rva - uno_dbgsym_rva[lo], rva);
}

/* ===========================================================================
 * report building
 * ======================================================================== */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vec, err;
    uint64_t rip, cs, rflags, rsp, ss;
} DbgFrame;

static const char *vec_name(int v)
{
    static const char *k[] = {
        "#DE divide error", "#DB debug", "NMI", "#BP breakpoint",
        "#OF overflow", "#BR bound range", "#UD invalid opcode",
        "#NM device n/a", "#DF double fault", "coproc", "#TS invalid TSS",
        "#NP segment not present", "#SS stack fault",
        "#GP general protection", "#PF page fault", "r15", "#MF x87",
        "#AC alignment check", "#MC machine check", "#XM SIMD", "#VE", "#CP"
    };
    if (v >= 0 && v < (int)(sizeof k / sizeof k[0])) return k[v];
    return "?";
}

static int g_rlen;
static void rp(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (g_rlen >= (int)sizeof g_report - 2) return;
    va_start(ap, fmt);
    n = vsnprintf(g_report + g_rlen, sizeof g_report - (unsigned)g_rlen - 1, fmt, ap);
    va_end(ap);
    if (n > 0) g_rlen += n;
    if (g_rlen > (int)sizeof g_report - 2) g_rlen = (int)sizeof g_report - 2;
}

static void rp_common_head(const char *kind)
{
    unsigned long hu = 0, hf = 0, hl = 0;
    int y, mo, d, h, mi, s;
    rp("==== UNODOS PC64 %s REPORT ====\n", kind);
    rp("build: %s\n", g_build_id);
    if (uno_pc64_time(&y, &mo, &d, &h, &mi, &s))
        rp("clock: %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
    rp("uptime_ms: %llu   detached: %d   image_base: %llx\n",
       uno_dbg_uptime_ms(), uno_pc64_detached(),
       (unsigned long long)(uintptr_t)__ImageBase);
    uno_heap_stats(&hu, &hf, &hl);
    rp("heap: used=%lu free=%lu largest=%lu   oom_injected=%u  notes=%u  spurious_irq=%u\n",
       hu, hf, hl, g_oom_injected, g_notes, g_dbg_spurious);
    if (g_stash)
        rp("last_checkpoint: %s @ %llums\n",
           g_stash->h.last_check[0] ? g_stash->h.last_check : "(none)",
           g_stash->h.last_check_ms);
    /* if we died INSIDE a window's draw callback, name the window - that is
     * a far sharper lead than the last checkpoint (F11) */
    if (uno_dbg_current_window())
        rp("in-draw window: %s\n", uno_dbg_current_window());
}

static void rp_log_tail(void)
{
    DbgStash *st = g_stash;
    rp("\n---- kernel log (tail) ----\n");
    if (!st) { rp("(no stash)\n"); return; }
    if (st->h.log_wrapped) {
        /* the ring wrapped: emit widx..end then 0..widx */
        unsigned int i;
        for (i = st->h.log_widx; i < LOG_RING; i++)
            if (st->log[i] && g_rlen < (int)sizeof g_report - 2)
                g_report[g_rlen++] = st->log[i];
    }
    { unsigned int i;
      for (i = 0; i < st->h.log_widx; i++)
          if (st->log[i] && g_rlen < (int)sizeof g_report - 2)
              g_report[g_rlen++] = st->log[i]; }
    g_report[g_rlen] = 0;
}

static void rp_env(void)
{
    rp("\n---- boot environment ----\n");
    if (g_env_len) rp("%s", g_env);
    else           rp("(env block not built yet - crash before init end)\n");
}

static void rp_frame(const DbgFrame *f)
{
    char nb[160];
    unsigned long long cr2 = 0, cr0, cr3, cr4;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    rp("\nexception: vector %llu (%s)  err=0x%llx",
       f->vec, vec_name((int)f->vec), f->err);
    if (f->vec == 14)
        rp("  CR2=0x%llx (%s%s%s%s)", cr2,
           (f->err & 1) ? "prot" : "not-present",
           (f->err & 2) ? " write" : " read",
           (f->err & 4) ? " user" : "",
           (f->err & 16) ? " ifetch" : "");
    rp("\n");
    sym_name(nb, sizeof nb, f->rip);
    rp("RIP: %llx  %s\n", f->rip, nb);
    /* a ud2 at RIP is a compiled-in sanitizer trap (-fsanitize-undefined-
     * trap-on-error) or __builtin_trap - call it out, it is the whole point */
    if (f->vec == 6 && in_image(f->rip)) {
        const unsigned char *ip = (const unsigned char *)(uintptr_t)f->rip;
        if (ip[0] == 0x0F && ip[1] == 0x0B)
            rp("  ^^ ud2: UBSAN TRAP (signed overflow / OOB index / bad shift /\n"
               "     null deref caught by the compiler) or __builtin_trap()\n");
    }
    rp("RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n",
       f->rax, f->rbx, f->rcx, f->rdx);
    rp("RSI=%016llx RDI=%016llx RBP=%016llx RSP=%016llx\n",
       f->rsi, f->rdi, f->rbp, f->rsp);
    rp("R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n",
       f->r8, f->r9, f->r10, f->r11);
    rp("R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n",
       f->r12, f->r13, f->r14, f->r15);
    rp("CS=%llx SS=%llx RFLAGS=%llx CR0=%llx CR3=%llx CR4=%llx\n",
       f->cs, f->ss, f->rflags, cr0, cr3, cr4);

    /* backtrace: RBP chain (built with -fno-omit-frame-pointer).  Bounds:
     * each frame pointer must sit within +-1 MiB of the faulting RSP and
     * strictly increase, each return address must land inside the image. */
    rp("\n---- backtrace (rbp chain) ----\n");
    {
        unsigned long long bp = f->rbp, lo = f->rsp - (1u << 20),
                           hi = f->rsp + (1u << 20);
        int depth;
        for (depth = 0; depth < 32; depth++) {
            unsigned long long ret, nbp;
            if (bp < lo || bp > hi - 16 || (bp & 7)) break;
            nbp = *(unsigned long long *)(uintptr_t)bp;
            ret = *(unsigned long long *)(uintptr_t)(bp + 8);
            if (!in_image(ret)) break;
            sym_name(nb, sizeof nb, ret);
            rp("  #%02d %llx  %s\n", depth, ret, nb);
            if (nbp <= bp) break;
            bp = nbp;
        }
        if (!depth) rp("  (no walkable frames - leaf fault or clobbered rbp)\n");
    }

    /* raw stack: 384 bytes from RSP, any in-image qword symbolized - catches
     * what the rbp walk misses (smashed frames are exactly when we need it) */
    rp("\n---- raw stack (in-image qwords symbolized) ----\n");
    {
        unsigned long long *sp = (unsigned long long *)(uintptr_t)(f->rsp & ~7ull);
        int i;
        for (i = 0; i < 48; i++) {
            unsigned long long v = sp[i];
            if (in_image(v)) {
                sym_name(nb, sizeof nb, v);
                rp("  rsp+%03x: %016llx  %s\n", i * 8, v, nb);
            }
        }
    }
}

/* ===========================================================================
 * persistence
 * ======================================================================== */
static void stash_report(int kind)
{
    DbgStash *st = g_stash;
    int n = g_rlen;
    if (!st) return;
    if (n > CRASH_MAX - 1) n = CRASH_MAX - 1;
    memcpy(st->crash, g_report, (size_t)n);
    st->crash[n] = 0;
    st->h.crash_len  = (uint32_t)n;
    st->h.crash_kind = (uint32_t)kind;
    st->h.crash_crc  = d_crc32(st->crash, (unsigned long)n);
}

/* pick (and cache) the FAT volume that holds CRASH\ - the boot volume in the
 * flasher's whole-disk layout.  Called from safe context at boot. */
static int crash_vol(void)
{
    /* P2.2: cache FAILURE too (with a bounded retry), not just success. On a
     * machine where no volume qualifies (F8/F9) every caller used to re-scan
     * every volume - and the heartbeat calls this every 30 s, which is
     * exactly the repeated firmware-BlockIO grind that made the MacBook feel
     * "extremely slow". Retry only every 64th failed call so late-appearing
     * storage still gets picked up. */
    static unsigned fail_backoff;
    int v, n;
    if (g_crash_vol >= 0) return g_crash_vol;
    if (fail_backoff) { fail_backoff--; return -1; }
    n = uno_fat_volumes();
    for (v = 0; v < n; v++) {
        /* uno_fat_list returns 0 for both "empty" and "missing", so probe the
         * ROOT listing (which includes subdir entries) for a real CRASH dir */
        uno_fat_entry e[64];
        int i, cnt = uno_fat_list_ex(v, "", e, 64);
        for (i = 0; i < cnt && i < 64; i++)
            if (e[i].is_dir && !strcmp(e[i].name, "CRASH")) { g_crash_vol = v; break; }
        if (g_crash_vol >= 0) break;
        if (uno_fat_mkdir(v, "CRASH")) { g_crash_vol = v; break; }
    }
    if (g_crash_vol < 0) fail_backoff = 63;
    return g_crash_vol;
}

/* ===========================================================================
 * machine identity (SMBIOS) -> per-machine telemetry folder
 *
 * One stick now serves a whole batch of machines: every telemetry file lands
 * under CRASH\<MACHINE>\ so results from different laptops never overwrite
 * each other (the reuse hazard documented in METAL-FINDINGS - stale telemetry
 * is indistinguishable from a failed run).  The name comes from SMBIOS Type 1
 * (System Information) via the EFI configuration table; the fleet gets short
 * stable 8.3 names, anything else a sanitized product string.
 * ======================================================================== */
void *uno_pc64_st(void);                 /* EFI_SYSTEM_TABLE (uefi_main.c) */

static char g_smb_mfr[48], g_smb_prod[48], g_smb_ver[48];
static char g_mtag[12];                  /* folder leaf, 8.3-safe          */
static char g_mdir[24];                  /* "CRASH\<TAG>"                  */

/* SMBIOS string `no` of the structure at s (strings follow the formatted
 * area, NUL separated, double-NUL terminated). `end` bounds the walk - a
 * malformed/non-double-NUL-terminated string set must not read off the table
 * into unmapped memory. */
static void smb_string(const unsigned char *s, int no, char *out, int cap,
                       const unsigned char *end)
{
    const char *p = (const char *)s + s[1];          /* past formatted area */
    const char *lim = (const char *)end;
    int i = 1;
    out[0] = 0;
    if (no <= 0) return;
    while (i < no && p < lim && *p) { while (p < lim && *p) p++; if (p < lim) p++; i++; }
    if (p >= lim || !*p) return;
    { int k = 0; while (p + k < lim && p[k] && k < cap - 1) { out[k] = p[k]; k++; } out[k] = 0; }
}

static int guid_eq(const EFI_GUID *a, const EFI_GUID *b)
{ return !memcmp(a, b, sizeof(EFI_GUID)); }

static void smb_read_type1(void)
{
    static const EFI_GUID g3 = EFI_SMBIOS3_TABLE_GUID;
    static const EFI_GUID g1 = EFI_SMBIOS_TABLE_GUID;
    EFI_SYSTEM_TABLE *ST = (EFI_SYSTEM_TABLE *)uno_pc64_st();
    const unsigned char *tab = 0;
    unsigned long len = 0;
    UINTN i;
    if (!ST || !ST->ConfigurationTable) return;
    for (i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &ST->ConfigurationTable[i];
        const unsigned char *ep = (const unsigned char *)ct->VendorTable;
        if (!ep) continue;
        if (guid_eq(&ct->VendorGuid, &g3) && !memcmp(ep, "_SM3_", 5)) {
            len = *(const unsigned int *)(ep + 0x0C);
            tab = (const unsigned char *)(uintptr_t)*(const unsigned long long *)(ep + 0x10);
            break;                                   /* 64-bit entry wins */
        }
        if (guid_eq(&ct->VendorGuid, &g1) && !memcmp(ep, "_SM_", 4) && !tab) {
            len = *(const unsigned short *)(ep + 0x16);
            tab = (const unsigned char *)(uintptr_t)*(const unsigned int *)(ep + 0x18);
        }
    }
    if (!tab || !len) return;
    {   /* walk to Type 1; strings live between formatted areas */
        const unsigned char *s = tab, *end = tab + len;
        while (s + 4 <= end && s[0] != 127 && s[1] >= 4) {
            const unsigned char *nx = s + s[1];
            /* Type 1 needs s[4..6] (mfr/product/version string indices); only
             * read them if the formatted area is actually that long AND in
             * bounds, else a truncated tail record reads past `end`. */
            if (s[0] == 1 && s[1] >= 7 && s + 7 <= end) {
                smb_string(s, s[4], g_smb_mfr,  sizeof g_smb_mfr,  end);
                smb_string(s, s[5], g_smb_prod, sizeof g_smb_prod, end);
                smb_string(s, s[6], g_smb_ver,  sizeof g_smb_ver,  end);
                return;
            }
            while (nx + 1 < end && (nx[0] || nx[1])) nx++;   /* skip strings */
            s = nx + 2;
        }
    }
}

static int ci_contains(const char *hay, const char *needle)
{
    int i, j;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j]; j++) {
            char a = hay[i + j], b = needle[j];
            if (a >= 'a' && a <= 'z') a -= 32;
            if (b >= 'a' && b <= 'z') b -= 32;
            if (a != b) break;
        }
        if (!needle[j]) return 1;
    }
    return 0;
}

const char *uno_dbg_machine_tag(void)
{
    if (g_mtag[0]) return g_mtag;
    smb_read_type1();
    {   /* the fleet first (Lenovo puts the friendly name in Version, the
         * machine-type code in Product - check both, either order) */
        static const struct { const char *needle, *tag; } kMap[] = {
            { "X1 Carbon",   "X1CARBON" },
            { "X13 Yoga",    "X13YOGA"  },
            { "Laptop Go",   "SURFGO"   },
            { "Surface",     "SURFACE"  },
            { "Latitude",    "LATITUDE" },
            { "MacBook",     "MACBOOK"  },
            { "QEMU",        "QEMU"     },
            { "Standard PC", "QEMU"     },
        };
        const char *src[3];
        int i, s;
        src[0] = g_smb_ver; src[1] = g_smb_prod; src[2] = g_smb_mfr;
        for (i = 0; i < (int)(sizeof kMap / sizeof kMap[0]); i++)
            for (s = 0; s < 3; s++)
                if (ci_contains(src[s], kMap[i].needle)) {
                    strcpy(g_mtag, kMap[i].tag);
                    return g_mtag;
                }
        /* unknown machine: sanitized product (else version, else maker) */
        for (s = 1; s >= 0 && !g_mtag[0]; s--) {   /* prod, then ver */
            const char *p = s == 1 ? g_smb_prod : g_smb_ver;
            int k = 0;
            for (i = 0; p[i] && k < 8; i++) {
                char c = p[i];
                if (c >= 'a' && c <= 'z') c -= 32;
                if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                    g_mtag[k++] = c;
            }
            g_mtag[k] = 0;
        }
        if (!g_mtag[0]) strcpy(g_mtag, "UNKNOWN");
    }
    return g_mtag;
}

/* "CRASH\<TAG>", created on first use.  Falls back to plain "CRASH" if the
 * subdir cannot be created - telemetry must never be lost to the layout. */
static const char *crash_dir(void)
{
    int vol = crash_vol();
    if (vol < 0) return 0;
    if (!g_mdir[0]) {
        const char *tag = uno_dbg_machine_tag();
        snprintf(g_mdir, sizeof g_mdir, "CRASH\\%s", tag);
        if (!uno_fat_mkdir(vol, g_mdir)) {           /* 0 = exists OR failed */
            uno_fat_entry e[64];
            int i, n = uno_fat_list_ex(vol, "CRASH", e, 64), ok = 0;
            for (i = 0; i < n && i < 64; i++)
                if (e[i].is_dir && !strcmp(e[i].name, tag)) ok = 1;
            if (!ok) snprintf(g_mdir, sizeof g_mdir, "CRASH");
        }
    }
    return g_mdir;
}

/* next free <dir>\XXnnn.TXT sequence number (by directory count - cheap and
 * monotonic enough; collisions just overwrite the same-numbered file) */
static int next_seq(int vol)
{
    uno_fat_entry e[999];
    int n = uno_fat_list_ex(vol, crash_dir(), e, 999);
    if (n < 0) n = 0;
    if (n > 998) n = 998;
    return n + 1;
}

static int disk_write_report(int kind)
{
    char path[40];
    const char *pfx = (kind == 'H') ? "HG" : (kind == 'P') ? "PN" :
                      (kind == 'R') ? "RS" : "CR";
    int vol = crash_vol();
    if (vol < 0) return 0;
    snprintf(path, sizeof path, "%s\\%s%03d.TXT", crash_dir(), pfx, next_seq(vol));
    if (!uno_fat_write(vol, path, (const unsigned char *)g_report,
                       (long)g_rlen))
        return 0;
    uno_fat_sync();
    return 1;
}

/* Named file under the machine's telemetry dir (the net test's NETLOG).
 * Whole-file rewrite each call - callers flush a growing buffer. */
int uno_dbg_write_crashfile(const char *name, const void *data, int len)
{
    char path[48];
    int vol = crash_vol();
    if (vol < 0) return 0;
    snprintf(path, sizeof path, "%s\\%s", crash_dir(), name);
    if (!uno_fat_write(vol, path, (const unsigned char *)data, (long)len))
        return 0;
    uno_fat_sync();
    return 1;
}

/* ===========================================================================
 * the exception path
 * ======================================================================== */
/* firmware ResetSystem (a runtime service): enough of the struct to reach it.
 * Layout matches uefi_main.c's EFI_RUNTIME_SERVICES. */
typedef struct {
    unsigned char Hdr[24];
    void *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime;
    void *SetVirtualAddressMap, *ConvertPointer;
    void *GetVariable, *GetNextVariableName, *SetVariable, *GetNextHighMonotonicCount;
    void (*ResetSystem)(int, EFI_STATUS, UINTN, void *);
} DbgRuntimeServices;

static void trap_reset(void)
{
    uno_native_delay_us(2000000);       /* 2 s: let debugcon/HDMI show it */
    /* An ATTACHED machine may ignore CF9 + the i8042 pulse - the Surface did
     * (CR003 captured fine, then black-screened because uno_native_reset never
     * took, leaving the firmware OSK drawn).  While attached the firmware still
     * owns the platform, so use its ResetSystem runtime service first; the
     * native CF9 path is the only option once detached, and the fallback. */
    if (!uno_pc64_detached() && g_st) {
        DbgRuntimeServices *rs = (DbgRuntimeServices *)g_st->RuntimeServices;
        /* WARM, not cold: an attached machine cannot write a HANG report from
         * the firmware timer callback (wrong TPL to touch Block IO), so that
         * report lives only in the RAM stash until the next boot flushes it.
         * A cold reset re-inits DRAM and would throw it away; a warm reset
         * preserves memory, which is what makes hang reports collectable on
         * attached machines like the Surface.  Falls through to the native
         * path below if the firmware declines. */
        if (rs && rs->ResetSystem) {
            rs->ResetSystem(1 /*EfiResetWarm*/, 0, 0, 0);
            rs->ResetSystem(0 /*EfiResetCold*/, 0, 0, 0);   /* warm refused */
        }
    }
    uno_native_reset();                 /* CF9 + i8042 pulse; then hlt */
}

void uno_dbg_trap(DbgFrame *f);         /* referenced from the asm stubs */
void uno_dbg_trap(DbgFrame *f)
{
    int trap_no = ++g_in_trap;

    if (trap_no >= 3)                   /* third fault: give up entirely   */
        uno_native_reset();

    if (trap_no == 2) {
        /* fault while persisting the first report: the stash already holds
         * it; skip whatever hurt us (almost certainly the disk) and reset. */
        trap_reset();
    }

    g_rlen = 0;
    rp_common_head("CRASH");
    rp_frame(f);
    rp_env();
    rp_log_tail();

    stash_report('C');
    d_dbgcon("\n==== CRASH (full report follows) ====\n");
    d_dbgcon(g_report);

    /* Disk: only when it looks safe - never when the fault interrupted
     * firmware code (it may hold storage locks), never re-entrantly from
     * the crash writer itself.  The stash + next-boot flush covers the
     * cases we skip. */
    if (!g_in_disk && in_image(f->rip))
        disk_write_report('C');

    trap_reset();
}

void uno_dbg_panic(const char *why)
{
    if (++g_in_trap >= 2) trap_reset();
    g_rlen = 0;
    rp_common_head("PANIC");
    rp("\npanic: %s\n", why ? why : "?");
    /* capture our own backtrace: current rbp chain */
    {
        char nb[160];
        unsigned long long bp;
        int depth;
        __asm__ volatile ("mov %%rbp, %0" : "=r"(bp));
        rp("\n---- backtrace ----\n");
        for (depth = 0; depth < 32 && bp && !(bp & 7); depth++) {
            unsigned long long ret = *(unsigned long long *)(uintptr_t)(bp + 8);
            unsigned long long nbp = *(unsigned long long *)(uintptr_t)bp;
            if (!in_image(ret)) break;
            sym_name(nb, sizeof nb, ret);
            rp("  #%02d %llx  %s\n", depth, ret, nb);
            if (nbp <= bp) break;
            bp = nbp;
        }
    }
    rp_env();
    rp_log_tail();
    stash_report('P');
    d_dbgcon("\n==== PANIC ====\n");
    d_dbgcon(g_report);
    if (!g_in_disk) disk_write_report('P');
    trap_reset();
}

/* stack-protector runtime (-fstack-protector-strong lands here) */
unsigned long long __stack_chk_guard = 0xBADC0FFEE0DDF00Dull;
void __stack_chk_fail(void);
void __stack_chk_fail(void)
{ uno_dbg_panic("stack smashed (__stack_chk_fail: canary overwritten)"); }

/* ===========================================================================
 * exception stubs + IDT plumbing
 * ======================================================================== */
#define STUB_NOERR(n) __asm__(                                   \
    ".globl dbg_vec" #n "\n"                                     \
    "dbg_vec" #n ":\n"                                           \
    "  pushq $0\n  pushq $" #n "\n  jmp dbg_trap_common\n");
#define STUB_ERR(n) __asm__(                                     \
    ".globl dbg_vec" #n "\n"                                     \
    "dbg_vec" #n ":\n"                                           \
    "  pushq $" #n "\n  jmp dbg_trap_common\n");

STUB_NOERR(0)  STUB_NOERR(1)  STUB_NOERR(2)  STUB_NOERR(3)
STUB_NOERR(4)  STUB_NOERR(5)  STUB_NOERR(6)  STUB_NOERR(7)
STUB_ERR(8)    STUB_NOERR(9)  STUB_ERR(10)   STUB_ERR(11)
STUB_ERR(12)   STUB_ERR(13)   STUB_ERR(14)   STUB_NOERR(15)
STUB_NOERR(16) STUB_ERR(17)   STUB_NOERR(18) STUB_NOERR(19)
STUB_NOERR(20) STUB_ERR(21)   STUB_NOERR(22) STUB_NOERR(23)
STUB_NOERR(24) STUB_NOERR(25) STUB_NOERR(26) STUB_NOERR(27)
STUB_NOERR(28) STUB_ERR(29)   STUB_ERR(30)   STUB_NOERR(31)

__asm__(
".globl dbg_trap_common\n"
"dbg_trap_common:\n"
"  cli\n"
"  pushq %rax\n  pushq %rcx\n  pushq %rdx\n  pushq %rbx\n"
"  pushq %rbp\n  pushq %rsi\n  pushq %rdi\n"
"  pushq %r8\n   pushq %r9\n   pushq %r10\n  pushq %r11\n"
"  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
"  movq %rsp, %rcx\n"           /* frame* -> first Win64 arg */
"  andq $-16, %rsp\n"
"  subq $32, %rsp\n"            /* shadow space */
"  call uno_dbg_trap\n"
"1:hlt\n  jmp 1b\n");

/* interrupts that RETURN (detached-mode IDT only): spurious + LAPIC timer.
 * Volatile GPRs only - this file is -mgeneral-regs-only, so the C handlers
 * touch no vector registers. */
__asm__(
".globl dbg_vec_spur\n"
"dbg_vec_spur:\n"
"  pushq %rax\n  pushq %rcx\n  pushq %rdx\n"
"  pushq %r8\n   pushq %r9\n   pushq %r10\n  pushq %r11\n"
"  movq %rsp, %rax\n  andq $-16, %rsp\n  pushq %rax\n  subq $40, %rsp\n"
"  call dbg_spur_c\n"
"  addq $40, %rsp\n  popq %rsp\n"
"  popq %r11\n  popq %r10\n  popq %r9\n  popq %r8\n"
"  popq %rdx\n  popq %rcx\n  popq %rax\n"
"  iretq\n"
".globl dbg_vec_timer\n"
"dbg_vec_timer:\n"
"  pushq %rax\n  pushq %rcx\n  pushq %rdx\n"
"  pushq %r8\n   pushq %r9\n   pushq %r10\n  pushq %r11\n"
"  movq %rsp, %rax\n  andq $-16, %rsp\n  pushq %rax\n  subq $40, %rsp\n"
"  call dbg_timer_c\n"
"  addq $40, %rsp\n  popq %rsp\n"
"  popq %r11\n  popq %r10\n  popq %r9\n  popq %r8\n"
"  popq %rdx\n  popq %rcx\n  popq %rax\n"
"  iretq\n");

extern void dbg_vec0(void);  extern void dbg_vec1(void);  extern void dbg_vec2(void);
extern void dbg_vec3(void);  extern void dbg_vec4(void);  extern void dbg_vec5(void);
extern void dbg_vec6(void);  extern void dbg_vec7(void);  extern void dbg_vec8(void);
extern void dbg_vec9(void);  extern void dbg_vec10(void); extern void dbg_vec11(void);
extern void dbg_vec12(void); extern void dbg_vec13(void); extern void dbg_vec14(void);
extern void dbg_vec15(void); extern void dbg_vec16(void); extern void dbg_vec17(void);
extern void dbg_vec18(void); extern void dbg_vec19(void); extern void dbg_vec20(void);
extern void dbg_vec21(void); extern void dbg_vec22(void); extern void dbg_vec23(void);
extern void dbg_vec24(void); extern void dbg_vec25(void); extern void dbg_vec26(void);
extern void dbg_vec27(void); extern void dbg_vec28(void); extern void dbg_vec29(void);
extern void dbg_vec30(void); extern void dbg_vec31(void);
extern void dbg_vec_spur(void); extern void dbg_vec_timer(void);

static void (*const kVec[32])(void) = {
    dbg_vec0,  dbg_vec1,  dbg_vec2,  dbg_vec3,  dbg_vec4,  dbg_vec5,
    dbg_vec6,  dbg_vec7,  dbg_vec8,  dbg_vec9,  dbg_vec10, dbg_vec11,
    dbg_vec12, dbg_vec13, dbg_vec14, dbg_vec15, dbg_vec16, dbg_vec17,
    dbg_vec18, dbg_vec19, dbg_vec20, dbg_vec21, dbg_vec22, dbg_vec23,
    dbg_vec24, dbg_vec25, dbg_vec26, dbg_vec27, dbg_vec28, dbg_vec29,
    dbg_vec30, dbg_vec31
};

typedef struct {
    uint16_t off0, sel;
    uint8_t  ist, attr;
    uint16_t off1;
    uint32_t off2, rsv;
} __attribute__((packed)) IdtGate;

typedef struct { uint16_t limit; uint64_t base; } __attribute__((packed)) Idtr;

static uint16_t cur_cs(void)
{ uint16_t cs; __asm__ volatile ("mov %%cs, %0" : "=r"(cs)); return cs; }

static void set_gate(IdtGate *g, void (*fn)(void), uint16_t cs)
{
    uint64_t a = (uint64_t)(uintptr_t)fn;
    g->off0 = (uint16_t)a;
    g->sel  = cs;
    g->ist  = 0;
    g->attr = 0x8E;                     /* present, DPL0, interrupt gate */
    g->off1 = (uint16_t)(a >> 16);
    g->off2 = (uint32_t)(a >> 32);
    g->rsv  = 0;
}

/* patch the CPU exception vectors in the IDT the firmware installed - its
 * timer/event vectors (>=32) stay untouched, so boot services keep running */
static void hook_firmware_idt(void)
{
    Idtr idtr;
    IdtGate *idt;
    uint16_t cs = cur_cs();
    int v, patched = 0;
    static const int kPatch[] = { 0, 4, 5, 6, 8, 10, 11, 12, 13, 14, 17 };
    __asm__ volatile ("sidt %0" : "=m"(idtr));
    idt = (IdtGate *)(uintptr_t)idtr.base;
    if (!idt || idtr.limit < 32 * sizeof(IdtGate) - 1) return;
    for (v = 0; v < (int)(sizeof kPatch / sizeof kPatch[0]); v++) {
        set_gate(&idt[kPatch[v]], kVec[kPatch[v]], cs);
        patched++;
    }
    uno_dbg_log("idt: hooked %d exception vectors in the firmware IDT (base %llx)",
                patched, (unsigned long long)idtr.base);
}

/* ===========================================================================
 * detached mode: our own full IDT + LAPIC watchdog timer
 * ======================================================================== */
static IdtGate g_idt[256] __attribute__((aligned(16)));
static volatile uint32_t *g_lapic;      /* xAPIC MMIO (0 when x2APIC) */
static int g_x2apic;
static unsigned int g_wd_isr_ticks;

static void lapic_wr(int reg, uint32_t v)
{
    if (g_x2apic) d_wrmsr(0x800u + (unsigned)(reg >> 4), v);
    else          g_lapic[reg / 4] = v;
}
static uint32_t lapic_rd(int reg)
{
    if (g_x2apic) return (uint32_t)d_rdmsr(0x800u + (unsigned)(reg >> 4));
    return g_lapic[reg / 4];
}

static void wd_fire(const char *how);

void dbg_spur_c(void);                  /* called from asm */
void dbg_spur_c(void)
{
    g_dbg_spurious++;
    if (g_lapic || g_x2apic) lapic_wr(0xB0, 0);
}

/* F9: the heartbeat is only fed once the SHELL's main loop runs, but the
 * watchdog arms at the end of init - on a slow machine (MacBook: slow
 * firmware + F3's uncached first paints) the gap can exceed the timeout and
 * the "watchdog" resets a machine that was merely slow, destroying the boot.
 * Until the shell has fed the heartbeat a few times, allow a generous grace. */
static unsigned g_hb_at_arm;
static unsigned long long wd_limit_ms(void)
{
    return (g_hb_feeds - g_hb_at_arm < 4) ? 120000ull
                                          : (unsigned long long)g_wd_timeout_s * 1000;
}

void dbg_timer_c(void);                 /* called from asm */
void dbg_timer_c(void)
{
    g_wd_isr_ticks++;
    if (g_wd_armed && g_tsc_per_ms) {
        unsigned long long now = uno_dbg_uptime_ms();
        unsigned long long hb  = g_heartbeat_ms;
        if (now > hb && now - hb > wd_limit_ms()) {
            lapic_wr(0xB0, 0);
            wd_fire("LAPIC watchdog (detached)");   /* resets; no return */
        }
    }
    lapic_wr(0xB0, 0);
}

void uno_dbg_on_detach(void)
{
    uint16_t cs = cur_cs();
    int v;
    unsigned long long apic_base;
    Idtr idtr;

    /* full IDT: exceptions -> report stubs, everything else -> spurious,
     * 0xE0 -> the watchdog timer */
    for (v = 0; v < 32; v++)  set_gate(&g_idt[v], kVec[v], cs);
    for (v = 32; v < 256; v++) set_gate(&g_idt[v], dbg_vec_spur, cs);
    set_gate(&g_idt[0xE0], dbg_vec_timer, cs);
    idtr.limit = sizeof g_idt - 1;
    idtr.base  = (uint64_t)(uintptr_t)g_idt;
    __asm__ volatile ("cli");
    __asm__ volatile ("lidt %0" : : "m"(idtr));

    /* silence the legacy PICs - detached firmware left whatever it left */
    d_outb(0x21, 0xFF);
    d_outb(0xA1, 0xFF);

    /* LAPIC: enable, calibrate the timer against the TSC, run periodic 1 s */
    apic_base = d_rdmsr(0x1B);
    g_x2apic  = (apic_base >> 10) & 1;
    if (!(apic_base & 0x800)) { d_wrmsr(0x1B, apic_base | 0x800); apic_base |= 0x800; }
    if (!g_x2apic)
        g_lapic = (volatile uint32_t *)(uintptr_t)(apic_base & ~0xFFFull);
    lapic_wr(0xF0, 0x1FF);              /* SVR: enable, spurious vector 0xFF  */
    lapic_wr(0x3E0, 0x0B);              /* divide by 1                        */
    lapic_wr(0x320, 0x10000);           /* LVT timer: masked while calibrating */
    lapic_wr(0x380, 0xFFFFFFFFu);       /* initial count                      */
    uno_native_delay_us(50000);
    {
        uint32_t left = lapic_rd(0x390);
        uint32_t per_50ms = 0xFFFFFFFFu - left;
        uint32_t per_s = per_50ms > 0x7FFFFFFFu / 20 ? 0xFFFFFFFFu : per_50ms * 20;
        if (per_s < 1000) per_s = 100000000;        /* nonsense reading */
        lapic_wr(0x320, 0x200E0);       /* periodic, vector 0xE0, unmasked    */
        lapic_wr(0x380, per_s);
        uno_dbg_log("wd: LAPIC timer up (%s, %u ticks/s), watchdog %ds",
                    g_x2apic ? "x2apic" : "xapic", per_s, g_wd_timeout_s);
    }
    g_hb_at_arm = g_hb_feeds;           /* F9 grace applies here too */
    uno_dbg_heartbeat();
    g_wd_armed = 1;
    __asm__ volatile ("sti");
}

/* ===========================================================================
 * watchdog - attached mode (firmware periodic timer event)
 * ======================================================================== */
static void wd_fire(const char *how)
{
    g_wd_armed = 0;
    g_rlen = 0;
    rp_common_head("HANG");
    rp("\nwatchdog: main loop silent for over %d s (%s)\n", g_wd_timeout_s, how);
    rp("heartbeat_ms: %llu   now_ms: %llu\n",
       (unsigned long long)g_heartbeat_ms, uno_dbg_uptime_ms());
    rp("The last checkpoint above is the code region that stalled.\n");
    rp_env();
    rp_log_tail();
    stash_report('H');
    d_dbgcon("\n==== HANG (watchdog) ====\n");
    d_dbgcon(g_report);
    /* Disk only when detached: our own polled AHCI/NVMe stack is safe from
     * an ISR.  While attached the firmware owns storage and we are inside
     * its timer dispatch - stash + reset, the next boot flushes it. */
    if (uno_pc64_detached() && !g_in_disk) {
        g_in_disk = 1;
        disk_write_report('H');
    }
    trap_reset();
}

typedef EFI_STATUS (*CE_FN)(UINT32, UINTN, void (*)(void *, void *), void *, void **);
typedef EFI_STATUS (*ST_FN)(void *, int, UINT64);

static void wd_event_cb(void *ev, void *ctx)
{
    (void)ev; (void)ctx;
    if (!g_wd_armed || !g_tsc_per_ms) return;
    {
        unsigned long long now = uno_dbg_uptime_ms();
        unsigned long long hb  = g_heartbeat_ms;
        if (now > hb && now - hb > wd_limit_ms())
            wd_fire("firmware timer event (attached)");
    }
}

void uno_dbg_watchdog_start(void)
{
    void *ev = 0;
    EFI_BOOT_SERVICES *bs;
    if (!g_st || uno_pc64_detached()) return;   /* detached: LAPIC path */
    bs = g_st->BootServices;
    if (((CE_FN)bs->CreateEvent)(0x80000200u /*TIMER|NOTIFY_SIGNAL*/,
                                 16 /*TPL_NOTIFY*/, wd_event_cb, 0, &ev)
        != EFI_SUCCESS || !ev) {
        uno_dbg_log("wd: CreateEvent failed - no attached watchdog");
        return;
    }
    if (((ST_FN)bs->SetTimer)(ev, 1 /*periodic*/, 20000000ull /*2 s*/)
        != EFI_SUCCESS) {
        uno_dbg_log("wd: SetTimer failed - no attached watchdog");
        return;
    }
    g_hb_at_arm = g_hb_feeds;           /* F9: grace until the shell breathes */
    uno_dbg_heartbeat();
    g_wd_armed = 1;
    uno_dbg_log("wd: firmware timer watchdog armed (%d s, 120 s until the "
                "shell's first heartbeats)", g_wd_timeout_s);
}

/* ===========================================================================
 * boot environment block
 * ======================================================================== */
static void env(const char *fmt, ...)
{
    va_list ap;
    int n;
    if (g_env_len >= (int)sizeof g_env - 2) return;
    va_start(ap, fmt);
    n = vsnprintf(g_env + g_env_len, sizeof g_env - (unsigned)g_env_len - 1, fmt, ap);
    va_end(ap);
    if (n > 0) g_env_len += n;
}

static void cpuid(unsigned int leaf, unsigned int sub,
                  unsigned int *a, unsigned int *b, unsigned int *c, unsigned int *d)
{
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                              : "a"(leaf), "c"(sub));
}

void uno_dbg_envblock(void)
{
    unsigned int a, b, c, d;
    g_env_len = 0;
    env("build: %s\n", g_build_id);
    env("machine: %s  (smbios mfr=\"%s\" product=\"%s\" version=\"%s\")\n",
        uno_dbg_machine_tag(), g_smb_mfr, g_smb_prod, g_smb_ver);

    {   /* CPU brand string */
        char brand[52];
        unsigned int *p = (unsigned int *)brand;
        int i;
        for (i = 0; i < 3; i++) {
            cpuid(0x80000002u + (unsigned)i, 0, &a, &b, &c, &d);
            *p++ = a; *p++ = b; *p++ = c; *p++ = d;
        }
        brand[48] = 0;
        env("cpu: %s\n", brand);
        cpuid(1, 0, &a, &b, &c, &d);
        env("cpuid1: eax=%x (fam/model) ecx=%x edx=%x\n", a, c, d);
    }

    {   /* display + present path */
        unsigned long long base = 0;
        int w = 0, h = 0, stride = 0, pixfmt = 0, useblt = 0,
            dtw = 0, dth = 0, outw = 0, outh = 0;
        uno_pc64_dbg_display(&base, &w, &h, &stride, &pixfmt, &useblt,
                             &dtw, &dth, &outw, &outh);
        env("gop: mode %dx%d stride %d pixfmt %d fb_base %llx present=%s\n",
            w, h, stride, pixfmt, base, useblt ? "Blt (NO linear fb!)" : "linear");
        env("desktop: %dx%d scaled to %dx%d\n", dtw, dth, outw, outh);

        /* the framebuffer cache attribute - the single biggest unmeasured
         * perf unknown on metal.  PAT + the MTRR covering the fb base. */
        env("ia32_pat: %llx\n", d_rdmsr(0x277));
        {
            unsigned long long cap = d_rdmsr(0xFE);
            int vcnt = (int)(cap & 0xFF), i, hit = -1;
            unsigned long long def = d_rdmsr(0x2FF);
            for (i = 0; i < vcnt; i++) {
                unsigned long long pb = d_rdmsr(0x200u + 2u * (unsigned)i);
                unsigned long long pm = d_rdmsr(0x201u + 2u * (unsigned)i);
                if (!(pm & 0x800)) continue;             /* not valid */
                if ((base & pm & ~0xFFFull) == (pb & pm & ~0xFFFull)) {
                    env("mtrr%d covers fb: base=%llx mask=%llx type=%llu %s\n",
                        i, pb, pm, pb & 0xFF,
                        (pb & 0xFF) == 1 ? "(WC - good)" :
                        (pb & 0xFF) == 0 ? "(UC - SLOW PRESENT!)" : "");
                    hit = i;
                }
            }
            if (hit < 0)
                env("mtrr: none covers fb base - default type %llu %s\n",
                    def & 0xFF, (def & 0xFF) == 0 ? "(UC - SLOW PRESENT!)" : "");
        }
        /* measured write bandwidth, which settles the cache question
         * regardless of what PAT/MTRR claim: sweep the bottom rows of VRAM
         * and a RAM buffer with the same store loop and compare. */
        if (!useblt && base && h > 80 && g_tsc_per_ms) {
            volatile uint32_t *vp =
                (volatile uint32_t *)(uintptr_t)base + (unsigned)(h - 64) * (unsigned)stride;
            static uint32_t rbuf[64 * 1024];
            unsigned long n = (unsigned long)stride * 64u, i, rep;
            unsigned long long t0, tv, tr;
            if (n > 1u << 20) n = 1u << 20;
            t0 = uno_native_rdtsc();
            for (i = 0; i < n; i++) vp[i] = 0xFF102040u + (uint32_t)i;
            tv = uno_native_rdtsc() - t0;
            t0 = uno_native_rdtsc();
            for (rep = 0; rep * (sizeof rbuf / 4) < n; rep++)
                for (i = 0; i < sizeof rbuf / 4; i++) rbuf[i] = 0xFF102040u + (uint32_t)i;
            tr = uno_native_rdtsc() - t0;
            {
                /* KB/s = bytes * (tsc/ms) / cycles */
                unsigned long long vk = tv ? (n * 4ull) * g_tsc_per_ms / tv : 0;
                unsigned long long rk = tr ? (n * 4ull) * g_tsc_per_ms / tr : 0;
                env("fb bench: vram %llu KB/s ram %llu KB/s ratio 1:%llu%s\n",
                    vk, rk, vk ? rk / vk : 0,
                    (vk && vk < 300000ull) ? "  << UNCACHED-CLASS VRAM (perf bug)" : "");
            }
            /* P3 scouting: the same bytes through the firmware's Blt().
             * If the blitter DMAs, it sidesteps the uncached-store problem
             * entirely and gUseBlt (an existing present path) becomes a
             * free win - no MTRR reprogramming required. */
            {
                unsigned long long bc = 0;
                unsigned long bb = 0;
                if (uno_pc64_dbg_blt_bench(&bc, &bb) && bc) {
                    unsigned long long bk = (bb / 1024ull) * g_tsc_per_ms * 1000ull / bc;
                    unsigned long long vk2 = tv ? (n * 4ull / 1024ull) * g_tsc_per_ms * 1000ull / tv : 0;
                    env("blt bench: %llu KB/s  (direct %llu KB/s)  -> %s\n",
                        bk, vk2,
                        bk > vk2 * 2 ? "BLT IS FASTER - try the gUseBlt present path (P3, free)"
                                     : (bk * 2 < vk2 ? "direct wins - Blt is not the answer"
                                                     : "comparable - no easy win here"));
                } else {
                    env("blt bench: unavailable (detached or no GOP)\n");
                }
            }
            uno_pc64_dbg_bench_cleanup();   /* F10: leave no pattern on screen */
        }
    }

    {   /* input inventory */
        int ns = 0, na = 0, blocked = 0;
        int nbars = 0, nctrl = 0, present = 0, addr = 0, parsed = 0;
        int kbd = 0, aux = 0, auxport = 0, auxid = 0;
        int unk = 0, unm = 0;
        uno_pc64_ptr_status(&ns, &na, &blocked);
        uno_i2c_hid_status(&nbars, &nctrl, &present, &addr, &parsed);
        uno_ps2_status(&kbd, &aux, &auxport, &auxid);
        uno_usb_hid_status(&unk, &unm);
        env("pointer: fw_simple=%d fw_abs=%d detach_blocked=%d\n", ns, na, blocked);
        env("i2c-hid: ctrls=%d present=%d addr=%x desc_parsed=%d acpi_hits=%d\n",
            nctrl, present, addr, parsed, uno_i2c_hid_acpi_hits());
        env("ps2: kbd=%d aux=%d auxport=%d auxid=%d\n", kbd, aux, auxport, auxid);
        env("usb-hid: kbd=%d mouse=%d\n", unk, unm);
    }

    {   /* storage inventory */
        int n = uno_fs_volumes(), v;
        env("volumes: %d\n", n);
        for (v = 0; v < n && v < 10; v++)
            env("  vol%d: %s kind=%d writable=%d\n", v,
                uno_fs_volume_name(v), uno_fs_kind(v), uno_fs_writable(v));
    }

    env("detached: %d   tsc/ms: %llu   stash: %s @%llx   crash_reports_seen: %d\n",
        uno_pc64_detached(), g_tsc_per_ms,
        g_stash == &g_stash_bss ? "BSS-FALLBACK (no warm-reset survival)" : "ram",
        (unsigned long long)(uintptr_t)g_stash, g_crash_seen);

    uno_dbg_log("envblock built (%d bytes)", g_env_len);
}

/* ===========================================================================
 * boot-time bring-up + residue flush
 * ======================================================================== */
typedef EFI_STATUS (*AP_FN)(int Type, int MemType, UINTN Pages,
                            EFI_PHYSICAL_ADDRESS *Mem);

static DbgStash *try_stash_at(unsigned long long addr)
{
    EFI_PHYSICAL_ADDRESS mem = addr;
    EFI_BOOT_SERVICES *bs = g_st->BootServices;
    /* AllocateAddress(2) / EfiLoaderData(2) */
    if (((AP_FN)bs->AllocatePages)(2, 2, STASH_PAGES, &mem) != EFI_SUCCESS)
        return 0;
    return (DbgStash *)(uintptr_t)mem;
}

void uno_dbg_early(void *SystemTable)
{
    int i;
    g_st = SystemTable;
    g_tsc0 = uno_native_rdtsc();
    {   /* rough local TSC rate now (init recalibrates the shared one later) */
        unsigned long long t0 = uno_native_rdtsc();
        g_st->BootServices->Stall(10000);
        g_tsc_per_ms = (uno_native_rdtsc() - t0) / 10;
        if (!g_tsc_per_ms) g_tsc_per_ms = 1000000;
    }
    image_extent();

    /* claim a fixed-address stash; RESIDUE (a previous boot's report/log) is
     * left in place untouched until uno_dbg_flush_residue can reach the disk */
    for (i = 0; i < NSTASH && !g_stash; i++)
        g_stash = try_stash_at(kStashAddr[i]);
    if (!g_stash) g_stash = &g_stash_bss;

    if (g_stash->h.magic != STASH_MAGIC || g_stash->h.ver != STASH_VER) {
        memset(g_stash, 0, sizeof *g_stash);        /* cold RAM: fresh stash */
        g_stash->h.magic = STASH_MAGIC;
        g_stash->h.ver   = STASH_VER;
    }
    g_stash->h.boot_count++;

    hook_firmware_idt();
    uno_dbg_log("debug core up: build %s, boot #%u, stash %s",
                g_build_id, g_stash->h.boot_count,
                g_stash == &g_stash_bss ? "bss" : "ram");
}

/* after uno_fat_init: persist anything the previous boot left behind, then
 * reset the ring for this boot */
void uno_dbg_flush_residue(void)
{
    DbgStash *st = g_stash;
    int wrote = 0;
    if (!st) return;

    /* count existing CRASH reports (boot-loop guard).  Only real fault/hang/
     * residue/panic reports count - not PF perf snapshots or the README, or
     * the guard would throttle after a few harmless perf writes. */
    {
        uno_fat_entry e[999];
        int v = crash_vol(), i, n, seen = 0;
        n = (v >= 0) ? uno_fat_list_ex(v, crash_dir(), e, 999) : 0;
        for (i = 0; i < n && i < 999; i++)
            if (!e[i].is_dir &&
                (!strncmp(e[i].name, "CR", 2) || !strncmp(e[i].name, "HG", 2) ||
                 !strncmp(e[i].name, "RS", 2) || !strncmp(e[i].name, "PN", 2)))
                seen++;
        g_crash_seen = seen;
    }

    if (st->h.crash_len && st->h.crash_len < CRASH_MAX &&
        d_crc32(st->crash, st->h.crash_len) == st->h.crash_crc) {
        /* a report captured by the previous boot never reached the disk */
        memcpy(g_report, st->crash, st->h.crash_len);
        g_rlen = (int)st->h.crash_len;
        g_in_disk = 1;
        wrote = disk_write_report((int)st->h.crash_kind);
        g_in_disk = 0;
        uno_dbg_log("residue: previous boot's %c report %s",
                    (int)st->h.crash_kind, wrote ? "flushed to CRASH\\" : "flush FAILED");
    } else if (!st->h.clean && st->h.log_widx + st->h.log_wrapped > 0 &&
               st->h.boot_count > 1) {
        /* no report, but the last boot never said goodbye: triple fault,
         * power event, or firmware watchdog.  Salvage the log tail. */
        g_rlen = 0;
        rp_common_head("RESIDUE");
        rp("\nThe previous boot ended without a clean shutdown AND without a\n"
           "crash report - a triple fault (fault inside the fault path),\n"
           "hard power event, or firmware-level reset.  Its last log lines\n"
           "and checkpoint follow; the checkpoint is your best lead.\n");
        rp("prev last_checkpoint: %s @ %llums\n",
           st->h.last_check[0] ? st->h.last_check : "(none)", st->h.last_check_ms);
        rp("\n---- previous boot's log (tail) ----\n");
        {
            unsigned int i;
            if (st->h.log_wrapped)
                for (i = st->h.log_widx; i < LOG_RING; i++)
                    if (st->log[i] && g_rlen < (int)sizeof g_report - 2)
                        g_report[g_rlen++] = st->log[i];
            for (i = 0; i < st->h.log_widx; i++)
                if (st->log[i] && g_rlen < (int)sizeof g_report - 2)
                    g_report[g_rlen++] = st->log[i];
            g_report[g_rlen] = 0;
        }
        g_in_disk = 1;
        wrote = disk_write_report('R');
        g_in_disk = 0;
        uno_dbg_log("residue: unclean previous boot, log tail %s",
                    wrote ? "flushed" : "flush FAILED");
    }

    /* this boot starts dirty; a deliberate shutdown/restart marks it clean */
    st->h.crash_len = 0;
    st->h.clean = 0;
    st->h.log_widx = 0; st->h.log_wrapped = 0;
    memset(st->log, 0, LOG_RING);
    st->h.last_check[0] = 0;
    uno_dbg_log("boot #%u (crash reports on disk: %d)",
                st->h.boot_count, g_crash_seen);
}

void uno_dbg_mark_clean(void);
void uno_dbg_mark_clean(void)
{
    if (g_stash) g_stash->h.clean = 1;
    uno_dbg_log("clean shutdown/restart");
}

/* write BOOTENV.TXT (latest env block) - called by the stress driver once,
 * from safe main-loop context */
void uno_dbg_write_bootenv(void);
void uno_dbg_write_bootenv(void)
{
    char path[48];
    int vol = crash_vol();
    if (vol < 0 || !g_env_len) return;
    g_in_disk = 1;
    /* root copy = "the last boot", machine-dir copy = "this machine's boot"
     * (one stick now covers a whole batch of machines) */
    uno_fat_write(vol, "BOOTENV.TXT", (const unsigned char *)g_env, g_env_len);
    snprintf(path, sizeof path, "%s\\BOOTENV.TXT", crash_dir());
    uno_fat_write(vol, path, (const unsigned char *)g_env, g_env_len);
    uno_fat_sync();     /* a test run normally ends in a forced power-off, and
                           an unsynced write dies in the FAT write-back cache */
    g_in_disk = 0;
}

/* Append one line to CRASH\BOOTS.TXT the moment storage is usable.
 *
 * "No telemetry on the stick" is ambiguous: it can mean the machine never
 * booted our kernel, or that it booted and died before the first write. The
 * Latitude produced nothing twice and we could not tell which. This marker is
 * written as EARLY as storage allows - long before the env block, the fb bench,
 * ACPI, audio or detach - so its presence proves the kernel ran and reached the
 * filesystem, and its absence means it never got that far. It accumulates, so
 * the file also counts how many times the machine actually booted us. */
void uno_dbg_boot_marker(void);
void uno_dbg_boot_marker(void)
{
    static char buf[2048];
    char line[160], path[48];
    int vol = crash_vol(), n, y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
    long have;
    if (vol < 0) return;
    uno_pc64_time(&y, &mo, &d, &h, &mi, &sec);
    n = snprintf(line, sizeof line,
                 "boot %u  %04d-%02d-%02d %02d:%02d:%02d  build %s\r\n",
                 g_stash ? g_stash->h.boot_count : 0,
                 y, mo, d, h, mi, sec, g_build_id);
    snprintf(path, sizeof path, "%s\\BOOTS.TXT", crash_dir());
    g_in_disk = 1;
    have = uno_fat_read(vol, path, (unsigned char *)buf,
                        (long)sizeof buf - n - 1);
    if (have < 0) have = 0;
    memcpy(buf + have, line, (size_t)n);
    uno_fat_write(vol, path, (const unsigned char *)buf,
                  have + n);
    uno_fat_sync();
    g_in_disk = 0;
    uno_dbg_log("boot marker written (boot #%u)",
                g_stash ? g_stash->h.boot_count : 0);
}

/* Dump the kernel log + env block to CRASH\BOOTLOG.TXT.
 *
 * Until now the kernel log only reached disk if something CRASHED, so a boot
 * that came up "wrong but alive" (no HUD, stress driver never arming) left
 * nothing to read and we were reduced to guessing from the machine's
 * behaviour.  This runs unconditionally at the end of init and periodically
 * after, so every boot leaves a readable trace. */
void uno_dbg_write_bootlog(void);
void uno_dbg_write_bootlog(void)
{
    char path[48];
    int vol = crash_vol();
    if (vol < 0 || g_in_trap) return;
    g_rlen = 0;
    rp_common_head("BOOT LOG");
    rp("\n(written unconditionally - this is NOT a crash)\n");
    rp_env();
    rp_log_tail();
    snprintf(path, sizeof path, "%s\\BOOTLOG.TXT", crash_dir());
    g_in_disk = 1;
    uno_fat_write(vol, path, (const unsigned char *)g_report,
                  (long)g_rlen);
    uno_fat_sync();
    g_in_disk = 0;
}

/* the stress driver's perf-snapshot writer */
void uno_dbg_write_perf(const char *text, int len);
void uno_dbg_write_perf(const char *text, int len)
{
    char path[40];
    int vol = crash_vol();
    if (vol < 0) return;
    g_in_disk = 1;
    snprintf(path, sizeof path, "%s\\PF%03d.TXT", crash_dir(), next_seq(vol));
    uno_fat_write(vol, path, (const unsigned char *)text, len);
    uno_fat_sync();     /* same: survive the operator pulling the plug */
    g_in_disk = 0;
}

/* ===========================================================================
 * perf HUD counters
 * ======================================================================== */
static unsigned long g_render_us, g_present_us;     /* EMA (x16)            */
static unsigned long g_frames, g_idle_frames;
static unsigned long long g_hud_t0;
static unsigned long g_fps;                         /* refreshed once a second */
/* WORST-CASE frame tracking.  render_avg/present_avg are moving averages and
 * fps is a one-second count, so all three smooth away exactly the thing an
 * operator actually notices: an occasional hitch in an otherwise smooth run.
 * Averages cannot answer "what stalls it once in a while", so track the peak
 * and how often we blow past a visible-stutter threshold - and capture the
 * checkpoint that was live at the time, which names the culprit. */
static unsigned long g_render_max, g_present_max, g_hitches;
static char g_worst_tag[64];
#define HITCH_US 100000ul                           /* 100 ms = clearly visible */

/* Remember what the OS was doing during the worst frame - the checkpoint tag is
 * already maintained for the crash/hang reports, so a hitch gets a name. */
static void note_worst(unsigned long us)
{
    int i;
    const char *tag = (g_stash && g_stash->h.last_check[0])
                      ? g_stash->h.last_check : "(none)";
    for (i = 0; i < 63 && tag[i]; i++) g_worst_tag[i] = tag[i];
    g_worst_tag[i] = 0;
    (void)us;
}
/* ---- per-window draw profiler (fed via unoui_profile_win) ----------------
 * F11 taught us the averages-and-halves view cannot NAME a spike: four
 * machines showed 1.2-3.3 s single renders and nothing said which window.
 * Track draw cycles per window title (pointer identity is fine - titles are
 * string literals), the current frame's most expensive window, and surface
 * both in the PF snapshot and in the worst-frame tag. */
#define WPROF_N 24
static struct { const char *title; unsigned long long cyc; unsigned long calls;
                unsigned long max_us; } g_wprof[WPROF_N];
static int g_wprof_n;
static const char *g_wp_cur;            /* window being drawn RIGHT NOW - the
                                           hang report reads it, so a wedged
                                           draw callback names its window     */
static unsigned long long g_wp_t0;
static const char *g_frame_top_title;   /* this frame's costliest window      */
static unsigned long g_frame_top_us;
static char g_worst_win[32];            /* worst frame's costliest window     */

const char *uno_dbg_current_window(void) { return g_wp_cur; }

/* unoautomate PROBE: walk the profiler table (unoauto_probe.c). Returns 1
 * and fills the outs while i is in range, 0 past the end. */
int uno_dbg_win_stat(int i, const char **title, unsigned long long *cyc,
                     unsigned long *max_us)
{
    if (i < 0 || i >= g_wprof_n) return 0;
    if (title)  *title  = g_wprof[i].title;
    if (cyc)    *cyc    = g_wprof[i].cyc;
    if (max_us) *max_us = g_wprof[i].max_us;
    return 1;
}

unsigned long uno_dbg_cyc_to_us(unsigned long long cyc)
{ return g_tsc_per_ms ? (unsigned long)(cyc * 1000 / g_tsc_per_ms) : 0; }

unsigned long long uno_dbg_tsc_per_ms(void) { return g_tsc_per_ms; }

void uno_dbg_win_profile(const char *title, int begin);
void uno_dbg_win_profile(const char *title, int begin)
{
    if (begin) { g_wp_cur = title; g_wp_t0 = uno_native_rdtsc(); return; }
    {
        unsigned long long dt = uno_native_rdtsc() - g_wp_t0;
        unsigned long us = g_tsc_per_ms ? (unsigned long)(dt * 1000 / g_tsc_per_ms) : 0;
        int i;
        g_wp_cur = 0;
        for (i = 0; i < g_wprof_n; i++) if (g_wprof[i].title == title) break;
        if (i == g_wprof_n && g_wprof_n < WPROF_N) g_wprof[g_wprof_n++].title = title;
        if (i < g_wprof_n) {
            g_wprof[i].cyc += dt; g_wprof[i].calls++;
            if (us > g_wprof[i].max_us) g_wprof[i].max_us = us;
        }
        if (us > g_frame_top_us) { g_frame_top_us = us; g_frame_top_title = title; }
    }
}
void uno_dbg_win_frame_reset(void);
void uno_dbg_win_frame_reset(void) { g_frame_top_us = 0; g_frame_top_title = 0; }

/* A hitch is a hitch whichever half caused it - and stacked halves count too:
 * the Surface showed 97 ms render + 82 ms present frames (~3x frame time,
 * clearly felt) that per-half thresholds scored as ZERO hitches. Render feeds
 * remember their us; the present feed judges the TOTAL frame. */
static unsigned long g_last_render_us, g_total_max;
static void bump(unsigned long us, unsigned long *maxp)
{
    if (us > *maxp) *maxp = us;
    if (us > HITCH_US) { g_hitches++; note_worst(us); }
}
void uno_dbg_frame_render_cyc(unsigned long long cyc)
{
    unsigned long us = g_tsc_per_ms ? (unsigned long)(cyc * 1000 / g_tsc_per_ms) : 0;
    g_render_us += us - g_render_us / 16;
    g_last_render_us = us;
    if (us > g_render_max && g_frame_top_title) {   /* name the culprit */
        int i; const char *s = g_frame_top_title;
        for (i = 0; i < (int)sizeof g_worst_win - 1 && s[i]; i++) g_worst_win[i] = s[i];
        g_worst_win[i] = 0;
    }
    bump(us, &g_render_max);
}
void uno_dbg_frame_present_cyc(unsigned long long cyc)
{
    unsigned long us = g_tsc_per_ms ? (unsigned long)(cyc * 1000 / g_tsc_per_ms) : 0;
    unsigned long total = us + g_last_render_us;
    g_present_us += us - g_present_us / 16;
    if (total > g_total_max) g_total_max = total;
    if (total > HITCH_US && us <= HITCH_US && g_last_render_us <= HITCH_US)
        { g_hitches++; note_worst(total); }         /* stacked-halves hitch */
    g_last_render_us = 0;
    bump(us, &g_present_max);
}
void uno_dbg_frame_idle(int was_idle)
{
    static unsigned long last_frames;
    unsigned long long now;
    g_frames++;
    if (was_idle) g_idle_frames++;
    /* fps here, not in the HUD: the HUD only runs when it is drawn/enabled, and
     * the perf snapshot must carry real numbers either way. */
    now = uno_dbg_uptime_ms();
    if (now - g_hud_t0 >= 1000) {
        g_fps = g_frames - last_frames;
        last_frames = g_frames;
        g_hud_t0 = now;
    }
}
void uno_dbg_hud_toggle(void) { g_hud_on = !g_hud_on; }

/* One line of hard perf numbers for CRASH\PF###.TXT.  The HUD is on-screen
 * only, so without this the render-vs-present split - the thing that says
 * whether a machine is CPU-bound or framebuffer-bound - never reached disk and
 * we had to ask the operator to read a HUD. */
int uno_dbg_perf_line(char *buf, int max)
{
    unsigned long hu = 0, hf = 0, hl = 0;
    uno_heap_stats(&hu, &hf, &hl);
    {
        /* snprintf returns the INTENDED length, which can exceed the buffer;
         * clamp every accumulation so `n` never runs past `max` (else the
         * caller's `sizeof buf - n` underflows to a huge size_t -> stack
         * smash). appended() caps each result to the space that remained. */
        int n, r;
        #define APPEND(...) do { \
            if (n < max) { r = snprintf(buf + n, (size_t)(max - n), __VA_ARGS__); \
                           n += (r < 0) ? 0 : (r > max - n ? max - n : r); } } while (0)
        n = 0;
        APPEND("perf: render_avg=%lu us  present_avg=%lu us  fps=%lu  idle=%lu%%  "
            "frames=%lu  heap_used=%lu\n"
            "worst: render_max=%lu us  present_max=%lu us  total_max=%lu us  "
            "hitches>%lums=%lu  during=%s  window=%s",
            g_render_us / 16, g_present_us / 16, g_fps,
            g_frames ? g_idle_frames * 100 / g_frames : 0,
            g_frames, hu,
            g_render_max, g_present_max, g_total_max, HITCH_US / 1000, g_hitches,
            g_worst_tag[0] ? g_worst_tag : "(none)",
            g_worst_win[0] ? g_worst_win : "(none)");
        /* top window draw costs this pass - the F11 attribution line */
        {
            int k, pass;
            for (pass = 0; pass < 4 && n < max - 8; pass++) {
                int best = -1; unsigned long long bc = 0;
                for (k = 0; k < g_wprof_n; k++)
                    if (g_wprof[k].calls && g_wprof[k].cyc >= bc)
                        { bc = g_wprof[k].cyc; best = k; }
                if (best < 0) break;
                APPEND("%s%s=%lums/%luc/max%luus", pass ? "  " : "\nwindows: ",
                    g_wprof[best].title,
                    (unsigned long)(g_tsc_per_ms ? g_wprof[best].cyc / g_tsc_per_ms : 0),
                    g_wprof[best].calls, g_wprof[best].max_us);
                g_wprof[best].calls = 0;            /* consumed for ranking */
            }
        }
        #undef APPEND
        /* per-pass, not cumulative: reset so each snapshot describes its own
         * pass and a late hitch cannot hide behind an early one */
        g_render_max = g_present_max = g_hitches = g_total_max = 0;
        g_worst_tag[0] = 0; g_worst_win[0] = 0;
        g_wprof_n = 0;
        memset(g_wprof, 0, sizeof g_wprof);
        return n;
    }
}

int uno_dbg_hud(char *buf, int max)
{
    unsigned long fps = g_fps;
    unsigned long hu = 0, hf = 0, hl = 0;
    if (!g_hud_on) return 0;
    uno_heap_stats(&hu, &hf, &hl);
    return snprintf(buf, (size_t)max,
                    "DBG r%lu.%lums p%lu.%lums f%lu idle%lu%% heap%luM cr%d",
                    (g_render_us / 16) / 1000, ((g_render_us / 16) / 100) % 10,
                    (g_present_us / 16) / 1000, ((g_present_us / 16) / 100) % 10,
                    fps,
                    g_frames ? g_idle_frames * 100 / g_frames : 0,
                    hu >> 20, g_crash_seen);
}

/* ===========================================================================
 * fault injection
 * ======================================================================== */
int uno_dbg_oom_tick(void)
{
    if (uno_dbg_oom_every <= 0) return 0;
    if (++g_oom_ctr % (unsigned)uno_dbg_oom_every) return 0;
    g_oom_injected++;
    return 1;
}

void uno_dbg_force(int what)
{
    uno_dbg_log("force: injecting fault %d", what);
    switch (what) {
    /* #PF: a high canonical address that no firmware/native page table maps -
     * a WRITE so it faults even if some mapping made it readable (0x10, the
     * low IVT/BDA, is mapped in QEMU and did NOT fault - lesson learned). */
    case 0: { volatile int *p = (volatile int *)0x00007F0000000000ull; *p = 0x1234; break; }
    case 1: __builtin_trap();                   /* #UD: always faults          */
    case 2: { volatile int z = 0; volatile int r = 1 / z; (void)r; break; }
    case 3: {                                   /* stack smash */
        volatile char buf[16];
        volatile int i;
        for (i = 0; i < 64; i++) buf[i] = (char)0xAA;
        (void)buf;
        break;
    }
    case 4: for (;;) { }                        /* hang: heartbeat stops */
    case 5: uno_dbg_panic("forced panic (uno_dbg_force 5)");
    }
}

#endif /* UNO_DEBUG */
