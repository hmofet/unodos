/* loadertest.c - the smallest possible 64-bit payload for the BIOS loader.
 *
 * This exists so phase A can be proven WITHOUT the operating system. If the
 * desktop fails to come up on a BIOS boot, the first question is always "did
 * the loader do its job?", and answering it with the full kernel means
 * debugging two new things at once. This payload answers it alone: it runs only
 * if the machine is in 64-bit long mode with the loader's page tables live, and
 * it can only draw if the VBE mode and the bootinfo block are both right.
 *
 * It is the sibling of tools/skintest.c and tools/dsptest.c: a harness that
 * turns a build-and-boot cycle per hypothesis into one unambiguous answer.
 *
 * Build + run:  tools/mkbios.sh test   (see that script)
 *
 * What you should see: four colour bars across the top of the screen, a white
 * frame around the whole visible area, and then the machine sits still. Any
 * other outcome localises the fault:
 *
 *   nothing, cursor still blinking   stage2 never reached long mode
 *   'kernel read error'              INT 13h / the sector run is wrong
 *   'no VBE linear-framebuffer mode' the video half failed; nothing else ran
 *   the long-mode refusal message    the CPU has no EM64T, as designed
 *   garbage pixels / wrong stripe    fb_pitch or fb_addr is wrong
 *   instant reboot                   a triple fault: page tables or the GDT
 */
#include "../bootinfo.h"

/* No CRT, no libc, no relocations: every reference here has to resolve inside
 * this one object, because the loader copies the image to 0x100000 and jumps
 * straight in. */

static void fill(volatile unsigned int *fb, unsigned int pitch_px,
                 unsigned int x0, unsigned int y0,
                 unsigned int w, unsigned int h, unsigned int argb)
{
    unsigned int x, y;
    for (y = y0; y < y0 + h; y++)
        for (x = x0; x < x0 + w; x++)
            fb[(unsigned long long)y * pitch_px + x] = argb;
}

void uno_bios_loadertest(const uno_bootinfo *bi)
{
    volatile unsigned int *fb;
    unsigned int pitch_px, w, h, i;
    static const unsigned int bars[4] = {
        0x00FF0000u, 0x0000FF00u, 0x000000FFu, 0x00FFFFFFu
    };

    /* A wrong or absent block is the one failure that must not be drawn over:
     * without it there is no framebuffer address to draw to, and guessing one
     * would fault. Halt instead and let the caller's symptom table apply. */
    if (!bi || bi->magic != BOOTINFO_MAGIC || bi->size < (uint32_t)BOOTINFO_SIZE ||
        !bi->fb_addr || bi->fb_bpp != 32)
        for (;;) __asm__ volatile ("cli; hlt");

    fb = (volatile unsigned int *)(unsigned long long)bi->fb_addr;
    pitch_px = bi->fb_pitch / 4;        /* the field is BYTES per scanline */
    w = bi->fb_width;
    h = bi->fb_height;

    fill(fb, pitch_px, 0, 0, w, h, 0x00101828u);          /* clear */

    for (i = 0; i < 4; i++)                                /* four bars */
        fill(fb, pitch_px, (w / 4) * i, 0, w / 4, h / 8, bars[i]);

    /* A frame proves width, height and pitch all agree: get pitch wrong and the
     * right-hand edge shears diagonally instead of standing vertical. */
    fill(fb, pitch_px, 0, 0, w, 2, 0x00FFFFFFu);
    fill(fb, pitch_px, 0, h - 2, w, 2, 0x00FFFFFFu);
    fill(fb, pitch_px, 0, 0, 2, h, 0x00FFFFFFu);
    fill(fb, pitch_px, w - 2, 0, 2, h, 0x00FFFFFFu);

    for (;;) __asm__ volatile ("cli; hlt");
}
