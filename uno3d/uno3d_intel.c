/* ===========================================================================
 * uno3d Intel backend (u3d_backend_intel) - pc64 hardware-3D scaffold.
 *
 * The uno3d backend interface is "clear / rasterise one screen-space triangle
 * / flush / present". A real Intel iGPU backend would map those onto the Gen
 * graphics command streamer: a GEM-style buffer allocator, a batch buffer of
 * 3DPRIMITIVE commands feeding the Gen render pipeline (VS pass-through +
 * fixed-function setup + a trivial pixel shader emitting the gouraud colour),
 * a depth buffer bound as a render target, and a flip via the display engine
 * (or a blit into the GOP scanout). That is a substantial driver - GTT setup,
 * ring/execlist submission, per-generation MMIO - and belongs to the same
 * "native driver tail" as xHCI/NVMe.
 *
 * Until that lands, this backend HONESTLY falls back to the software
 * rasteriser: init() probes PCI for an Intel display-class device (8086,
 * class 0x03) and reports whether one is present, but every draw call is
 * delegated to u3d_backend_soft. So selecting "intel" always renders
 * correctly (via the CPU), and the day the real command-streamer path is
 * written, only the four hooks below change - no app or pipeline edits.
 *
 * This mirrors the family's other backends being "built for more targets":
 * the vtable is the contract; the hardware path is swapped in behind it.
 * ======================================================================== */
#include "uno3d_backend.h"
#include "pc64_pci.h"

static int g_have_intel;

int uno3d_intel_present(void) { return g_have_intel; }

static int intel_init(int w, int h)
{
    pci_dev d;
    g_have_intel = 0;
    /* Intel VGA/display controller: vendor 8086, class 0x03 (display) */
    if (pci_find_class(0x03, 0x00, &d) && d.vendor == 0x8086)
        g_have_intel = 1;
    else if (pci_find_class(0x03, 0x80, &d) && d.vendor == 0x8086)
        g_have_intel = 1;
    /* hardware path not implemented yet -> the software backend owns the
       actual rasterisation (identical output, CPU-timed) */
    return u3d_backend_soft.init(w, h);
}
static void intel_shutdown(void) { u3d_backend_soft.shutdown(); }
static void intel_clear(unsigned char r, unsigned char g, unsigned char b)
{ u3d_backend_soft.clear(r, g, b); }
static void intel_tri(const u3d_stri *t) { u3d_backend_soft.tri(t); }
static void intel_flush(void) { u3d_backend_soft.flush(); }
static void intel_present(void) { u3d_backend_soft.present(); }

const u3d_backend u3d_backend_intel = {
    "intel(soft-fallback)",
    U3D_CAP_ZBUFFER | U3D_CAP_GOURAUD,     /* no U3D_CAP_HW yet - honest */
    intel_init, intel_shutdown, intel_clear, intel_tri, intel_flush, intel_present
};
