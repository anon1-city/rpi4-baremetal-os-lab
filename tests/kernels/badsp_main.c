/* A fault caused by a corrupted stack pointer: the stub runs on its own exception
 * stack, so it can still report it. */
#include <stdint.h>
#include "uart.h"
#include "gdb.h"

void main(void) {
    uart_init();
    uart_puts("BADSP READY\n");
    __asm__ volatile("brk #0");
    uint64_t bad = 1ull << 48;      /* outside the physical address space */
    __asm__ volatile("mov x9, sp\n\t"
                     "mov sp, %0\n\t"
                     "ldr x1, [sp]\n\t"
                     "mov sp, x9" :: "r"(bad) : "x1", "x9", "memory");
    for (;;)
        gdb_poll();
}

void irq_handler_c(void) {}
