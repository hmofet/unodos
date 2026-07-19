/* acpi_power.c - see acpi_power.h.  Portable; depends only on vendored uACPI,
 * acpi_arena, and ec_handler.  No OS/firmware headers. */
#include "acpi_power.h"
#include "acpi_arena.h"
#include "ec_handler.h"
#include "smbus_handler.h"

#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
#include <uacpi/namespace.h>
#include <uacpi/utilities.h>
#include <uacpi/opregion.h>
#include <uacpi/status.h>
#include <uacpi/acpi.h>
#include <uacpi/kernel_api.h>   /* uacpi_kernel_get_nanoseconds_since_boot */
#include <uacpi/internal/context.h>   /* g_uacpi_rt_ctx.has_global_lock (see below) */

static acpi_power_diag      g_pd;
static int                  g_done;
static uacpi_namespace_node *g_bat_node;   /* first present PNP0C0A */
static uacpi_namespace_node *g_lid_node;   /* first present PNP0C0D */

const char *acpi_power_status_str(acpi_power_status s)
{
    switch (s) {
    case ACPI_POWER_OK:       return "ok";
    case ACPI_POWER_ENOARENA: return "no-arena";
    case ACPI_POWER_EINIT:    return "init-fail";
    case ACPI_POWER_ELOAD:    return "load-fail";
    default:                  return "unknown";
    }
}

