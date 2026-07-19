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
#include <uacpi/resources.h>
#include <uacpi/opregion.h>
#include <uacpi/status.h>
#include <uacpi/acpi.h>
#include <uacpi/kernel_api.h>   /* uacpi_kernel_get_nanoseconds_since_boot */
#include <uacpi/internal/context.h>   /* g_uacpi_rt_ctx.has_global_lock (see below) */
#include <uacpi/internal/namespace.h> /* uacpi_namespace_node_get_object (match
                                       * _PR0 references to their nodes, below) */

static acpi_power_diag      g_pd;
static int                  g_done;
static uacpi_namespace_node *g_bat_node;   /* first present PNP0C0A */
static uacpi_namespace_node *g_lid_node;   /* first present PNP0C0D */

/* EC _CRS port collection (data port first, command/status second). */
static struct { uint16_t ports[2]; int n; } g_ec_crs;
static uacpi_iteration_decision ec_crs_cb(void *user, uacpi_resource *r)
{
    (void)user;
    uint16_t port = 0;
    if (r->type == UACPI_RESOURCE_TYPE_IO)            port = r->io.minimum;
    else if (r->type == UACPI_RESOURCE_TYPE_FIXED_IO) port = r->fixed_io.address;
    if (port && g_ec_crs.n < 2) g_ec_crs.ports[g_ec_crs.n++] = port;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

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

/* ---- UART-controller power bring-up (see acpi_power.h) --------------------- */
static acpi_uart_power_diag g_ud;

/* Match a _PR0 package element (a reference to a PowerResource) back to its
 * namespace node so _ON can be executed on it.  Method returns are shallow
 * copies in uACPI, so the dereferenced element IS the node's own object. */
struct pwr_find { uacpi_object *target; uacpi_namespace_node *node; };
static uacpi_iteration_decision pwr_find_cb(void *user, uacpi_namespace_node *node,
                                            uacpi_u32 depth)
{
    (void)depth;
    struct pwr_find *pf = (struct pwr_find *)user;
    if (uacpi_namespace_node_get_object(node) == pf->target) {
        pf->node = node;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* Evaluate dev._PR0 and run _ON on every referenced power resource.
 * uACPI stores named objects inside static packages as PATH STRINGS (resolved
 * lazily, mirroring NT), so the common case is a string element; a reference
 * element (dynamically built package) is matched back to its node by object
 * pointer instead. */
static void run_pr0_on(uacpi_namespace_node *dev, int *n_out, int *on_out)
{
    *n_out = 0; *on_out = 0;
    uacpi_object *ret = UACPI_NULL;
    if (uacpi_eval_typed(dev, "_PR0", UACPI_NULL, UACPI_OBJECT_PACKAGE_BIT, &ret)
            != UACPI_STATUS_OK || !ret)
        return;
    uacpi_object_array arr;
    if (uacpi_object_get_package(ret, &arr) == UACPI_STATUS_OK) {
        *n_out = (int)arr.count;
        for (uacpi_size i = 0; i < arr.count; i++) {
            uacpi_object *el = arr.objects[i], *deref = UACPI_NULL;
            uacpi_namespace_node *pr = UACPI_NULL;
            uacpi_data_view sv;
            if (uacpi_object_get_string(el, &sv) == UACPI_STATUS_OK &&
                sv.text && sv.length) {
                /* path string: absolute first, then scope-relative w/ search-up */
                if (uacpi_namespace_node_find(UACPI_NULL, sv.text, &pr)
                        != UACPI_STATUS_OK)
                    pr = UACPI_NULL;
                if (!pr &&
                    uacpi_namespace_node_resolve_from_aml_namepath(dev, sv.text,
                                                                   &pr)
                        != UACPI_STATUS_OK)
                    pr = UACPI_NULL;
            } else {
                if (uacpi_object_get_type(el) == UACPI_OBJECT_REFERENCE &&
                    uacpi_object_get_dereferenced(el, &deref) == UACPI_STATUS_OK)
                    el = deref;
                struct pwr_find pf = { el, UACPI_NULL };
                uacpi_namespace_for_each_child(
                    uacpi_namespace_root(), pwr_find_cb, UACPI_NULL,
                    UACPI_OBJECT_POWER_RESOURCE_BIT, UACPI_MAX_DEPTH_ANY, &pf);
                pr = pf.node;
            }
            if (pr && uacpi_execute_simple(pr, "_ON") == UACPI_STATUS_OK)
                (*on_out)++;
            if (deref)
                uacpi_object_unref(deref);
        }
    }
    uacpi_object_unref(ret);
}

/* Client _CRS walk: copy the serial-bus ResourceSource (controller path) and
 * the UART baud.  _CRS is evaluated at runtime (after namespace_initialize ran
 * _INI), so a descriptor whose speed field is patched by _INI - the Surface
 * pattern - reads back with the REAL rate here, not the static-AML zero. */
static uacpi_iteration_decision uart_src_cb(void *user, uacpi_resource *r)
{
    (void)user;
    if ((r->type == UACPI_RESOURCE_TYPE_SERIAL_UART_CONNECTION ||
         r->type == UACPI_RESOURCE_TYPE_SERIAL_I2C_CONNECTION ||
         r->type == UACPI_RESOURCE_TYPE_SERIAL_SPI_CONNECTION) &&
        r->serial_bus_common.source.string && !g_ud.ctrl_path[0]) {
        const char *s = r->serial_bus_common.source.string;
        size_t i = 0;
        for (; s[i] && i < sizeof(g_ud.ctrl_path) - 1; i++)
            g_ud.ctrl_path[i] = s[i];
        g_ud.ctrl_path[i] = 0;
    }
    if (r->type == UACPI_RESOURCE_TYPE_SERIAL_UART_CONNECTION &&
        !g_ud.client_baud)
        g_ud.client_baud = r->uart_connection.baud_rate;
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* Controller _CRS walk: first memory descriptor = the UART's MMIO base. */
static uacpi_iteration_decision uart_mmio_cb(void *user, uacpi_resource *r)
{
    (void)user;
    if (!g_ud.ctrl_mmio) {
        if (r->type == UACPI_RESOURCE_TYPE_FIXED_MEMORY32)
            g_ud.ctrl_mmio = r->fixed_memory32.address;
        else if (r->type == UACPI_RESOURCE_TYPE_MEMORY32)
            g_ud.ctrl_mmio = r->memory32.minimum;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* _ADR fallback: find a device whose _ADR == the wanted PCI (dev << 16) | fn. */
struct adr_find { uacpi_u64 adr; uacpi_namespace_node *node; };
static uacpi_iteration_decision adr_find_cb(void *user, uacpi_namespace_node *node,
                                            uacpi_u32 depth)
{
    (void)depth;
    struct adr_find *af = (struct adr_find *)user;
    uacpi_u64 v = 0;
    if (uacpi_eval_simple_integer(node, "_ADR", &v) == UACPI_STATUS_OK &&
        v == af->adr) {
        af->node = node;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

int acpi_uart_power_on(const char *client_hid, uint32_t pci_adr)
{
    g_ud.ran = 1;
    g_ud.ctrl_ps0_st = g_ud.client_ps0_st = 0xFFFFFFFFu;   /* "not run" */

    uacpi_namespace_node *client = find_first_device(client_hid, UACPI_NULL);
    g_ud.client_found = (client != UACPI_NULL);

    /* Resolve the controller from the client's _CRS ResourceSource. */
    uacpi_namespace_node *ctrl = UACPI_NULL;
    if (client) {
        uacpi_resources *res = UACPI_NULL;
        if (uacpi_get_current_resources(client, &res) == UACPI_STATUS_OK && res) {
            uacpi_for_each_resource(res, uart_src_cb, UACPI_NULL);
            uacpi_free_resources(res);
        }
        if (g_ud.ctrl_path[0]) {
            if (uacpi_namespace_node_find(UACPI_NULL, g_ud.ctrl_path, &ctrl)
                    != UACPI_STATUS_OK)
                ctrl = UACPI_NULL;
            if (!ctrl &&
                uacpi_namespace_node_resolve_from_aml_namepath(
                    uacpi_namespace_node_parent(client), g_ud.ctrl_path, &ctrl)
                    != UACPI_STATUS_OK)
                ctrl = UACPI_NULL;
        }
    }
    if (!ctrl && pci_adr != 0xFFFFFFFFu) {   /* fallback: search by PCI _ADR */
        struct adr_find af = { pci_adr, UACPI_NULL };
        uacpi_namespace_for_each_child(
            uacpi_namespace_root(), adr_find_cb, UACPI_NULL,
            UACPI_OBJECT_DEVICE_BIT, UACPI_MAX_DEPTH_ANY, &af);
        ctrl = af.node;
        if (ctrl) {
            g_ud.ctrl_by_adr = 1;
            const char *p = uacpi_namespace_node_generate_absolute_path(ctrl);
            if (p) {
                size_t i = 0;
                for (; p[i] && i < sizeof(g_ud.ctrl_path) - 1; i++)
                    g_ud.ctrl_path[i] = p[i];
                g_ud.ctrl_path[i] = 0;
                uacpi_free_absolute_path(p);
            }
        }
    }
    g_ud.ctrl_found = (ctrl != UACPI_NULL);

    /* Controller to D0 the way an OS does: _PR0 resources _ON, then _PS0.
     * Then the client's own _PR0/_PS0 (it may reference the now-live clock). */
    if (ctrl) {
        run_pr0_on(ctrl, &g_ud.ctrl_pr0_n, &g_ud.ctrl_pr0_on);
        g_ud.ctrl_ps0_st = (uint32_t)uacpi_execute_simple(ctrl, "_PS0");
        uacpi_resources *res = UACPI_NULL;
        if (uacpi_get_current_resources(ctrl, &res) == UACPI_STATUS_OK && res) {
            uacpi_for_each_resource(res, uart_mmio_cb, UACPI_NULL);
            uacpi_free_resources(res);
        }
    }
    if (client) {
        run_pr0_on(client, &g_ud.client_pr0_n, &g_ud.client_pr0_on);
        g_ud.client_ps0_st = (uint32_t)uacpi_execute_simple(client, "_PS0");
        g_pd.ps0_ran = 1;                    /* keep the old PS0= overlay live */
        g_pd.ps0_ok  = (g_ud.client_ps0_st == UACPI_STATUS_OK);
    }
    return g_ud.ctrl_found && g_ud.ctrl_ps0_st == UACPI_STATUS_OK;
}

void acpi_uart_power_get_diag(acpi_uart_power_diag *out)
{
    if (out)
        *out = g_ud;
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

    /* Without an ECDT (e.g. the Dell) the EC ports must come from the PNP0C09
     * _CRS - two IO descriptors, data port first, command/status second.  Do it
     * after namespace_initialize so _CRS/_STA are reliably evaluable. */
    int ec_src = 0;
    wu_ec_info(UACPI_NULL, UACPI_NULL, &ec_src, UACPI_NULL, UACPI_NULL);
    if (ec_src != 1) {   /* not from an ECDT -> try the PNP0C09 _CRS */
        int ec_cnt = 0;
        uacpi_namespace_node *ecn = find_first_device("PNP0C09", &ec_cnt);
        if (ecn) {
            uacpi_resources *res = UACPI_NULL;
            if (uacpi_get_current_resources(ecn, &res) == UACPI_STATUS_OK && res) {
                uacpi_for_each_resource(res, ec_crs_cb, UACPI_NULL);
                uacpi_free_resources(res);
                if (g_ec_crs.n >= 2)
                    wu_ec_set_ports(g_ec_crs.ports[1], g_ec_crs.ports[0]);  /* [1]=cmd, [0]=data */
            }
        }
    }

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
