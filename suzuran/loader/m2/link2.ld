ENTRY(_start)
SECTIONS {
  . = 0x80000;
  .text : { *(.text.head) *(.text*) }
  . = ALIGN(2048);
  .vectors : { *(.text.vectors) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  .bss : { *(.bss*) *(COMMON) }
  . = ALIGN(16); . += 0x10000; _stack_top = .;
  _image_end = .;
}
