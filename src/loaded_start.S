.section .text.boot
.global _start
.extern loaded_main
.extern __bss_start
.extern __bss_end
.extern _stack_top
.extern _exc_stack_top


_start:
    .cfi_startproc
    .cfi_undefined x30      /* nothing called _start: GDB's backtrace ends here */

    /* Two stacks: SP_EL1 is the exception stack (the GDB stub and interrupt
     * handlers run on it), SP_EL0 is the program's own stack. The program runs
     * at EL1t, so a debugger stop never touches the program's stack. */
    msr spsel, #1
    ldr x0, =_exc_stack_top
    mov sp, x0
    msr spsel, #0
    ldr x0, =_stack_top
    mov sp, x0

    /* clear .bss */
    ldr x1, =__bss_start
    ldr x2, =__bss_end
    mov x3, #0
1:
    cmp x1, x2
    b.hs 2f
    str x3, [x1], #8
    b 1b
2:
    /* Debug registers only work with the OS (double) lock released. */
    msr osdlr_el1, xzr
    msr oslar_el1, xzr

    /* Do not trap FP/SIMD at EL1: GDB reads v0-v31 straight from the hardware. */
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    isb

    ldr x0, =vectors
    msr vbar_el1, x0
    isb

    /* Terminate the frame chain so GDB backtraces end cleanly at _start. */
    mov x29, xzr
    mov x30, xzr
    bl loaded_main
    .cfi_endproc

/* Vector table and handlers: see include/exception_frame.h. Every synchronous
 * exception goes to the GDB stub (BRK, single step, hardware breakpoints and
 * watchpoints, and real faults, which GDB reports as a signal instead of the CPU
 * silently re-faulting forever). A kernel that handles some exceptions itself
 * (system calls, page faults, ...) registers gdb_stub_set_hook(). */
#include "exception_frame.h"
