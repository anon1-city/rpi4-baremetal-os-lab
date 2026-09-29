/* Exception entry code shared by src/start.S (bootloader) and src/loaded_start.S
 * (demo kernel). Assembly only: #include it from a .S file, after the boot code.
 *
 * Stack model. The program runs at EL1t, i.e. on SP_EL0. Every exception is taken
 * on SP_EL1, which _start points at a dedicated exception stack (_exc_stack_top).
 * So the debugger stub never uses the program's stack: a small task stack cannot
 * be overflowed by a debugger stop, and a fault caused by a corrupted sp is still
 * reported instead of hanging.
 *
 * Frame layout (must match gdb_regs_t in gdb_stub.h, which has a _Static_assert):
 *   x[0] @ 0 ... x[30] @ 240, sp @ 248, pc @ 256, pstate @ 264; 272 bytes,
 *   allocated as 288 to keep sp 16-byte aligned.
 *
 * "sp" in the frame is the program's stack pointer: SP_EL0 for an exception taken
 * from EL1t or from a lower EL, and the pre-exception SP_EL1 for one taken from
 * EL1h (a nested exception, e.g. a breakpoint inside an interrupt handler).
 */

.extern gdb_stub_main
.extern irq_handler_c

/* \from_sp0 = 1: the interrupted program was using SP_EL0. */
.macro save_regs from_sp0
    sub sp, sp, #288
    stp x0,  x1,  [sp, #0]
    stp x2,  x3,  [sp, #16]
    stp x4,  x5,  [sp, #32]
    stp x6,  x7,  [sp, #48]
    stp x8,  x9,  [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30,      [sp, #240]
.if \from_sp0
    mrs x0, sp_el0
.else
    add x0, sp, #288
.endif
    str x0, [sp, #248]
    mrs x0, elr_el1
    str x0, [sp, #256]
    mrs x0, spsr_el1
    str x0, [sp, #264]
.endm

.macro restore_regs from_sp0
    /* Exception return state first (x0 is still free as a scratch register). */
    ldr x0, [sp, #264]
    msr spsr_el1, x0
    ldr x0, [sp, #256]
    msr elr_el1, x0
.if \from_sp0
    /* GDB may have rewritten the program's sp ('set $sp', 'return'). */
    ldr x0, [sp, #248]
    msr sp_el0, x0
.endif
    ldp x2,  x3,  [sp, #16]
    ldp x4,  x5,  [sp, #32]
    ldp x6,  x7,  [sp, #48]
    ldp x8,  x9,  [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30,      [sp, #240]
.if \from_sp0
    ldp x0, x1, [sp, #0]
    add sp, sp, #288
.else
    /* Switch to the saved sp slot last (GDB may have changed it). x1 keeps the
     * frame address while x0/x1 are reloaded. */
    mov x1, sp
    ldr x0, [x1, #248]
    mov sp, x0
    ldr x0, [x1, #0]
    ldr x1, [x1, #8]
.endif
.endm

/* One handler: save, call C with x0 = frame and w1 = vector type, restore, eret. */
.macro exc_entry name, from_sp0, vec, handler
\name:
    save_regs \from_sp0
    mov x0, sp
    mov w1, #\vec
    bl \handler
    restore_regs \from_sp0
    eret
.endm

.macro vec_slot target
    .balign 0x80
    b \target
.endm

.balign 0x800
.global vectors
vectors:
    /* Current EL with SP0 (the program at EL1t) */
    vec_slot sync_sp0
    vec_slot irq_sp0
    vec_slot fiq_sp0
    vec_slot serr_sp0
    /* Current EL with SPx (a nested exception on the exception stack) */
    vec_slot sync_spx
    vec_slot irq_spx
    vec_slot fiq_spx
    vec_slot serr_spx
    /* Lower EL using AArch64: the interrupted program used SP_EL0 */
    vec_slot sync_sp0
    vec_slot irq_sp0
    vec_slot fiq_sp0
    vec_slot serr_sp0
    /* Lower EL using AArch32: not supported */
    vec_slot .
    vec_slot .
    vec_slot .
    vec_slot .

/* vec: 0 = synchronous, 1 = IRQ (irq_handler_c ignores it), 2 = FIQ, 3 = SError */
exc_entry sync_sp0, 1, 0, gdb_stub_main
exc_entry irq_sp0,  1, 1, irq_handler_c
exc_entry fiq_sp0,  1, 2, gdb_stub_main
exc_entry serr_sp0, 1, 3, gdb_stub_main
exc_entry sync_spx, 0, 0, gdb_stub_main
exc_entry irq_spx,  0, 1, irq_handler_c
exc_entry fiq_spx,  0, 2, gdb_stub_main
exc_entry serr_spx, 0, 3, gdb_stub_main
