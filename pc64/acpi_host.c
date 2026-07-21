/* acpi_host.c - the UnoDOS/pc64 (UEFI) host layer for the unoacpi stack.
 *
 * Implements the ~25 uacpi_kernel_* callbacks the vendored uACPI interpreter
 * calls out to, then drives the portable bring-up (unoacpi/acpi_power.c).
 * pc64 is a single-threaded, no-preemption, boot-services environment, so
 * mutexes/events/spinlocks/interrupts collapse to near-no-ops.  The substance:
 * RSDP discovery from the EFI config table, an identity memory map (UEFI boot
 * services run identity-mapped), the shared arena heap for alloc/free, a
 * TSC-based nanosecond clock (the EC/SMBus handlers' timeouts depend on it
 * advancing), port I/O for the SystemIO op-region, and legacy PCI config
 * mechanism #1 for the PCI_Config op-region.
 *
 * Adapted from the Writer's Unlock reference host (uefi/acpi_host.c in
 * hmofet/writers-unlock); the porting contract is that repo's
 * docs/ACPI-POWER-INTERFACE.md.  Only built with -DUNO_ACPI (see build.sh).
 */
#include "uefi.h"
#include "string.h"
#include "acpi_host.h"
#include "pc64_pci.h"

#include "acpi_arena.h"
#include "acpi_power.h"
#include "smbus_handler.h"

#include <uacpi/kernel_api.h>
#include <uacpi/status.h>
#include <uacpi/types.h>

/* 8 MiB arena: a real laptop namespace is ~1-1.5 MiB persistent (the namespace
 * LIVES here for the interpreter's life - never reset it) plus transient eval
 * headroom; 1 MiB silently truncates SSDTs on real machines. */
#define ARENA_BYTES (8u << 20)
#define EfiLoaderData 2

