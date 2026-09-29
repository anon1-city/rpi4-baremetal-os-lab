#include <stdint.h>
#include "uart.h"
#include "gdb.h"
#include "gdb_stub.h"

void gdb_break(void) {
  __asm__ volatile ("brk #0");
}

/* brk #2: stop for GDB and report SIGINT (what Ctrl-C means to GDB). */
void gdb_interrupt(void) {
  __asm__ volatile ("brk #2");
}

void gdb_poll(void) {
  if (!uart_rx_ready())
    return;

  char c = uart_getc();
  if ((unsigned char)c == 0x03) {
    gdb_interrupt();
  } else if (c == '$') {
    /* GDB is connecting to a running kernel: stop silently (GDB asks for the
     * stop reason itself) and hand the byte back so the packet is read whole. */
    gdb_stub_pushback(c);
    __asm__ volatile ("brk #3");
  }
  /* anything else (stray '+' acks) is dropped */
}

void gdb_wait_forever(void) {
  uart_puts("[gdb] waiting in debug loop, use GDB to inspect then continue...\n");
  for (;;) {
    gdb_poll();
  }
}

void gdb_demo_point(unsigned tag) {
  uart_puts("[gdb] demo point tag=0x");
  uart_hex32(tag);
  uart_puts("\n");

  volatile unsigned a = tag + 1;
  volatile unsigned b = a * 3;
  volatile unsigned c = b ^ 0xA5A5A5A5u;
  (void)c;

  gdb_break();
}
