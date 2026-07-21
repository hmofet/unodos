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

/* ===========================================================================
 * F4: I2C-HID discovery from the ACPI namespace.
 *
 * The PCI-scan probe finds LPSS controllers on every machine and binds a
 * device on none, because nothing tells it the SLAVE ADDRESS (addr=0 in every
 * BOOTENV). ACPI names everything: a HID-over-I2C device is a PNP0C50 node
 * whose _CRS carries an I2cSerialBus (slave address + a path to the
 * controller device), and whose _DSM (3CDFF6F7-4267-4555-AD05-B30A3D8938DE,
 * rev 1, fn 1) returns the HID-descriptor register. The controller node's
 * _ADR gives the PCI dev/fn to match against our BAR list.
 * ======================================================================== */
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>
#include <uacpi/namespace.h>
#include <uacpi/types.h>
#include <uacpi/sleep.h>

struct i2chid_ctx { uno_acpi_i2chid *out; int max, n; };

static uacpi_iteration_decision i2chid_res_cb(void *user, uacpi_resource *res)
{
    struct i2chid_ctx *cx = (struct i2chid_ctx *)user;
    if (res->type != UACPI_RESOURCE_TYPE_SERIAL_I2C_CONNECTION)
        return UACPI_ITERATION_DECISION_CONTINUE;
    if (cx->n < cx->max) {
        uacpi_resource_i2c_connection *ic = &res->i2c_connection;
        uno_acpi_i2chid *h = &cx->out[cx->n];
        h->slave = ic->slave_address;
        h->ctrl_dev = h->ctrl_fn = -1;
        h->src[0] = 0;
        if (ic->common.source.string) {
            int i;
            for (i = 0; ic->common.source.string[i] && i < (int)sizeof h->src - 1; i++)
                h->src[i] = ic->common.source.string[i];
            h->src[i] = 0;
        }
    }
    return UACPI_ITERATION_DECISION_BREAK;    /* first I2C resource is the one */
}

static uacpi_iteration_decision i2chid_dev_cb(void *user,
                                              uacpi_namespace_node *node,
                                              uacpi_u32 depth)
{
    struct i2chid_ctx *cx = (struct i2chid_ctx *)user;
    int had = cx->n;
    (void)depth;
    if (cx->n >= cx->max) return UACPI_ITERATION_DECISION_BREAK;

    if (uacpi_for_each_device_resource(node, "_CRS", i2chid_res_cb, cx)
            != UACPI_STATUS_OK || cx->out[cx->n].src[0] == 0)
        return UACPI_ITERATION_DECISION_CONTINUE;   /* no I2C resource */

    /* HID descriptor register via _DSM(hid-over-i2c GUID, rev 1, fn 1) */
    cx->out[cx->n].desc_reg = 0x0001;               /* spec default fallback */
    {
        static const uacpi_u8 kGuid[16] = {         /* GUID little-endian mix */
            0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45,
            0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE };
        uacpi_data_view dv;
        uacpi_object *args[4];
        uacpi_object_array arr;
        uacpi_object *ret = UACPI_NULL;
        dv.bytes = (uacpi_u8 *)kGuid; dv.length = 16;
        args[0] = uacpi_object_create_buffer(dv);
        args[1] = uacpi_object_create_integer(1);
        args[2] = uacpi_object_create_integer(1);
        { uacpi_object_array empty; empty.objects = UACPI_NULL; empty.count = 0;
          args[3] = uacpi_object_create_package(empty); }
        arr.objects = args; arr.count = 4;
        if (args[0] && args[1] && args[2] && args[3] &&
            uacpi_eval(node, "_DSM", &arr, &ret) == UACPI_STATUS_OK && ret) {
            uacpi_u64 v = 0;
            uacpi_data_view bv;
            if (uacpi_object_get_integer(ret, &v) == UACPI_STATUS_OK && v)
                cx->out[cx->n].desc_reg = (unsigned short)v;
            else if (uacpi_object_get_buffer(ret, &bv) == UACPI_STATUS_OK &&
                     bv.length >= 2)
                cx->out[cx->n].desc_reg =
                    (unsigned short)(bv.bytes[0] | (bv.bytes[1] << 8));
            uacpi_object_unref(ret);
        }
        { int k; for (k = 0; k < 4; k++) if (args[k]) uacpi_object_unref(args[k]); }
    }

    /* resolve the controller path -> PCI dev/fn via its _ADR */
    {
        uacpi_namespace_node *ctrl = UACPI_NULL;
        if (uacpi_namespace_node_find(UACPI_NULL, cx->out[cx->n].src, &ctrl)
                == UACPI_STATUS_OK && ctrl) {
            uacpi_u64 adr = 0;
            if (uacpi_eval_simple_integer(ctrl, "_ADR", &adr) == UACPI_STATUS_OK) {
                cx->out[cx->n].ctrl_dev = (int)((adr >> 16) & 0xFFFF);
                cx->out[cx->n].ctrl_fn  = (int)(adr & 0xFFFF);
            }
        }
    }

    cx->n++;
    (void)had;
    return (cx->n >= cx->max) ? UACPI_ITERATION_DECISION_BREAK
                              : UACPI_ITERATION_DECISION_CONTINUE;
}

int uno_acpi_i2c_hid_enum(uno_acpi_i2chid *out, int max)
{
    struct i2chid_ctx cx;
    if (g_status <= 0) return 0;                    /* interpreter not up */
    cx.out = out; cx.max = max; cx.n = 0;
    uacpi_find_devices("PNP0C50", i2chid_dev_cb, &cx);
    if (cx.n < max) uacpi_find_devices("ACPI0C50", i2chid_dev_cb, &cx);
    return cx.n;
}

/* ACPI S5 (soft-off) - the reliable poweroff on hardware where the firmware's
 * EFI_RESET_SHUTDOWN is a no-op (the Surface stalls on "Shutting down"
 * forever with it). Writes SLP_TYPa|SLP_EN to PM1_CNT via uACPI; this is a
 * hardware register write that powers the machine off regardless of ACPI
 * enable mode, so it works from our NO_ACPI_MODE attached context. Returns
 * only on failure (if it succeeds, the machine is off). */
int uno_acpi_poweroff(void)
{
    if (g_status <= 0) return -1;                 /* interpreter not up */
    if (uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5) != UACPI_STATUS_OK)
        return -1;
    /* interrupts off: enter_sleep_state expects to run uninterrupted */
    __asm__ volatile ("cli");
    uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    return -1;                                     /* still here = it failed */
}