/* ---- local port I/O + TSC -------------------------------------------------- */
static inline uint8_t  pin8 (uint16_t p){ uint8_t  v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint16_t pin16(uint16_t p){ uint16_t v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline uint32_t pin32(uint16_t p){ uint32_t v; __asm__ volatile("inl %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void     pout8 (uint16_t p, uint8_t  v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void     pout16(uint16_t p, uint16_t v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }
static inline void     pout32(uint16_t p, uint32_t v){ __asm__ volatile("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint64_t rdtsc(void){ uint32_t lo,hi; __asm__ volatile("rdtsc":"=a"(lo),"=d"(hi)); return ((uint64_t)hi<<32)|lo; }

static EFI_BOOT_SERVICES *gBS_;
static uint64_t g_rsdp;
static uint64_t g_tsc_start;
static uint64_t g_tsc_mhz = 1;          /* TSC cycles per microsecond          */
static int      g_dummy;                /* address source for opaque handles   */
static int      g_status = -1;          /* uno_acpi_start result (-1 = not run)*/

/* ---- RSDP discovery (EFI configuration table) ------------------------------
 * (EFI_CONFIGURATION_TABLE now comes from uefi.h - the SMBIOS machine-identity
 * reader needed it too, so the local copy that used to live here moved there) */

static uint64_t find_rsdp(EFI_SYSTEM_TABLE *ST)
{
    static EFI_GUID g20 = { 0x8868e871, 0xe4f1, 0x11d3,
                            { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
    static EFI_GUID g10 = { 0xeb9d2d30, 0x2d88, 0x11d3,
                            { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };
    EFI_CONFIGURATION_TABLE *ct = (EFI_CONFIGURATION_TABLE *)ST->ConfigurationTable;
    uint64_t rsdp = 0;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        if (!memcmp(&ct[i].VendorGuid, &g20, sizeof(EFI_GUID)))
            return (uint64_t)(uintptr_t)ct[i].VendorTable;   /* 2.0+ preferred */
        if (!memcmp(&ct[i].VendorGuid, &g10, sizeof(EFI_GUID)))
            rsdp = (uint64_t)(uintptr_t)ct[i].VendorTable;   /* 1.0 fallback   */
    }
    return rsdp;
}

/* ---- entry: bind EFI, seed handlers, run the portable bring-up ------------- */
int uno_acpi_start(void *stv)
{
    if (g_status >= 0)
        return g_status;                          /* idempotent               */
    EFI_SYSTEM_TABLE *ST = (EFI_SYSTEM_TABLE *)stv;
    gBS_ = ST->BootServices;
    g_status = 0;

    g_rsdp = find_rsdp(ST);
    if (!g_rsdp)
        return 0;

    /* Calibrate the TSC clock against a 10 ms firmware stall. */
    g_tsc_start = rdtsc();
    uint64_t a = rdtsc();
    gBS_->Stall(10000);
    uint64_t b = rdtsc();
    uint64_t mhz = ((b - a) * 100u) / 1000000u;   /* cycles/s -> cycles/us    */
    g_tsc_mhz = mhz ? mhz : 1;

    void *buf = 0;
    if (gBS_->AllocatePool(EfiLoaderData, ARENA_BYTES, &buf) != EFI_SUCCESS || !buf)
        return 0;
    if (!acpi_arena_init(buf, ARENA_BYTES))
        return 0;

    /* Seed the SMBus region handler with the Intel PCH SMBus I/O base (PCI
     * class 0x0C05, BAR4).  Absent controller -> base 0 -> handler disabled. */
    {
        pci_dev smb;
        if (pci_find_class(0x0C, 0x05, &smb)) {
            unsigned long long bar = pci_bar(&smb, 4);
            if (bar & 1)                          /* I/O-space BAR            */
                wu_smbus_init((uint16_t)(bar & ~1ull));
        }
    }

    g_status = (acpi_power_bringup() == ACPI_POWER_OK);
    return g_status;
}

uint64_t uno_acpi_rsdp(void) { return g_rsdp; }

/* ---- log ring (tail kept for the System window / debugging) ---------------- */
#define LOGSZ 1024
static char     g_log[LOGSZ];
static uint32_t g_logn;

void uacpi_kernel_log(uacpi_log_level lvl, const uacpi_char *s)
{
    (void)lvl;
#ifdef UNO_DBGCON                       /* mirror to QEMU debugcon (metal-unsafe) */
    for (const char *p = s; *p; p++) pout8(0x402, (uint8_t)*p);
#endif
    uint32_t sl = (uint32_t)strlen(s);
    if (sl >= LOGSZ - 1) { s += sl - (LOGSZ - 1); sl = LOGSZ - 1; }
    if (g_logn + sl >= LOGSZ) {                       /* drop from the front  */
        uint32_t drop = g_logn + sl - (LOGSZ - 1);
        if (drop > g_logn) drop = g_logn;
        memmove(g_log, g_log + drop, g_logn - drop);
        g_logn -= drop;
    }
    memcpy(g_log + g_logn, s, sl);
    g_logn += sl;
    g_log[g_logn] = 0;
}

const char *uno_acpi_log_tail(void) { g_log[g_logn] = 0; return g_log; }

/* ---- RSDP / memory map ----------------------------------------------------- */
uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address)
{
    if (!g_rsdp)
        return UACPI_STATUS_NOT_FOUND;
    *out_rsdp_address = (uacpi_phys_addr)g_rsdp;
    return UACPI_STATUS_OK;
}

/* UEFI boot services are identity-mapped, so mapping is a cast. */
void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) { (void)len; return (void *)(uintptr_t)addr; }
void  uacpi_kernel_unmap(void *addr, uacpi_size len)         { (void)addr; (void)len; }

/* ---- allocation (shared arena; built without UACPI_SIZED_FREES) ------------ */
void *uacpi_kernel_alloc(uacpi_size size) { return acpi_arena_alloc((size_t)size); }
void  uacpi_kernel_free(void *mem)        { acpi_arena_free(mem); }

/* ---- time ------------------------------------------------------------------ */
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void)
{
    uint64_t d = rdtsc() - g_tsc_start;               /* elapsed cycles       */
    return (uacpi_u64)((d * 1000u) / g_tsc_mhz);      /* d/mhz = us; *1000=ns */
}

int  uno_pc64_detached(void);                     /* uefi_main.c (M3) */
void uno_native_delay_us(unsigned long us);       /* pc64_native.c    */

void uacpi_kernel_stall(uacpi_u8 usec)
{ if (uno_pc64_detached()) uno_native_delay_us(usec); else if (gBS_) gBS_->Stall(usec); }
void uacpi_kernel_sleep(uacpi_u64 msec)
{ if (uno_pc64_detached()) uno_native_delay_us((unsigned long)msec * 1000u);
  else if (gBS_) gBS_->Stall((UINTN)(msec * 1000u)); }

/* ---- locks / events / threads (single-threaded boot-services stubs) -------- */
uacpi_handle uacpi_kernel_create_mutex(void)                        { return (uacpi_handle)&g_dummy; }
void         uacpi_kernel_free_mutex(uacpi_handle h)                { (void)h; }
uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle h, uacpi_u16 t){ (void)h; (void)t; return UACPI_STATUS_OK; }
void         uacpi_kernel_release_mutex(uacpi_handle h)             { (void)h; }

uacpi_handle uacpi_kernel_create_event(void)                        { return (uacpi_handle)&g_dummy; }
void         uacpi_kernel_free_event(uacpi_handle h)                { (void)h; }
uacpi_bool   uacpi_kernel_wait_for_event(uacpi_handle h, uacpi_u16 t){ (void)h; (void)t; return UACPI_FALSE; }
void         uacpi_kernel_signal_event(uacpi_handle h)              { (void)h; }
void         uacpi_kernel_reset_event(uacpi_handle h)               { (void)h; }

/* Must never be UACPI_THREAD_ID_NONE ((void*)-1). */
uacpi_thread_id uacpi_kernel_get_thread_id(void) { return (uacpi_thread_id)1; }

uacpi_handle    uacpi_kernel_create_spinlock(void)                          { return (uacpi_handle)&g_dummy; }
void            uacpi_kernel_free_spinlock(uacpi_handle h)                  { (void)h; }
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h)                  { (void)h; return 0; }
void            uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags f) { (void)h; (void)f; }

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void)                 { return 0; }
void                  uacpi_kernel_restore_interrupts(uacpi_interrupt_state s) { (void)s; }

/* ---- firmware requests / interrupts / deferred work ------------------------ */
uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *r) { (void)r; return UACPI_STATUS_OK; }

