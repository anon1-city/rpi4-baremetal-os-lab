#ifndef GDB_H
#define GDB_H

void gdb_break(void);
void gdb_wait_forever(void);
void gdb_demo_point(unsigned tag);

/* Call this regularly from long-running or idle loops (the console wait, a
 * scheduler idle loop, ...). It looks at the GDB UART without blocking: Ctrl-C
 * from GDB stops the program ("interrupt"), and so does the first byte of a
 * GDB connection attempt, so `target remote` works on a running kernel. */
void gdb_poll(void);
void gdb_interrupt(void);

#endif
