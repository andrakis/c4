/* C4PC.COM: one segment, loaded at 0x100 */
OUTPUT_FORMAT(binary)
SECTIONS {
  . = 0x100;
  .text : { *(.start) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  __bss_start = .;
  .bss : { *(.bss*) *(COMMON) }
  __bss_end = .;
  /DISCARD/ : { *(.note*) *(.comment*) *(.eh_frame*) }
}