uacpi_status uacpi_kernel_install_interrupt_handler(
    uacpi_u32 irq, uacpi_interrupt_handler h, uacpi_handle ctx, uacpi_handle *out_irq_handle)
{
    (void)irq; (void)h; (void)ctx;                    /* no GPE wiring        */
    if (out_irq_handle) *out_irq_handle = (uacpi_handle)&g_dummy;
    return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler h, uacpi_handle irq_handle)
{
    (void)h; (void)irq_handle; return UACPI_STATUS_OK;
}

/* Single-threaded: run deferred work synchronously rather than queueing it. */
uacpi_status uacpi_kernel_schedule_work(uacpi_work_type t, uacpi_work_handler h, uacpi_handle ctx)
{
    (void)t; if (h) h(ctx); return UACPI_STATUS_OK;
}
uacpi_status uacpi_kernel_wait_for_work_completion(void) { return UACPI_STATUS_OK; }

/* ---- SystemIO op-region (x86 port I/O) ------------------------------------- */
uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle)
{
    (void)len; *out_handle = (uacpi_handle)(uintptr_t)base; return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle handle) { (void)handle; }

static inline uint16_t io_port(uacpi_handle h, uacpi_size off) { return (uint16_t)((uintptr_t)h + off); }

uacpi_status uacpi_kernel_io_read8 (uacpi_handle h, uacpi_size o, uacpi_u8  *v){ *v = pin8 (io_port(h,o)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size o, uacpi_u16 *v){ *v = pin16(io_port(h,o)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size o, uacpi_u32 *v){ *v = pin32(io_port(h,o)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write8 (uacpi_handle h, uacpi_size o, uacpi_u8  v){ pout8 (io_port(h,o), v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size o, uacpi_u16 v){ pout16(io_port(h,o), v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size o, uacpi_u32 v){ pout32(io_port(h,o), v); return UACPI_STATUS_OK; }

/* ---- PCI_Config op-region (legacy configuration mechanism #1, segment 0) ---- */
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out_handle)
{
    /* Encode bus/device/function into the CF8 address (enable bit set, offset
     * added per access).  Segment > 0 (ECAM-only) is not reachable this way. */
    uint32_t enc = 0x80000000u
                 | ((uint32_t)address.bus      << 16)
                 | ((uint32_t)address.device   << 11)
                 | ((uint32_t)address.function << 8);
    *out_handle = (uacpi_handle)(uintptr_t)enc;
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle h) { (void)h; }

static uint32_t cfg_addr(uacpi_handle h, uacpi_size off)
{
    return ((uint32_t)(uintptr_t)h) | ((uint32_t)off & 0xFCu);
}
static uint32_t cfg_read32(uacpi_handle h, uacpi_size off)
{
    pout32(0xCF8, cfg_addr(h, off));
    return pin32(0xCFC);
}
static void cfg_write_masked(uacpi_handle h, uacpi_size off, uint32_t val, uint32_t mask, int shift)
{
    uint32_t addr = cfg_addr(h, off);
    pout32(0xCF8, addr);
    uint32_t d = pin32(0xCFC);
    d = (d & ~(mask << shift)) | ((val & mask) << shift);
    pout32(0xCF8, addr);
    pout32(0xCFC, d);
}

uacpi_status uacpi_kernel_pci_read8 (uacpi_handle h, uacpi_size o, uacpi_u8  *v){ *v = (uacpi_u8) (cfg_read32(h,o) >> ((o & 3) * 8)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_read16(uacpi_handle h, uacpi_size o, uacpi_u16 *v){ *v = (uacpi_u16)(cfg_read32(h,o) >> ((o & 2) * 8)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_read32(uacpi_handle h, uacpi_size o, uacpi_u32 *v){ *v = cfg_read32(h,o); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_write8 (uacpi_handle h, uacpi_size o, uacpi_u8  v){ cfg_write_masked(h,o,v,0xFFu,   (o & 3) * 8); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_write16(uacpi_handle h, uacpi_size o, uacpi_u16 v){ cfg_write_masked(h,o,v,0xFFFFu, (o & 2) * 8); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_pci_write32(uacpi_handle h, uacpi_size o, uacpi_u32 v){ pout32(0xCF8, cfg_addr(h,o)); pout32(0xCFC, v); return UACPI_STATUS_OK; }
