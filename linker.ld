ENTRY(_start)
SECTIONS {
  . = 0x80000;
  .text : { *(.text.boot) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  . = ALIGN(16);
  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss*) *(COMMON)
    . = ALIGN(16);
    __bss_end = .;
  }
  . = ALIGN(16);
  . += 0x10000;
  _stack_top = .;         /* the program's stack (SP_EL0) grows down from here */
  . += 0x2000;
  _exc_stack_top = .;     /* the exception stack (SP_EL1): stub + interrupt handlers */
}
