/* SAMPLE.UNO - the reference loadable driver (unodevices phase 4).
 *
 * This is the acceptance case from docs/UNODEVICES-PLAN.md ("a sample trivial
 * driver shipped as \DRIVERS\SAMPLE.UNO binds to a QEMU device"), and it is
 * also the worked example a real out-of-tree driver copies.
 *
 * It claims the PCH SMBus function, which is deliberately the most boring
 * device on a q35: nothing else in the tree wants it, the built-in drivers all
 * decline it, and it is not on any path the OS needs to boot. A sample driver
 * that binds something load-bearing would be a sample driver that can break
 * the machine.
 *
 * WHAT THIS FILE DEMONSTRATES, in order of how easy it is to get wrong:
 *
 *  1. It resolves NOTHING by name. Everything it can do arrives in the
 *     services struct, and it checks that struct's api before using it. A
 *     driver built against a different services layout would otherwise read
 *     function pointers at the wrong offsets, which is not a failure that
 *     announces itself.
 *  2. Its probe touches no hardware beyond config-space reads. A probe runs
 *     while the manager is still deciding who owns the device.
 *  3. Its remove() does nothing but forget. After remove() returns, a driver
 *     must not touch the device again (DEVICES.md 7), and the easiest way to
 *     obey that is to hold nothing worth touching.
 *
 * Build: compiled to a DLL and converted by tools/mkuno.py with flag 8
 * (UNO_MODF_DRV) into build/esp/DRIVERS/SAMPLE.UNO. See build.sh.
 */
#include "../uno_devmgr.h"

/* The services the manager handed us. Kept because a real driver would use
 * map_bar/dma_alloc/msi_enable from here; this one only proves it arrived. */
static const uno_drv_services *g_svc;
static uno_device *g_dev;
static unsigned int g_vendor_seen;

static int sample_probe(uno_device *d)
{
    if (!g_svc || !d) return 0;
    /* A config read is the most this may safely do: the manager has not
     * decided we own the device yet, and a declining probe must leave no
     * trace at all. */
    g_vendor_seen = g_svc->cfg_read32(d, 0x00);
    if ((g_vendor_seen & 0xFFFFu) == 0xFFFFu) return 0;   /* not really there */
    g_dev = d;
    g_svc->log("sample: claimed the smbus function\n");
    return 1;
}

static void sample_remove(uno_device *d)
{
    (void)d;
    /* Forget, and nothing else. The node we were passed on a hotplug removal
     * is a synthetic stand-in carrying only an address, so there is nothing
     * here that would still be valid to dereference. */
    g_dev = 0;
}

static const uno_match sample_match[] = {
    { UNO_MATCH_PCI_CLASS, 0, 0, 0x0C, 0x05, 0, 0 },   /* serial-bus / SMBus */
    { UNO_MATCH_END,       0, 0, 0,    0,    0, 0 }
};

static const uno_driver sample_drv = {
    "sample", UNO_BUS_PCI, UNO_DEVMGR_API, sample_match, sample_probe, sample_remove
};

static const uno_drv_module sample_mod = { UNO_DRVSVC_API, &sample_drv };

/* The module entry point. Returning NULL on an api mismatch is the whole
 * contract: the manager checks it too, but a driver that checks for itself
 * cannot be tricked by a manager that forgot to. */
const uno_drv_module *uno_drv_main(const uno_drv_services *svc)
{
    if (!svc || svc->api != UNO_DRVSVC_API) return 0;
    g_svc = svc;
    return &sample_mod;
}
