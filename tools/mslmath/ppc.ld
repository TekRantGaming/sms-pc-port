ENTRY(_start)
SECTIONS {
  . = 0x10000000;
  .text : { *(.text) }
  .ctors : { __ctors_start = .; KEEP(*(.ctors)) __ctors_end = .; }
  .rodata : { *(.rodata) }
  .data : { *(.data) }
  .sdata : { *(.sdata) }
  _SDA_BASE_ = ADDR(.sdata) + 0x8000;
  .sbss : { *(.sbss) }
  .sdata2 : { *(.sdata2) }
  _SDA2_BASE_ = ADDR(.sdata2) + 0x8000;
  .sbss2 : { *(.sbss2) }
  .bss : { *(.bss) }
  /DISCARD/ : { *(.comment) *(.dtors) }
}
