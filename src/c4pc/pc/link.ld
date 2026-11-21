/* c4pc.elf: loaded at 1 MB, as multiboot loaders expect */
ENTRY(_start)
SECTIONS {
  . = 1M;
  .multiboot : { *(.multiboot) }
  .text : { *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(COMMON) *(.bss*) }
  _end = .;
  /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
