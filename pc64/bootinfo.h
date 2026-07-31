/* bootinfo.h - the ABI between the real-mode loader and the 64-bit kernel.
 *
 * boot/bios_stage2.asm fills this block at a fixed physical address before it
 * enters long mode; the kernel's BIOS entry receives a pointer to it. There is
 * no equivalent on the UEFI path, where the firmware answers these questions
 * directly - this exists only because on a BIOS boot nothing will be able to
 * answer them again once real mode is gone.
 *
 * EVERY FIELD IS uint32_t so stage2 can write it with a plain 32-bit store at a
 * known byte offset while still in 16-bit code (see fill_bootinfo). Widening a
 * field is a break; append instead, and bump `size`.
 *
 * `magic` and `size` come first and exist for one reason: stage2 lives in the
 * first 16 sectors of the disk and the kernel in the rest, so the two halves
 * can be updated independently and WILL eventually be mismatched. A kernel that
 * finds the wrong magic can say so; one that trusts the block reads a wild
 * pointer and triple-faults, which is the least diagnosable failure a boot can
 * have.
 */
#ifndef PC64_BOOTINFO_H
#define PC64_BOOTINFO_H

#include <stdint.h>

#define BOOTINFO_ADDR   0x1000u     /* must match BOOTINFO in bios_stage2.asm */
#define BOOTINFO_MAGIC  0x36364F42u /* 'BO66'                                 */
#define BOOTINFO_SIZE   48u

typedef struct uno_bootinfo {
    uint32_t magic;          /* +0  BOOTINFO_MAGIC                            */
    uint32_t size;           /* +4  bytes of this struct the loader filled    */
    uint32_t fb_addr;        /* +8  linear framebuffer physical address       */
    uint32_t fb_pitch;       /* +12 BYTES per scanline (not pixels)           */
    uint32_t fb_width;       /* +16 pixels                                    */
    uint32_t fb_height;      /* +20 pixels                                    */
    uint32_t fb_bpp;         /* +24 bits per pixel; always 32 on this path    */
    uint32_t mmap_count;     /* +28 number of E820 entries                    */
    uint32_t mmap_addr;      /* +32 physical address of the E820 entry array  */
    uint32_t kernel_sectors; /* +36 sectors the loader read for the kernel    */
    uint32_t boot_drive;     /* +40 BIOS drive number the machine booted from */
    uint32_t kernel_base;    /* +44 where the kernel was loaded (0x100000)    */
} uno_bootinfo;

/* One E820 entry as INT 15h AX=E820h returns it (the 24-byte form). */
typedef struct uno_e820 {
    uint64_t base;
    uint64_t length;
    uint32_t type;           /* 1 = usable RAM                                */
    uint32_t acpi;           /* ACPI 3.0 extended attributes                  */
} uno_e820;

#define UNO_E820_USABLE 1

#endif /* PC64_BOOTINFO_H */
