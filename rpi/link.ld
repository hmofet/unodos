/* UnoDOS / Raspberry Pi (AArch64) linker script.
   The firmware loads kernel8.img flat at 0x80000 and jumps there in EL2/EL1.
   Variables, the mailbox buffer and the framebuffer all use fixed absolute
   addresses (the framebuffer base itself is allocated at run time by the GPU
   via the mailbox), so only the image (.text + .rodata) is placed here. */
ENTRY(_start)
SECTIONS {
  . = 0x80000;
  .text : {
    KEEP(*(.text._start))
    *(.text*)
    *(.rodata*)
    . = ALIGN(8);
  }
  /DISCARD/ : { *(.comment) *(.ARM.attributes) *(.note*) }
}
