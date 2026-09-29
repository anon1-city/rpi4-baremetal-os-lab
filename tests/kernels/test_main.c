/* Target program for the stub tests: stops at a "brk #0" once, then loops over
 * a few small functions so the tests have breakpoints and a variable to watch. */
#include <stdint.h>
#include "uart.h"
#include "gdb.h"

volatile uint64_t g_var = 0;
volatile uint32_t g_small = 0;

__attribute__((noinline)) void target_fn(void) { g_var += 1; }
__attribute__((noinline)) void write_var(void) { g_var = 0x1234; }
__attribute__((noinline)) uint64_t read_var(void) { return g_var; }

void main(void) {
    uart_init();
    uart_puts("TEST READY\n");
    __asm__ volatile("brk #0");
    for (;;) {
        target_fn();
        write_var();
        (void)read_var();
        g_small++;
        gdb_poll();     /* lets the tests interrupt the running loop */
    }
}

void irq_handler_c(void) {}
