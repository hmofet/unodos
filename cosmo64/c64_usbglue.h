/* cosmo64/c64_usbglue.h -- FORCE-INCLUDED (clang -include) into pc64's
 * xhci.c and usbhid.c when they are compiled for this platform. Never
 * #included by name; build.sh puts it in front of those two files and no
 * others. It exists so that the USB lane's files compile here UNCHANGED.
 *
 * It does two things:
 *
 * 1. DMA MEMORY (xhci.c only, under C64_XDMA). The xHCI is a bus master and
 *    on this SoC it is not coherent with the CPU caches: a TRB written into a
 *    write-back line is invisible to the controller until the line is
 *    evicted, and a descriptor the controller writes into DRAM is hidden
 *    behind whatever the cache already holds. x86 never had this problem, so
 *    xhci.c keeps all of its DMA structures -- rings, contexts, scratchpads,
 *    the buffers it hands the controller -- in ordinary static .bss with no
 *    allocation seam to redirect. The pragma below moves EVERY zero-initialised
 *    static in the translation unit, function-local statics included, into a
 *    section of its own (".xdma"); flatten.py records where the linker put it,
 *    and mmu.c maps those pages as Device memory. Device rather than merely
 *    non-cacheable, because the ring writes and the doorbell write must reach
 *    the controller in program order and xhci.c has no barriers to ask for
 *    that -- Device-nGnRnE gives it for free. The driver's ordinary state
 *    variables ride along into the same section; that costs a bus cycle per
 *    access on a driver that is polled a few times per frame, which is
 *    nothing.
 *
 * 2. DIAGNOSTICS (both files). xhci.c narrates its bring-up through
 *    uno_dbg_log(), which uno_debug.h turns into ((void)0) outside a debug
 *    build. Those lines are exactly the ones a hardware boot needs on the eMMC
 *    log, so uno_debug.h is pre-empted here (its include guard is defined, so
 *    the real header contributes nothing) and uno_dbg_log goes to log.c.
 */
#ifndef C64_USBGLUE_H
#define C64_USBGLUE_H

#ifdef C64_XDMA
#pragma clang section bss = ".xdma"
#endif

#define UNO_DEBUG_H
void c64_dbg_log(const char *fmt, ...);
#define uno_dbg_log(...) c64_dbg_log(__VA_ARGS__)
#define uno_dbg_heartbeat() ((void)0)

#endif
