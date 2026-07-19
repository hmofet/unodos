/* acpi_power.h - portable ACPI power interface (battery, lid, device power).
 *
 * This is the OS-neutral consumer API for the shared AML/ACPI stack.  It is the
 * SAME file in Writer's Unlock and UnoDOS - neither the interface nor its
 * implementation (acpi_power.c) contains anything OS-specific.  Everything
 * platform-dependent lives behind two seams the host OS provides:
 *
 *   1. uACPI's host callbacks - the uacpi_kernel_* functions (see
 *      vendored/uacpi/include/uacpi/kernel_api.h).  The host implements ~25 of
 *      them; most are trivial on a single-core, no-preemption kernel.
 *   2. The arena heap - the host allocates a buffer (>= a few MiB; a real laptop
 *      namespace is ~1-1.5 MiB) and hands it to acpi_arena_init() once, before
 *      calling acpi_power_bringup().
 *
 * The EC OperationRegion handler (shared/ec_handler.c) and the arena
 * (shared/acpi_arena.c) are shared verbatim and need no per-OS work.
 *
 * Full integration spec: docs/ACPI-POWER-INTERFACE.md.
 *
 * Typical host sequence:
 *     acpi_arena_init(buf, len);        // host-owned buffer
 *     acpi_power_bringup();             // init + namespace + EC + device scan
 *     int pct = acpi_battery_percent(); // poll as needed (0..100 or -1)
 *     int lid = acpi_lid_state();       // 1 open / 0 closed / -1 unknown
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Result of acpi_power_bringup(). */
typedef enum {
    ACPI_POWER_OK = 0,       /* interpreter up, namespace built              */
    ACPI_POWER_ENOARENA,     /* acpi_arena_init() was not called / too small */
    ACPI_POWER_EINIT,        /* uacpi_initialize() failed                    */
    ACPI_POWER_ELOAD         /* uacpi_namespace_load() failed                */
} acpi_power_status;

/* Portable diagnostics snapshot (for a host that wants to surface details). */
typedef struct {
    int         ok;              /* bringup reached ACPI_POWER_OK               */
    uint32_t    init_status;     /* raw uacpi_status from uacpi_initialize      */
    uint32_t    load_status;     /* raw uacpi_status from uacpi_namespace_load  */
    uint32_t    nsinit_status;   /* raw uacpi_status from namespace_initialize  */
    uint32_t    ns_nodes;        /* namespace nodes after load                  */
    int         dsdt_found;
    /* EC */
    int         ec_present;      /* EC status port answered (not open bus)      */
    int         ec_from_ecdt;    /* ports came from the ECDT (vs 0x66/0x62)     */
    uint16_t    ec_cmd, ec_data;
    int         ec_reads, ec_timeouts;
    /* battery / lid */
    int         batteries;       /* present PNP0C0A count                       */
    int         bat_present;     /* a reading was obtained                      */
    int         bat_percent;     /* 0..100 or -1                                */
    int         lids;            /* present PNP0C0D count                       */
    int         lid_state;       /* 1 open / 0 closed / -1 unknown              */
    /* device power (_PS0) */
    int         ps0_ran;
    int         ps0_ok;
    /* arena */
    uint64_t    arena_used, arena_peak, arena_total;
    const char *init_str, *load_str, *nsinit_str;   /* uacpi_status_to_string   */
} acpi_power_diag;

/* Bring up the interpreter and locate the battery/lid devices:
 *   uacpi_initialize(NO_ACPI_MODE) -> namespace_load -> install the EC handler
 *   -> namespace_initialize (_REG/_STA/_INI) -> find first present PNP0C0A/0C0D.
 * The host must already have implemented uacpi_kernel_* and called
 * acpi_arena_init().  Idempotent: only the first call does the work. */
acpi_power_status acpi_power_bringup(void);

/* Live battery charge via _STA/_BIX/_BST over the EC.  0..100, or -1 if there is
 * no battery / not present / EC unresponsive.  Safe to call repeatedly. */
int acpi_battery_percent(void);

/* Lid state via _LID.  1 open, 0 closed, -1 unknown. */
int acpi_lid_state(void);

/* ---- Lid transition events (polled edge detection) -------------------------
 * The product-side primitive for "sleep on lid close": call acpi_lid_event()
 * as often as convenient (every frame is fine - each call reads through the
 * 1 s lid cache, so _LID is evaluated at most ~1 Hz) and act on the edges.
 * Read-only: this never touches GPE/SCI hardware, it only re-evaluates _LID.
 *
 * Robustness contract:
 *   - the first KNOWN reading only establishes the baseline (never an edge),
 *     so booting with the lid already closed does not fire a close event;
 *   - -1 (unknown) readings are ignored entirely - a transient EC timeout
 *     between two identical states can neither fabricate nor swallow an edge;
 *   - one detector instance (module state): both of a host's consumers should
 *     share the returned event, not call this from two places. */
typedef enum {
    ACPI_LID_EVT_NONE = 0,
    ACPI_LID_EVT_CLOSE,      /* open -> closed edge observed  */
    ACPI_LID_EVT_OPEN        /* closed -> open edge observed  */
} acpi_lid_event_t;

acpi_lid_event_t acpi_lid_event(void);

/* Put a device (located by _HID/_CID) into D0 by evaluating its _PS0 (ungates,
 * e.g., an LPSS UART clock).  Returns 1 on success, 0 otherwise. */
int acpi_device_power_on(const char *hid);

/* ---- UART-controller power bring-up (Surface SAM path) ---------------------
 * A UART *client* device (e.g. the Surface Serial Hub, MSHW0084) does not own
 * the UART silicon - its _CRS UARTSerialBusV2 descriptor names the *controller*
 * device (the LPSS UART PCI function) as its ResourceSource.  Powering the link
 * on the way an OS does means driving the CONTROLLER to D0 - its _PR0 power
 * resources' _ON, then its _PS0 - and only then the client's own _PR0/_PS0.
 * acpi_uart_power_on() does exactly that, resolving the controller from the
 * client's _CRS (falling back to a namespace-wide _ADR search for 'pci_adr' =
 * (dev << 16) | fn, pass 0xFFFFFFFF for none). */
typedef struct {
    int      ran;
    int      client_found;      /* the client HID exists / is present         */
    char     ctrl_path[64];     /* controller path (from _CRS ResourceSource) */
    int      ctrl_found;        /* controller node resolved                   */
    int      ctrl_by_adr;       /* ...via the _ADR fallback, not _CRS         */
    uint32_t ctrl_mmio;         /* controller _CRS memory base (0 = none)     */
    uint32_t client_baud;       /* client _CRS UART baud (post-_INI, so the
                                 * REAL rate - e.g. Surface Laptop Go 1's _INI
                                 * patches 3,000,000 into the descriptor)     */
    int      ctrl_pr0_n,   ctrl_pr0_on;    /* _PR0 resources found / _ON ok   */
    int      client_pr0_n, client_pr0_on;
    uint32_t ctrl_ps0_st,  client_ps0_st;  /* raw uacpi_status of each _PS0   */
} acpi_uart_power_diag;

int  acpi_uart_power_on(const char *client_hid, uint32_t pci_adr);
void acpi_uart_power_get_diag(acpi_uart_power_diag *out);

/* Copy the latest diagnostics (arena stats refreshed at call time). */
void acpi_power_get_diag(acpi_power_diag *out);

/* Short human string for a bringup status. */
const char *acpi_power_status_str(acpi_power_status s);