/* ---- namespace helpers ---------------------------------------------------- */
static uacpi_iteration_decision count_cb(void *user, uacpi_namespace_node *node,
                                         uacpi_u32 depth)
{
    (void)node; (void)depth;
    (*(uint32_t *)user)++;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

struct find_ctx { uacpi_namespace_node *node; int count; };
static uacpi_iteration_decision find_cb(void *user, uacpi_namespace_node *node,
                                        uacpi_u32 depth)
{
    (void)depth;
    struct find_ctx *fc = (struct find_ctx *)user;
    if (!fc->node) fc->node = node;
    fc->count++;
    return UACPI_ITERATION_DECISION_CONTINUE;
}
static uacpi_namespace_node *find_first_device(const char *hid, int *count)
{
    struct find_ctx fc = { UACPI_NULL, 0 };
    uacpi_find_devices(hid, find_cb, &fc);   /* only _STA-present devices match */
    if (count) *count = fc.count;
    return fc.node;
}

/* Evaluate a method returning a Package; read element 'idx' as an integer. */
static int eval_pkg_uint(uacpi_namespace_node *node, const char *method,
                         uacpi_size idx, uacpi_u64 *out)
{
    uacpi_object *ret = UACPI_NULL;
    if (uacpi_eval_typed(node, method, UACPI_NULL, UACPI_OBJECT_PACKAGE_BIT, &ret)
            != UACPI_STATUS_OK || !ret)
        return 0;
    uacpi_object_array arr;
    int ok = 0;
    if (uacpi_object_get_package(ret, &arr) == UACPI_STATUS_OK && idx < arr.count)
        ok = (uacpi_object_get_integer(arr.objects[idx], out) == UACPI_STATUS_OK);
    uacpi_object_unref(ret);
    return ok;
}

#define ACPI_UNKNOWN_U32 0xFFFFFFFFull   /* _BST/_BIX "unknown" sentinel */

/* A short shared cache so every caller (the OS gauge AND a diagnostic overlay)
 * sees the SAME live reading, and so we don't re-evaluate _BST at frame rate.
 * uacpi_kernel_get_nanoseconds_since_boot() is monotonic; g_*_at == 0 == never. */
#define BAT_CACHE_NS (2ull * 1000 * 1000 * 1000)   /* 2 s */
#define LID_CACHE_NS (1ull * 1000 * 1000 * 1000)   /* 1 s */
static int      g_bat_cache = -1, g_lid_cache = -1;
static uint64_t g_bat_at, g_lid_at;

int acpi_battery_percent(void)
{
    if (!g_bat_node)
        return -1;
    uint64_t now = uacpi_kernel_get_nanoseconds_since_boot();
    if (g_bat_at != 0 && (now - g_bat_at) < BAT_CACHE_NS)
        return g_bat_cache;

    int pct = -1;
    uacpi_u64 full = 0;                       /* last full charge capacity */
    if ((eval_pkg_uint(g_bat_node, "_BIX", 3, &full) && full && full != ACPI_UNKNOWN_U32) ||
        (eval_pkg_uint(g_bat_node, "_BIF", 2, &full) && full && full != ACPI_UNKNOWN_U32)) {
        uacpi_u64 rem = 0;                    /* remaining capacity (_BST[2]) */
        if (eval_pkg_uint(g_bat_node, "_BST", 2, &rem) && rem != ACPI_UNKNOWN_U32) {
            if (rem > full) rem = full;
            pct = (int)((rem * 100) / full);
        }
    }
    g_bat_cache = pct;
    g_bat_at = now ? now : 1;
    return pct;
}

int acpi_lid_state(void)
{
    if (!g_lid_node)
        return -1;
    uint64_t now = uacpi_kernel_get_nanoseconds_since_boot();
    if (g_lid_at != 0 && (now - g_lid_at) < LID_CACHE_NS)
        return g_lid_cache;

    uacpi_u64 v = 0;
    g_lid_cache = (uacpi_eval_simple_integer(g_lid_node, "_LID", &v) == UACPI_STATUS_OK)
                  ? (v ? 1 : 0) : -1;
    g_lid_at = now ? now : 1;
    return g_lid_cache;
}

int acpi_device_power_on(const char *hid)
{
    uacpi_namespace_node *node = find_first_device(hid, UACPI_NULL);
    if (!node)
        return 0;
    g_pd.ps0_ran = 1;
    g_pd.ps0_ok = (uacpi_execute_simple(node, "_PS0") == UACPI_STATUS_OK);
    return g_pd.ps0_ok;
}

acpi_power_status acpi_power_bringup(void)
{
    if (g_done)
        return g_pd.ok ? ACPI_POWER_OK
             : (g_pd.load_status ? ACPI_POWER_ELOAD : ACPI_POWER_EINIT);
    g_done = 1;
    g_pd.bat_percent = -1;
    g_pd.lid_state   = -1;

    if (!acpi_arena_ready())
        return ACPI_POWER_ENOARENA;

    /* NO_ACPI_MODE: never write ACPI_ENABLE/SMI_CMD.  The firmware has already
     * entered ACPI mode under UEFI, and writing power/SMI registers on unknown
     * firmware is hazardous; a read-only battery/lid client never needs it. */
    uacpi_status s = uacpi_initialize(UACPI_FLAG_NO_ACPI_MODE);
    g_pd.init_status = (uint32_t)s;
    g_pd.init_str    = uacpi_status_to_string(s);
    if (uacpi_unlikely_error(s))
        return ACPI_POWER_EINIT;

    s = uacpi_namespace_load();
    g_pd.load_status = (uint32_t)s;
    g_pd.load_str    = uacpi_status_to_string(s);
    if (uacpi_unlikely_error(s))
        return ACPI_POWER_ELOAD;

    /* Neutralise the ACPI hardware Global Lock.  For a `Lock`-qualified field
     * (common for EC battery fields, e.g. Dell's \ECBT/\ECG3), uACPI tries to
     * acquire the FACS global lock BEFORE dispatching to our region handler; the
     * handshake needs the firmware's GBL release SCI, which we never service
     * (no ACPI mode / no SCI), so the acquire spins to 65535 and returns
     * HARDWARE_TIMEOUT - aborting the read before it ever reaches our EC handler
     * (the Dell "FieldAsInteger, 0 EC reads" symptom).  ACPICA does exactly this
     * when the lock hardware doesn't respond (AcpiGbl_GlobalLockPresent = FALSE).
     * Safe here: we are a single-threaded, read-only battery/lid client with no
     * concurrent SMM contention, and uacpi_namespace_load() has just set this
     * true at the end of its event init, so clearing it now (before
     * namespace_initialize runs _STA/_INI) makes every locked field dispatch
     * straight to our handler.  (Also removes the 65535-spin latency elsewhere.) */
    g_uacpi_rt_ctx.has_global_lock = UACPI_FALSE;

    uacpi_table dsdt;
    g_pd.dsdt_found =
        (uacpi_table_find_by_signature(ACPI_DSDT_SIGNATURE, &dsdt) == UACPI_STATUS_OK);
    uacpi_namespace_node *root = uacpi_namespace_root();
    uint32_t n = 0;
    if (root)
        uacpi_namespace_for_each_child_simple(root, count_cb, &n);
    g_pd.ns_nodes = n;

    /* EC region handler, installed before namespace_initialize so its _REG pass
     * connects it.  Bounded, so a hostile/absent EC only fails reads. */
    g_pd.ec_present = wu_ec_init();
    if (root) {
        uacpi_install_address_space_handler(
            root, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, wu_ec_region_handler, UACPI_NULL);
        /* SMBus / GenericSerialBus: some batteries (e.g. Apple BAT0 -> SMB0.SBRW)
         * read _STA/_BST through an SMBus region.  The host must have called
         * wu_smbus_init() with the controller base first (no-op if it did not). */
        uacpi_install_address_space_handler(
            root, UACPI_ADDRESS_SPACE_SMBUS, wu_smbus_region_handler, UACPI_NULL);
        uacpi_install_address_space_handler(
            root, UACPI_ADDRESS_SPACE_GENERIC_SERIAL_BUS, wu_smbus_region_handler, UACPI_NULL);
    }

    s = uacpi_namespace_initialize();
    g_pd.nsinit_status = (uint32_t)s;
    g_pd.nsinit_str    = uacpi_status_to_string(s);
    /* non-fatal: a single device's _INI may fault; still read what did init */

    g_bat_node = find_first_device("PNP0C0A", &g_pd.batteries);
    g_lid_node = find_first_device("PNP0C0D", &g_pd.lids);

    g_pd.bat_percent = acpi_battery_percent();
    g_pd.bat_present = (g_pd.bat_percent >= 0);
    g_pd.lid_state   = acpi_lid_state();
    wu_ec_info(&g_pd.ec_cmd, &g_pd.ec_data, &g_pd.ec_from_ecdt,
               &g_pd.ec_reads, &g_pd.ec_timeouts);

    g_pd.ok = 1;
    return ACPI_POWER_OK;
}

void acpi_power_get_diag(acpi_power_diag *out)
{
    if (!out)
        return;
    /* Refresh battery/lid LIVE (cheap - self-limited to a few seconds) so the
     * overlay tracks the same value the OS gauge shows, rather than a snapshot
     * frozen at bring-up time. */
    if (g_pd.ok) {
        g_pd.bat_percent = acpi_battery_percent();
        g_pd.bat_present = (g_pd.bat_percent >= 0);
        g_pd.lid_state   = acpi_lid_state();
    }
    size_t used = 0, peak = 0, total = 0;
    acpi_arena_stats(&used, &peak, &total);
    g_pd.arena_used  = used;
    g_pd.arena_peak  = peak;
    g_pd.arena_total = total;
    wu_ec_info(&g_pd.ec_cmd, &g_pd.ec_data, &g_pd.ec_from_ecdt,
               &g_pd.ec_reads, &g_pd.ec_timeouts);
    if (!g_pd.init_str)   g_pd.init_str   = "-";
    if (!g_pd.load_str)   g_pd.load_str   = "-";
    if (!g_pd.nsinit_str) g_pd.nsinit_str = "-";
    *out = g_pd;
}
