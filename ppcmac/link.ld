/* UnoDOS / PowerPC Mac (32-bit PowerPC) linker script.
   Open Firmware loads the client program into RAM and enters at _start with the
   OF client-interface entry in r5. We link at a fixed RAM address; variables, the
   OF call buffer and the framebuffer all use fixed absolute addresses (the
   framebuffer base itself is fetched from OF's `screen` device at run time). */
ENTRY(_start)
SECTIONS {
  . = 0x00100000;
  .text : {
    KEEP(*(.text._start))
    *(.text*)
    *(.rodata*)
    . = ALIGN(8);
  }
  /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame) }
}
