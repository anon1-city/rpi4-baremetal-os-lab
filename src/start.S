.section .text.boot
.global _start
.extern main
.extern __bss_start
.extern __bss_end
.extern _stack_top
.extern _exc_stack_top


_start:
    .cfi_startproc
    .cfi_undefined x30      // nothing called _start: GDB's backtrace ends here
    ldr x0, =_stack_top
    mov sp, x0

    // If booted in EL2, drop to EL1h.
    mrs x1, CurrentEL
    cmp x1, #0x8
    b.ne 1f

    mov x2, #(1 << 31)      // HCR_EL2.RW = 1 (EL1 is AArch64)
    orr x2, x2, #(1 << 1)
    msr hcr_el2, x2

    // Allow EL1 access to generic timer/counter.
    mov x2, #3
    msr cnthctl_el2, x2
    mov x2, #0
    msr cntvoff_el2, x2

    msr sp_el1, x0

    mov x2, #0x3C5          // EL1h, DAIF masked on entry
    msr spsr_el2, x2
    adr x2, 1f
    msr elr_el2, x2
    eret

1:
    // Two stacks: SP_EL1 is the exception stack (the GDB stub and interrupt
    // handlers run on it), SP_EL0 is the program's own stack. The program runs
    // at EL1t, so a debugger stop never touches the program's stack.
    msr spsel, #1
    ldr x0, =_exc_stack_top
    mov sp, x0
    msr spsel, #0
    ldr x0, =_stack_top
    mov sp, x0

    // Clear .bss: the stub's state lives there, and RAM is not guaranteed to be
    // zero after the firmware has run (QEMU zeroes it, real hardware does not).
    ldr x1, =__bss_start
    ldr x2, =__bss_end
    mov x3, #0
2:
    cmp x1, x2
    b.hs 3f
    str x3, [x1], #8
    b 2b
3:
    // Debug registers only work with the OS (double) lock released.
    msr osdlr_el1, xzr
    msr oslar_el1, xzr

    // Do not trap FP/SIMD at EL1: GDB reads v0-v31 straight from the hardware.
    mov x0, #(3 << 20)
    msr cpacr_el1, x0

    ldr x0, =vectors
    msr vbar_el1, x0
    isb

    // Terminate the frame chain so GDB backtraces end cleanly at _start.
    mov x29, xzr
    mov x30, xzr
    bl main
hang:
    wfe
    b hang
    .cfi_endproc

#include "exception_frame.h"
