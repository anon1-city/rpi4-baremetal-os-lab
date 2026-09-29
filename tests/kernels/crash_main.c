/* Target program that crashes on purpose, one fault per resume. */
#include <stdint.h>
#include "uart.h"

void main(void) {
    uart_init();
    uart_puts("CRASH READY\n");
    __asm__ volatile("brk #0");
    /* data abort: the address is outside the physical address space */
    *(volatile uint64_t *)0x1000000000000ull = 1;
    /* undefined instruction */
    __asm__ volatile(".word 0x00000000");
    for (;;) { }
}

void irq_handler_c(void) {}
