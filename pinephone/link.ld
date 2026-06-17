/* UnoDOS / PinePhone (Allwinner A64, AArch64) linker script.
   The boot chain (SPL/U-Boot) loads the AArch64 payload into DRAM at the standard
   arm64 load address 0x40080000 and jumps there. Variables, the DE2 register pokes
   and the framebuffer all use fixed absolute DRAM addresses, so only the image
   (.text + .rodata) is placed here. */
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
