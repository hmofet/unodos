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
  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}
