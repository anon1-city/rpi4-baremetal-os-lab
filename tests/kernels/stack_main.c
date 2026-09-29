/* Runs on a 4 KB stack with guard bytes on both sides and stops in the debugger:
 * the stub must not touch the program's stack or anything next to it. */
#include <stdint.h>
#include "uart.h"

struct {
    uint8_t below[4096];
    uint8_t stack[4096];
    uint8_t above[16];
} mem __attribute__((aligned(16)));

void main(void) {
    uart_init();
    for (int i = 0; i < 4096; i++) {
        mem.below[i] = 0xAA;
        mem.stack[i] = 0xBB;
    }
    for (int i = 0; i < 16; i++)
        mem.above[i] = 0xCC;
    uart_puts("STACK READY\n");
    uint64_t top = (uint64_t)&mem.stack[4096];
    __asm__ volatile("mov sp, %0\n\tbrk #0" :: "r"(top));
    for (;;) { }
}

void irq_handler_c(void) {}
