/* UnoDOS / GBA linker script — a plain cartridge ROM at 0x08000000.
   Variables/framebuffer use fixed absolute addresses (IWRAM/VRAM), so only the
   ROM image (.text + .rodata) is placed here. */
ENTRY(_start)
MEMORY {
  rom (rx) : ORIGIN = 0x08000000, LENGTH = 32M
}
SECTIONS {
  .text : {
    *(.text._start)
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
  } > rom
  /DISCARD/ : { *(.comment) *(.ARM.attributes) }
}
