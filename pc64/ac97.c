/* ===========================================================================
 * UnoDOS/pc64 - AC'97 driver (Intel ICH "AC'97 Audio Controller" layout).
 *
 * The pre-HDA generation (ICH0..ICH5 boards, and QEMU's -device AC97). Two
 * I/O BARs: BAR0 = the codec mixer (NAM), BAR1 = the bus master (NABM).
 * Playback only, polled, no interrupts - same shape as the port's other
 * silicon drivers.
 *
 * The PCM-out bus master walks a 32-entry buffer-descriptor list whose
 * entries tile one contiguous 48 kHz s16 stereo ring. LVI is kept one lap
 * behind CIV (uno_ac97_kick, called from the PCM layer's poll) so the DMA
 * engine treats the ring as endless; if it ever does halt (a missed poll
 * during a long blocking operation) the kick rewrites LVI and re-runs it.
 *
 * Bounds: every wait is an iteration-bounded spin - a dead codec fails
 * probe instead of hanging boot.
 * ======================================================================== */
#include "pc64_pci.h"
#include "ac97.h"
#include <stdint.h>

static inline void outb_(uint16_t p, uint8_t v)  { __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outw_(uint16_t p, uint16_t v) { __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(p)); }
static inline void outl_(uint16_t p, uint32_t v) { __asm__ volatile ("outl %0, %1" : : "a"(v), "Nd"(p)); }
static inline uint8_t  inb_(uint16_t p) { uint8_t  v; __asm__ volatile ("inb %1, %0"  : "=a"(v) : "Nd"(p)); return v; }
static inline uint16_t inw_(uint16_t p) { uint16_t v; __asm__ volatile ("inw %1, %0"  : "=a"(v) : "Nd"(p)); return v; }
static inline uint32_t inl_(uint16_t p) { uint32_t v; __asm__ volatile ("inl %1, %0"  : "=a"(v) : "Nd"(p)); return v; }

/* ---- NAM (mixer) registers ------------------------------------------------ */
#define NAM_RESET    0x00
#define NAM_MASTER   0x02              /* master volume (0x0000 = 0 dB)        */
#define NAM_AUXOUT   0x04              /* aux/headphone volume                 */
#define NAM_PCMOUT   0x18              /* PCM out gain (0x0808 = 0 dB)         */

/* ---- NABM: PCM-out box (+0x10) + global registers ------------------------- */
#define PO_BDBAR     0x10              /* descriptor-list base (dword)         */
#define PO_CIV       0x14              /* current index (byte, RO)             */
#define PO_LVI       0x15              /* last valid index (byte)              */
#define PO_SR        0x16              /* status (word; bits 2-4 are W1C)      */
#define PO_PICB      0x18              /* samples left in current entry (word) */
#define PO_CR        0x1B              /* control: bit0 run, bit1 reset        */
#define GLOB_CNT     0x2C              /* bit1 = cold reset deasserted         */
#define GLOB_STA     0x30              /* bit8 = primary codec ready           */

#define SR_DCH       0x01              /* DMA halted                           */
#define SR_W1C       0x1C              /* LVBCI | BCIS | FIFOE                 */

#define SPIN_MAX     4000000u
#define NDESC        32u
#define ENTRY_FRAMES 512u              /* 32 * 512 = 16384-frame ring (~341ms) */
#define RING_FRAMES  (NDESC * ENTRY_FRAMES)

static struct { uint32_t addr; uint16_t len, flags; }
              gBdl[NDESC] __attribute__((aligned(8)));
static short  gRing[RING_FRAMES * 2] __attribute__((aligned(8)));

static uint16_t gNam, gNabm;
static int gUp;

int uno_ac97_init(void)
{
    pci_dev d; unsigned i; uint16_t cmd;
    uint64_t b0, b1;

    if (!pci_find_class(0x04, 0x01, &d)) return 0;
    cmd = pci_cfg_read16(&d, 0x04);              /* I/O decode + bus master    */
    pci_cfg_write16(&d, 0x04, cmd | 0x5);
    b0 = pci_bar(&d, 0); b1 = pci_bar(&d, 1);
    if (!b0 || !b1 || b0 > 0xFFFF || b1 > 0xFFFF) return 0;   /* I/O ports    */
    gNam = (uint16_t)b0; gNabm = (uint16_t)b1;

    outl_(gNabm + GLOB_CNT, inl_(gNabm + GLOB_CNT) | 0x2);    /* cold reset off */
    for (i = 0; i < SPIN_MAX && !(inl_(gNabm + GLOB_STA) & 0x100); i++) { }
    if (!(inl_(gNabm + GLOB_STA) & 0x100)) return 0;          /* codec dead   */

    outw_(gNam + NAM_RESET, 0);                  /* mixer to defaults          */
    outw_(gNam + NAM_MASTER, 0x0000);            /* 0 dB, unmuted              */
    outw_(gNam + NAM_AUXOUT, 0x0000);
    outw_(gNam + NAM_PCMOUT, 0x0808);            /* 0 dB, unmuted              */
    /* no VRA: the fixed 48 kHz default matches the PCM layer's rate */

    for (i = 0; i < NDESC; i++) {
        gBdl[i].addr  = (uint32_t)(uintptr_t)(gRing + i * ENTRY_FRAMES * 2);
        gBdl[i].len   = ENTRY_FRAMES * 2;        /* length is in SAMPLES       */
        gBdl[i].flags = 0;                       /* no IOC, no BUP             */
    }

    outb_(gNabm + PO_CR, 0x2);                   /* channel reset              */
    for (i = 0; i < SPIN_MAX && (inb_(gNabm + PO_CR) & 0x2); i++) { }
    outl_(gNabm + PO_BDBAR, (uint32_t)(uintptr_t)gBdl);
    outw_(gNabm + PO_SR, SR_W1C);
    outb_(gNabm + PO_LVI, NDESC - 1);
    outb_(gNabm + PO_CR, 0x1);                   /* run                        */
    gUp = 1;
    return 1;
}

short *uno_ac97_ring(unsigned *frames) { *frames = RING_FRAMES; return gRing; }

unsigned uno_ac97_pos(void)
{
    uint8_t civ; uint16_t picb; unsigned done;
    if (!gUp) return 0;
    do {                                         /* CIV/PICB stable-pair read  */
        civ  = inb_(gNabm + PO_CIV);
        picb = inw_(gNabm + PO_PICB);
    } while (civ != inb_(gNabm + PO_CIV));
    done = ENTRY_FRAMES - (unsigned)picb / 2;
    return ((unsigned)civ * ENTRY_FRAMES + done) % RING_FRAMES;
}

void uno_ac97_kick(void)
{
    uint8_t civ, lvi;
    if (!gUp) return;
    civ = inb_(gNabm + PO_CIV);
    lvi = (uint8_t)((civ + NDESC - 1) % NDESC);  /* LVI = one lap behind CIV   */
    if (inb_(gNabm + PO_LVI) != lvi) outb_(gNabm + PO_LVI, lvi);
    outw_(gNabm + PO_SR, SR_W1C);
    if (inb_(gNabm + PO_SR) & SR_DCH)            /* halted (missed a lap):     */
        outb_(gNabm + PO_CR, 0x1);               /* re-run - ring is cyclic    */
}
