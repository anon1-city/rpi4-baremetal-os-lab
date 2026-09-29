ENTRY(_start)

SECTIONS
{
    . = 0x00200000;

    .text : ALIGN(16) {
        *(.text.boot)
        *(.text*)
    }

    .rodata : ALIGN(16) {
        *(.rodata*)
    }

    .data : ALIGN(16) {
        *(.data*)
    }

    .bss : ALIGN(16) {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end = .;
    }

    . = ALIGN(16);
    _end = .;
    _stack_top = _end + 0x4000;         /* the program's stack (SP_EL0) grows down from here */
    _exc_stack_top = _stack_top + 0x2000;   /* the exception stack (SP_EL1): stub + interrupt handlers */
}

