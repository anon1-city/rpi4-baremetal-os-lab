#include <stdint.h>
#include "uart.h"
#include "gdb.h"
#include "gdb_stub.h"
#include "power.h"

#define CMD_BUF_SZ 64

static int streq(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b) return 0;
    a++; b++;
  }
  return (*a == 0 && *b == 0);
}

/* The console is UART2 on the board (the mini UART in the QEMU build). */
void cons_puts(const char *s) { uart2_puts(s); }
void cons_putc(char c) { uart2_putc(c); }
char cons_getc(void) {
  /* Keep the debugger reachable (Ctrl-C / target remote) while idle. */
  while (!uart2_rx_ready())
    gdb_poll();
  return uart2_getc();
}

void print_help(void) {
  cons_puts("Commands:\n");
  cons_puts("  help       - show this help\n");
  cons_puts("  ping       - reply with pong\n");
  cons_puts("  info       - show loaded kernel info\n");
  cons_puts("  reboot     - reset the board; the bootloader is back, upload a new image\n");
  cons_puts("  gdbbreak   - trigger BRK instruction for debugger break\n");
  cons_puts("  gdbwait    - same, with a \"Kernel paused\" message (attach + inspect)\n");
}

void process_cmd(const char *cmd) {
  if (cmd[0] == 0) {
    return;
  } else if (streq(cmd, "help")) {
    print_help();
  } else if (streq(cmd, "ping")) {
    cons_puts("pong\n");
  } else if (streq(cmd, "info")) {
    cons_puts("loaded kernel @ 0x00200000, cmd buffer=64 bytes\n");
  } else if (streq(cmd, "reboot")) {
    cons_puts("Resetting the board...\n");
    system_reset();
  } else if (streq(cmd, "gdbbreak")) {
    cons_puts("[gdb] BRK now\n");
    gdb_break();
    cons_puts("[gdb] resumed after break\n");
  } else if (streq(cmd, "gdbwait")) {
    cons_puts("Kernel paused. Attach GDB now...\n");
    gdb_break();
    cons_puts("Debugger resumed the kernel.\n");
  } else {
    cons_puts("unknown command: ");
    cons_puts(cmd);
    cons_puts("\n");
  }
}

void loaded_main(void) {
  /* Init both UARTs:
     UART0 = GDB
     UART2 = console
  */
  uart_init();
  uart2_init();   // console only

  uart2_puts("*** LOADED KERNEL: command shell ready ***\n");
  uart2_puts("Console: UART2\n");
  uart2_puts("GDB: UART0\n");
  uart2_puts("Type help for commands.\n");

  char buf[CMD_BUF_SZ];
  unsigned n = 0;
  
  for (;;) {
    cons_puts("loaded> ");
    n = 0;
    for (;;) {
      char c = cons_getc();

      if (c == 13 || c == 10) {
        cons_puts("\n");
        break;
      }

      if (c == 0x08 || c == 0x7f) {
        if (n > 0) {
          n--;
          cons_puts("\b \b");
        }
        continue;
      }

      if (c >= 32 && c <= 126) {
        if (n < CMD_BUF_SZ - 1) {
          buf[n++] = c;
          cons_putc(c);
        }
      }
    }

    buf[n] = 0;
    process_cmd(buf);
  }
}

void irq_handler_c(void) {
  // The demo kernel does not use interrupts; later labs replace this handler.
}
