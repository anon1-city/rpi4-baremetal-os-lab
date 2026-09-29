/* A kernel that handles system calls itself through gdb_stub_set_hook(): "svc #0"
 * is the kernel's, so it must not stop in the debugger; "svc #7" is nobody's, so it
 * must. */
#include <stdint.h>
#include "uart.h"
#include "gdb.h"
#include "gdb_stub.h"

volatile uint64_t svc_count = 0;
volatile uint64_t progress = 0;

static int hook(gdb_regs_t *regs, unsigned vec, uint64_t esr) {
    (void)regs;
    if (vec == 0 && ((esr >> 26) & 0x3f) == 0x15 && (esr & 0xffff) == 0) {
        svc_count++;
        return 1;
    }
    return 0;
}

void main(void) {
    uart_init();
    gdb_stub_set_hook(hook);
    uart_puts("HOOK READY\n");
    __asm__ volatile("brk #0");
    __asm__ volatile("svc #0");
    __asm__ volatile("svc #0");
    progress = 1;
    __asm__ volatile("svc #7");
    progress = 2;
    for (;;)
        gdb_poll();
}

void irq_handler_c(void) {}
