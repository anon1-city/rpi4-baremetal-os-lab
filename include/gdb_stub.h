#ifndef GDB_STUB_H
#define GDB_STUB_H

#include <stdint.h>

typedef struct {
    uint64_t x[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
} gdb_regs_t;

/* The exception entry code (include/exception_frame.h) saves exactly this layout. */
_Static_assert(sizeof(gdb_regs_t) == 272, "gdb_regs_t must match the exception frame");

/* vec: 0 = synchronous, 2 = FIQ, 3 = SError (which vector was taken). */
void gdb_stub_main(gdb_regs_t *regs, unsigned vec);

/* A kernel that handles some exceptions itself (system calls, page faults, ...)
 * registers a hook. The stub calls it first for every exception that is not a
 * debug event (BRK, single step, hardware breakpoint or watchpoint); if it returns
 * non-zero the exception is considered handled and the program resumes at regs->pc
 * (already after an SVC). Otherwise the stub reports it to GDB as a fault. vec is
 * as above, esr is ESR_EL1. Pass 0 to remove the hook. The hook runs on the
 * exception stack with interrupts masked. */
typedef int (*gdb_exception_hook_t)(gdb_regs_t *regs, unsigned vec, uint64_t esr);
void gdb_stub_set_hook(gdb_exception_hook_t hook);

/* Give one received byte back to the stub (used by gdb_poll() when it has to
 * peek at the byte that starts a GDB packet). */
void gdb_stub_pushback(char c);
#endif
