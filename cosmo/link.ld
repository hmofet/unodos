/* UnoDOS / Cosmo Communicator (MT6771, AArch64) linker script.
   Planet's LK loads our payload (wrapped in an Android boot.img) as the "kernel"
   per the arm64 Linux boot protocol: kernel_addr 0x40080000, DTB in x0, MMU off.
   The ARM64 Image header at _start carries text_offset 0x80000 so LK's load base
   0x40000000 + 0x80000 = 0x40080000 = this link address (non-PIC absolute code).
   Variables, FBINFO and the fallback framebuffer use fixed absolute DRAM addresses;
   the real framebuffer base is LK's, discovered at run time (adopt-FB). */
ENTRY(_start)
SECTIONS {
  . = 0x40080000;
  .text : {
    KEEP(*(.text._start))
    *(.text*)
    *(.rodata*)
    . = ALIGN(8);
  }
  /* The shadow surface lives here (shadow_buf, ~1.2 MB): image-OWNED memory,
     because page 1 of LK's vram is still scanned out by a leftover display
     layer in the p38 RECOVERY_BOOT2 boot mode (hardware, 2026-08-31). NOBITS,
     so objcopy -O binary does not grow the payload; fb_init zeroes it. */
  .bss (NOLOAD) : {
    . = ALIGN(16);
    *(.bss*)
    *(COMMON)
    . = ALIGN(16);
    __image_end = .;
  }
  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}
/* The stack grows down from STACK_TOP 0x40200000 (kernel.s); keep 128 KB for it. */
ASSERT(__image_end <= 0x401E0000,
       "image + shadow bss overruns the stack region below 0x40200000")
