#include <stdint.h>
#include <stddef.h>
#include "gdb_stub.h"
#include "uart.h"
#include "power.h"

static const char hexchars[] = "0123456789abcdef";

#define AARCH64_GDB_BRK_INSN  0xD4200020u   /* brk #1 */
#define PERIPH_BASE           0xFE000000ull /* BCM2711 peripheral window */
#define PERIPH_END            0x100000000ull
#define UART0_DR_ADDR         0xFE201000ull
#define MAX_SW_BREAKPOINTS    32
#define MDSCR_EL1_SS   (1ull << 0)
#define MDSCR_EL1_KDE  (1ull << 13)
#define MDSCR_EL1_MDE  (1ull << 15)
#define PSTATE_SS_BIT  (1u << 21)
#define PSTATE_D_BIT   (1u << 9)

/* ESR_EL1.EC values the stub cares about. */
#define EC_UNKNOWN        0x00
#define EC_FP_TRAP        0x07
#define EC_ILLEGAL_STATE  0x0e
#define EC_SVC64          0x15
#define EC_IABORT_LOW     0x20
#define EC_IABORT_CUR     0x21
#define EC_PC_ALIGN       0x22
#define EC_DABORT_LOW     0x24
#define EC_DABORT_CUR     0x25
#define EC_SP_ALIGN       0x26
#define EC_FP_EXC         0x2c
#define EC_SERROR         0x2f
#define EC_HWBP_LOW       0x30
#define EC_HWBP_CUR       0x31
#define EC_STEP_LOW       0x32
#define EC_STEP_CUR       0x33
#define EC_WATCH_LOW      0x34
#define EC_WATCH_CUR      0x35
#define EC_BRK64          0x3c

/* Which exception vector brought us here (second argument of gdb_stub_main). */
#define VEC_SYNC    0
#define VEC_FIQ     2
#define VEC_SERROR  3

/* Signal numbers used in stop replies (GDB's numbering). */
#define SIG_INT    2
#define SIG_ILL    4
#define SIG_TRAP   5
#define SIG_FPE    8
#define SIG_BUS    10
#define SIG_SEGV   11
#define SIG_SYS    12

/* Packet sizing. GDB_PACKET_SIZE is advertised in qSupported (as hex "800"),
 * so GDB never sends a packet longer than this. MAX_MEM_XFER bytes of memory
 * (2 hex digits each) always fit in one such packet. */
#define GDB_PACKET_SIZE   2048
#define GDB_BUF_SIZE      (GDB_PACKET_SIZE + 16)
#define MAX_MEM_XFER      (GDB_PACKET_SIZE / 2)

/* GDB register numbers: 0-30 = x0-x30, 31 = sp, 32 = pc, 33 = cpsr (32-bit). */
#define REG_FP            29
#define REG_SP            31
#define REG_PC            32
#define REG_CPSR          33
#define REG_V0            34     /* v0-v31 = 34..65 (128-bit), fpsr = 66, fpcr = 67 */
#define REG_FPSR          66
#define REG_FPCR          67
#define G_CORE_BYTES      (31 * 8 + 8 + 8 + 4)   /* x0-x30, sp, pc, cpsr */
#define G_FP_BYTES        (32 * 16 + 4 + 4)      /* v0-v31, fpsr, fpcr */
#define G_PACKET_BYTES    (G_CORE_BYTES + G_FP_BYTES)   /* layout of the 'g'/'G' packet */

/* A debugger may change the condition flags (NZCV) and the interrupt masks
 * (DAIF) of the stopped program. The exception level, stack-pointer select and
 * the single-step bit stay under the stub's control: changing them would make
 * the return from the stub (eret) land in the wrong mode. */
#define PSTATE_WRITABLE_MASK  0xF00003C0u

/* Hardware debug resources (Z1 breakpoints, Z2/Z3/Z4 watchpoints). The real
 * counts are read from ID_AA64DFR0_EL1; these are the architectural maxima. */
#define MAX_HW_BREAKPOINTS  16
#define MAX_HW_WATCHPOINTS  16

/* Why the target is stopped. */
typedef enum {
    STOP_UNKNOWN = 0,
    STOP_MANUAL_BRK,    /* "brk #0" compiled into the program (gdb_break) */
    STOP_GDB_BRK,       /* "brk #1" planted by a Z0 packet */
    STOP_SINGLESTEP,
    STOP_HW_BRK,        /* hardware breakpoint (Z1) */
    STOP_WATCH,         /* hardware watchpoint (Z2/Z3/Z4) */
    STOP_FAULT,         /* an exception the program did not expect */
} stop_reason_t;

typedef enum { RESUME_CONT, RESUME_STEP } resume_t;

typedef struct {
    uint64_t addr;
    uint32_t orig_insn;
    int used;
} sw_bp_t;

typedef struct {
    uint64_t addr;
    int used;
} hw_bp_t;

typedef struct {
    uint64_t addr;
    uint32_t len;       /* 1..8 bytes */
    int type;           /* 2 = write, 3 = read, 4 = access (the Z packet number) */
    int used;
} hw_wp_t;

static stop_reason_t last_stop_reason = STOP_UNKNOWN;
static int stop_sig = SIG_TRAP;
static uint64_t stop_watch_addr;
static int stop_watch_type;         /* 0 = none, else the Z packet number */

/* Exception syndrome captured on entry (see "monitor info"). */
static uint64_t exc_esr, exc_far, exc_elr;
static unsigned exc_vec;

static sw_bp_t sw_bps[MAX_SW_BREAKPOINTS];
static hw_bp_t hw_bps[MAX_HW_BREAKPOINTS];
static hw_wp_t hw_wps[MAX_HW_WATCHPOINTS];

/* Set when GDB moved pc itself (P/G/c ADDR/s ADDR). A manual "brk #0" leaves
 * pc on the brk instruction and the stub normally steps over it on resume;
 * that must not happen to a pc the debugger chose. */
static int pc_written;

/* A hidden single step is in flight: the instruction at pc is being executed
 * with its breakpoint out of the way. When it completes the breakpoint is put
 * back and, unless GDB asked for a step (or a watchpoint fired), the target is
 * resumed without GDB ever noticing. */
static int stepover_active;
static int stepover_sw = -1;        /* sw breakpoint slot temporarily removed */
static int stepover_hw_bp;          /* hardware breakpoints disarmed */
static int stepover_hw_wp;          /* hardware watchpoints disarmed */
static resume_t resume_mode;        /* what GDB asked for */

/* PSTATE.D is cleared while the target runs with stepping / hardware debug
 * active (otherwise its debug exceptions are masked) and put back on entry. */
static int d_cleared;
static uint64_t saved_d_bit;

static int silent_entry;            /* entered by "brk #3": no stop reply, GDB is connecting */
static int stub_active;             /* the stub itself is running */
static volatile int mem_guard;      /* a guarded access is in progress */
static volatile int mem_fault;      /* ... and it faulted */

static int gdb_stub_state_inited = 0;

static gdb_exception_hook_t exception_hook;

void gdb_stub_set_hook(gdb_exception_hook_t hook) { exception_hook = hook; }

/* Debug events belong to the stub; everything else may belong to the kernel. */
static int is_debug_event(unsigned vec, uint64_t esr) {
    unsigned ec = (unsigned)((esr >> 26) & 0x3f);

    if (vec != VEC_SYNC)
        return 0;
    return ec == EC_BRK64 || ec == EC_STEP_LOW || ec == EC_STEP_CUR ||
           ec == EC_HWBP_LOW || ec == EC_HWBP_CUR ||
           ec == EC_WATCH_LOW || ec == EC_WATCH_CUR;
}

static void gdb_stub_init_state(void) {
    last_stop_reason = STOP_UNKNOWN;
    stop_sig = SIG_TRAP;
    stepover_active = 0;
    stepover_sw = -1;
    d_cleared = 0;

    for (int i = 0; i < MAX_SW_BREAKPOINTS; i++) {
        sw_bps[i].addr = 0;
        sw_bps[i].orig_insn = 0;
        sw_bps[i].used = 0;
    }
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++)
        hw_bps[i].used = 0;
    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++)
        hw_wps[i].used = 0;

    gdb_stub_state_inited = 1;
}

static inline void gdb_putc(char c) { uart_putc(c); }

static int rx_pushback = -1;

static inline char gdb_getc(void) {
    if (rx_pushback >= 0) {
        char c = (char)rx_pushback;
        rx_pushback = -1;
        return c;
    }
    return uart_getc();
}

void gdb_stub_pushback(char c) { rx_pushback = (unsigned char)c; }

static int startswith(const char *s, const char *p) {
    while (*p) {
        if (*s++ != *p++) return 0;
    }
    return 1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static uint64_t parse_hex_u64(const char *s, int *used) {
    uint64_t v = 0;
    int i = 0;
    while (1) {
        int h = hexval(s[i]);
        if (h < 0) break;
        v = (v << 4) | (uint64_t)h;
        i++;
    }
    if (used) *used = i;
    return v;
}

static void append_hex_byte(char **p, uint8_t v) {
    *(*p)++ = hexchars[(v >> 4) & 0xf];
    *(*p)++ = hexchars[v & 0xf];
}

static void append_hex_u32_le(char **p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        append_hex_byte(p, (uint8_t)((v >> (i * 8)) & 0xff));
    }
}

static void append_hex_u64_le(char **p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        append_hex_byte(p, (uint8_t)((v >> (i * 8)) & 0xff));
    }
}

/* Make code written at [addr, addr+len) visible to instruction fetch. Cleaning
 * the data cache to the point of unification first keeps this correct once a
 * later lab turns the caches on; with them off it is only a few barriers. */
static void sync_icache(uint64_t addr, uint64_t len) {
    uint64_t ctr, a;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t dline = 4ull << ((ctr >> 16) & 0xf);
    uint64_t iline = 4ull << (ctr & 0xf);

    for (a = addr & ~(dline - 1); a < addr + len; a += dline)
        __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
    for (a = addr & ~(iline - 1); a < addr + len; a += iline)
        __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

/* Map the exception syndrome to a stop reason and a GDB signal number. */
static void classify_exception(uint64_t esr, unsigned vec) {
    unsigned ec  = (unsigned)((esr >> 26) & 0x3f);
    unsigned iss = (unsigned)(esr & 0x1ffffff);
    unsigned dfsc = iss & 0x3f;

    stop_sig = SIG_TRAP;
    stop_watch_type = 0;
    silent_entry = 0;

    if (vec == VEC_SERROR) {
        last_stop_reason = STOP_FAULT;
        stop_sig = SIG_BUS;
        return;
    }
    if (vec == VEC_FIQ) {
        last_stop_reason = STOP_FAULT;
        return;
    }

    switch (ec) {
    case EC_BRK64:
        /* brk #0 = gdb_break(), #1 = planted by GDB (Z0), #2 = gdb_interrupt()
         * (Ctrl-C), #3 = gdb_poll() saw GDB connect. #0/#2/#3 are compiled into
         * the program, so resuming steps past them. */
        switch (iss & 0xffff) {
        case 0:  last_stop_reason = STOP_MANUAL_BRK; break;
        case 1:  last_stop_reason = STOP_GDB_BRK; break;
        case 2:  last_stop_reason = STOP_MANUAL_BRK; stop_sig = SIG_INT; break;
        case 3:  last_stop_reason = STOP_MANUAL_BRK; silent_entry = 1; break;
        default: last_stop_reason = STOP_UNKNOWN; break;   /* some other brk #imm: a trap */
        }
        break;
    case EC_STEP_LOW:
    case EC_STEP_CUR:
        last_stop_reason = STOP_SINGLESTEP;
        break;
    case EC_HWBP_LOW:
    case EC_HWBP_CUR:
        last_stop_reason = STOP_HW_BRK;
        break;
    case EC_WATCH_LOW:
    case EC_WATCH_CUR:
        last_stop_reason = STOP_WATCH;
        break;
    case EC_UNKNOWN:
    case EC_ILLEGAL_STATE:
    case EC_FP_TRAP:
        last_stop_reason = STOP_FAULT;
        stop_sig = SIG_ILL;
        break;
    case EC_FP_EXC:
        last_stop_reason = STOP_FAULT;
        stop_sig = SIG_FPE;
        break;
    case EC_SVC64:
        last_stop_reason = STOP_FAULT;
        stop_sig = SIG_SYS;
        break;
    case EC_PC_ALIGN:
    case EC_SP_ALIGN:
        last_stop_reason = STOP_FAULT;
        stop_sig = SIG_BUS;
        break;
    case EC_IABORT_LOW:
    case EC_IABORT_CUR:
    case EC_DABORT_LOW:
    case EC_DABORT_CUR:
        last_stop_reason = STOP_FAULT;
        /* alignment fault (0x21) and external aborts (0x10-0x17) are bus errors */
        stop_sig = (dfsc == 0x21 || (dfsc >= 0x10 && dfsc <= 0x17)) ? SIG_BUS : SIG_SEGV;
        break;
    case EC_SERROR:
        last_stop_reason = STOP_FAULT;
        stop_sig = SIG_BUS;
        break;
    default:
        last_stop_reason = STOP_FAULT;
        break;
    }
}

/* The last packet sent is kept (already framed) so it can be retransmitted
 * when GDB answers it with '-'. The largest reply is an 'm' of MAX_MEM_XFER
 * bytes. */
static char tx_buf[2 * MAX_MEM_XFER + 16];
static int tx_len;

static void tx_send(void) {
    for (int i = 0; i < tx_len; i++)
        gdb_putc(tx_buf[i]);
}

static void put_packet(const char *buf) {
    uint8_t cksum = 0;
    int n = 0;

    tx_buf[n++] = '$';
    for (int i = 0; buf[i] && n < (int)sizeof(tx_buf) - 4; i++) {
        cksum += (uint8_t)buf[i];
        tx_buf[n++] = buf[i];
    }
    tx_buf[n++] = '#';
    tx_buf[n++] = hexchars[(cksum >> 4) & 0xf];
    tx_buf[n++] = hexchars[cksum & 0xf];
    tx_len = n;
    tx_send();
}

static void put_empty(void) {
    put_packet("");
}

static int bp_find(uint64_t addr);

static void append_hex_num(char **p, uint64_t v) {
    int started = 0;

    for (int i = 60; i >= 0; i -= 4) {
        int n = (int)((v >> i) & 0xf);
        if (n || started || i == 0) {
            *(*p)++ = hexchars[n];
            started = 1;
        }
    }
}

static void append_reg_field(char **p, unsigned regno, uint64_t v) {
    append_hex_byte(p, (uint8_t)regno);
    *(*p)++ = ':';
    append_hex_u64_le(p, v);
    *(*p)++ = ';';
}

static void append_str(char **p, const char *s) {
    while (*s)
        *(*p)++ = *s++;
}

/* "T" stop reply: signal, the registers GDB needs first for a backtrace (pc,
 * sp, fp) so it can skip three 'p' round trips, the thread, and the reason so
 * GDB can tell a breakpoint from a watchpoint from a plain trap. */
static void put_stop_reply(const gdb_regs_t *regs) {
    char buf[200];
    char *p = buf;

    *p++ = 'T';
    append_hex_byte(&p, (uint8_t)stop_sig);
    append_reg_field(&p, REG_PC, regs->pc);
    append_reg_field(&p, REG_SP, regs->sp);
    append_reg_field(&p, REG_FP, regs->x[REG_FP]);
    append_str(&p, "thread:1;");

    if (last_stop_reason == STOP_GDB_BRK && bp_find(regs->pc) >= 0) {
        append_str(&p, "swbreak:;");
    } else if (last_stop_reason == STOP_HW_BRK) {
        append_str(&p, "hwbreak:;");
    } else if (last_stop_reason == STOP_WATCH && stop_watch_type) {
        append_str(&p, stop_watch_type == 2 ? "watch:" :
                       stop_watch_type == 3 ? "rwatch:" : "awatch:");
        append_hex_num(&p, stop_watch_addr);
        *p++ = ';';
    }

    *p = 0;
    put_packet(buf);
}

static int read_csum(uint8_t *dst) {
    char c1 = gdb_getc();
    char c2 = gdb_getc();
    int h1 = hexval(c1);
    int h2 = hexval(c2);

    if (h1 < 0 || h2 < 0)
        return -1;

    *dst = (uint8_t)((h1 << 4) | h2);
    return 0;
}

static void ack_packet(int good) {
    gdb_putc(good ? '+' : '-');
}

/* Receive one packet into buf (NUL-terminated) and store its length in *len
 * (binary 'X' payloads may contain NUL bytes, so callers cannot use strlen).
 * Returns 0 on success, -1 if the packet was NAKed (bad checksum or too long
 * for buf; GDB retransmits). A '-' from GDB retransmits our last reply; a
 * stray Ctrl-C byte while the target is already stopped is ignored (it must
 * not disturb the recorded stop state). */
static int recv_packet(char *buf, int buflen, int *len) {
    int i = 0;
    int overflow = 0;
    uint8_t csum = 0, recv = 0;
    char c;

    for (;;) {
        c = gdb_getc();
        if (c == '$')
            break;

        if (c == '-' && tx_len)
            tx_send();
    }

restart:
    i = 0;
    overflow = 0;
    csum = 0;
    while (1) {
        c = gdb_getc();
        if (c == '$')
            goto restart;   /* GDB gave up on the previous packet and resent */
        if (c == '#')
            break;

        if (i < buflen - 1)
            buf[i++] = c;
        else
            overflow = 1;   /* never act on a truncated packet (e.g. a memory write) */
        csum += (uint8_t)c;
    }
    buf[i] = 0;
    *len = i;

    if (read_csum(&recv) < 0) {
        ack_packet(0);
        return -1;
    }

    if (overflow || recv != csum) {
        ack_packet(0);
        return -1;
    }

    ack_packet(1);
    return 0;
}

/* ---- Guarded target memory access -------------------------------------- */

/* One instruction per access. If it takes a data abort (bad address, or an
 * unaligned access to device memory), the stub's own exception entry sees
 * mem_guard, sets mem_fault and steps over that instruction, so the caller gets
 * an error instead of the stub dying in a nested exception. */
static int guarded_load(uint64_t addr, unsigned width, uint64_t *out) {
    uint64_t v = 0;
    uint32_t t = 0;

    mem_fault = 0;
    mem_guard = 1;
    switch (width) {
    case 8:  __asm__ volatile("ldr %0, [%1]"   : "=r"(v) : "r"(addr) : "memory"); break;
    case 4:  __asm__ volatile("ldr %w0, [%1]"  : "=r"(t) : "r"(addr) : "memory"); v = t; break;
    case 2:  __asm__ volatile("ldrh %w0, [%1]" : "=r"(t) : "r"(addr) : "memory"); v = t; break;
    default: __asm__ volatile("ldrb %w0, [%1]" : "=r"(t) : "r"(addr) : "memory"); v = t; break;
    }
    mem_guard = 0;

    if (mem_fault)
        return -1;
    *out = v;
    return 0;
}

static int guarded_store(uint64_t addr, unsigned width, uint64_t v) {
    mem_fault = 0;
    mem_guard = 1;
    switch (width) {
    case 8:  __asm__ volatile("str %0, [%1]"   :: "r"(v), "r"(addr) : "memory"); break;
    case 4:  __asm__ volatile("str %w0, [%1]"  :: "r"(v), "r"(addr) : "memory"); break;
    case 2:  __asm__ volatile("strh %w0, [%1]" :: "r"(v), "r"(addr) : "memory"); break;
    default: __asm__ volatile("strb %w0, [%1]" :: "r"(v), "r"(addr) : "memory"); break;
    }
    mem_guard = 0;

    return mem_fault ? -1 : 0;
}

/* ---- Software breakpoints (Z0/z0) -------------------------------------- */

static int bp_find(uint64_t addr) {
    for (int i = 0; i < MAX_SW_BREAKPOINTS; i++) {
        if (sw_bps[i].used && sw_bps[i].addr == addr)
            return i;
    }
    return -1;
}

static int bp_alloc_slot(void) {
    for (int i = 0; i < MAX_SW_BREAKPOINTS; i++) {
        if (!sw_bps[i].used)
            return i;
    }
    return -1;
}

/* 0 = inserted (or already there), -1 = misaligned, -2 = no free slot,
 * -3 = the address cannot be accessed. */
static int bp_insert(uint64_t addr) {
    uint64_t old;

    if (addr & 3u)
        return -1;

    if (bp_find(addr) >= 0)
        return 0;

    int slot = bp_alloc_slot();
    if (slot < 0)
        return -2;

    if (guarded_load(addr, 4, &old) < 0)
        return -3;

    sw_bps[slot].addr = addr;
    sw_bps[slot].orig_insn = (uint32_t)old;
    sw_bps[slot].used = 1;

    if (guarded_store(addr, 4, AARCH64_GDB_BRK_INSN) < 0) {
        sw_bps[slot].used = 0;
        return -3;
    }
    sync_icache(addr, 4);
    return 0;
}

static int bp_remove(uint64_t addr) {
    int idx = bp_find(addr);
    if (idx < 0)
        return -1;

    int rc = guarded_store(addr, 4, sw_bps[idx].orig_insn);
    sync_icache(addr, 4);

    sw_bps[idx].used = 0;
    sw_bps[idx].addr = 0;
    sw_bps[idx].orig_insn = 0;
    return rc;
}

/* ---- Target memory access (m / M / X packets) -------------------------- */

/* Use the widest naturally aligned access the address and length allow, so a
 * 4-byte 'set var' on a device register is one 32-bit store, not four bytes. */
static unsigned access_width(uint64_t addr, uint64_t len) {
    uint64_t a = addr | len;
    unsigned w = 1;

    if ((a & 7) == 0) w = 8;
    else if ((a & 3) == 0) w = 4;
    else if ((a & 1) == 0) w = 2;
    /* Peripheral registers are 32 bits wide: never issue a 64-bit access there. */
    if (w > 4 && addr >= PERIPH_BASE && addr < PERIPH_END)
        w = 4;
    return w;
}

/* Reading UART0's data register removes a byte from the receive FIFO, and that
 * FIFO carries GDB's own packets. A debugger read of it (say, "x/x 0xfe201000")
 * would eat the debugger's bytes, so it returns 0 without touching the register. */
static int touches_uart0_dr(uint64_t addr, unsigned width) {
    return addr < UART0_DR_ADDR + 4 && UART0_DR_ADDR < addr + width;
}

/* Does inserted breakpoint i (a 4-byte "brk") overlap [addr, addr+len)? */
static int bp_in_range(int i, uint64_t addr, uint64_t len) {
    return sw_bps[i].used && sw_bps[i].addr < addr + len && addr < sw_bps[i].addr + 4;
}

/* Read up to len bytes of target memory and return how many were read (less
 * than len if an access faulted; GDB accepts a short reply). Inserted software
 * breakpoints are hidden: the caller sees the original instruction, not
 * "brk #1". */
static uint64_t mem_read(uint64_t addr, uint8_t *dst, uint64_t len) {
    unsigned w = access_width(addr, len);
    uint64_t got = 0;

    for (uint64_t j = 0; j < len; j += w) {
        uint64_t v = 0;
        if (!touches_uart0_dr(addr + j, w) && guarded_load(addr + j, w, &v) < 0)
            break;
        for (unsigned k = 0; k < w; k++)
            dst[j + k] = (uint8_t)(v >> (8 * k));
        got = j + w;
    }

    for (int i = 0; i < MAX_SW_BREAKPOINTS; i++) {
        if (!bp_in_range(i, addr, got))
            continue;
        for (unsigned k = 0; k < 4; k++) {
            uint64_t a = sw_bps[i].addr + k;
            if (a >= addr && a < addr + got)
                dst[a - addr] = (uint8_t)(sw_bps[i].orig_insn >> (8 * k));
        }
    }
    return got;
}

/* Write len bytes to target memory; 0 on success, -1 if an access faulted. A
 * write that overlaps an inserted software breakpoint updates the saved
 * original instruction and leaves the "brk" in place, so the breakpoint keeps
 * working and later reads/removal see the new code. Instruction memory is
 * synchronised afterwards. */
static int mem_write(uint64_t addr, const uint8_t *src, uint64_t len) {
    unsigned w = access_width(addr, len);
    int rc = 0;

    for (uint64_t j = 0; j < len; j += w) {
        uint64_t v = 0;
        for (unsigned k = 0; k < w; k++)
            v |= (uint64_t)src[j + k] << (8 * k);
        if (guarded_store(addr + j, w, v) < 0) {
            rc = -1;
            break;
        }
    }

    for (int i = 0; i < MAX_SW_BREAKPOINTS; i++) {
        if (!bp_in_range(i, addr, len))
            continue;
        if (rc == 0) {
            for (unsigned k = 0; k < 4; k++) {
                uint64_t a = sw_bps[i].addr + k;
                if (a >= addr && a < addr + len) {
                    sw_bps[i].orig_insn &= ~(0xFFu << (8 * k));
                    sw_bps[i].orig_insn |= (uint32_t)src[a - addr] << (8 * k);
                }
            }
        }
        guarded_store(sw_bps[i].addr, 4, AARCH64_GDB_BRK_INSN);
    }

    sync_icache(addr, len);
    return rc;
}

/* ---- Hardware breakpoints and watchpoints (Z1-Z4) ----------------------- */

#define DBG_CASE(reg, n) case n: __asm__ volatile("msr " #reg #n "_el1, %0" :: "r"(v)); break;
#define DBG_CASES(reg) \
    DBG_CASE(reg, 0)  DBG_CASE(reg, 1)  DBG_CASE(reg, 2)  DBG_CASE(reg, 3) \
    DBG_CASE(reg, 4)  DBG_CASE(reg, 5)  DBG_CASE(reg, 6)  DBG_CASE(reg, 7) \
    DBG_CASE(reg, 8)  DBG_CASE(reg, 9)  DBG_CASE(reg, 10) DBG_CASE(reg, 11) \
    DBG_CASE(reg, 12) DBG_CASE(reg, 13) DBG_CASE(reg, 14) DBG_CASE(reg, 15)

static void dbgbvr_write(unsigned n, uint64_t v) { switch (n) { DBG_CASES(dbgbvr) default: break; } }
static void dbgbcr_write(unsigned n, uint64_t v) { switch (n) { DBG_CASES(dbgbcr) default: break; } }
static void dbgwvr_write(unsigned n, uint64_t v) { switch (n) { DBG_CASES(dbgwvr) default: break; } }
static void dbgwcr_write(unsigned n, uint64_t v) { switch (n) { DBG_CASES(dbgwcr) default: break; } }

/* DBGBCR/DBGWCR: E (bit 0), PMC/PAC = 0b11 (EL1 and EL0), BAS = byte address
 * select (bits 8:5 for breakpoints: 0xF = the whole 4-byte instruction; bits
 * 12:5 for watchpoints), LSC (watchpoint bits 4:3): 1 = load, 2 = store. */
#define DBGCR_E        1u
#define DBGCR_PRIV     (3u << 1)

static unsigned hw_num_bps(void) {
    uint64_t d;
    __asm__ volatile("mrs %0, id_aa64dfr0_el1" : "=r"(d));
    unsigned n = (unsigned)((d >> 12) & 0xf) + 1;
    return n > MAX_HW_BREAKPOINTS ? MAX_HW_BREAKPOINTS : n;
}

static unsigned hw_num_wps(void) {
    uint64_t d;
    __asm__ volatile("mrs %0, id_aa64dfr0_el1" : "=r"(d));
    unsigned n = (unsigned)((d >> 20) & 0xf) + 1;
    return n > MAX_HW_WATCHPOINTS ? MAX_HW_WATCHPOINTS : n;
}

/* A watchpoint watches bytes inside one aligned 8-byte block, so a region that
 * straddles two blocks needs two debug registers. */
typedef struct {
    uint64_t base;
    unsigned bas;
} wp_chunk_t;

static int wp_chunks(const hw_wp_t *w, wp_chunk_t c[2]) {
    unsigned off = (unsigned)(w->addr & 7);

    c[0].base = w->addr & ~7ull;
    if (off + w->len <= 8) {
        c[0].bas = ((1u << w->len) - 1) << off;
        return 1;
    }
    c[0].bas = (0xffu << off) & 0xffu;
    c[1].base = c[0].base + 8;
    c[1].bas = (1u << (off + w->len - 8)) - 1;
    return 2;
}

static unsigned wp_lsc(int type) {
    return type == 2 ? 2u : type == 3 ? 1u : 3u;
}

static unsigned hw_wp_regs_used(void) {
    unsigned n = 0;
    wp_chunk_t c[2];

    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++) {
        if (hw_wps[i].used)
            n += (unsigned)wp_chunks(&hw_wps[i], c);
    }
    return n;
}

static int hw_bp_count(void) {
    int n = 0;
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++)
        n += hw_bps[i].used;
    return n;
}

static int hw_any_used(void) {
    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++) {
        if (hw_wps[i].used)
            return 1;
    }
    return hw_bp_count() > 0;
}

static int hw_bp_at(uint64_t addr) {
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++) {
        if (hw_bps[i].used && hw_bps[i].addr == addr)
            return 1;
    }
    return 0;
}

/* Program the debug registers from the tables. dis_bp / dis_wp leave that kind
 * disarmed (used while stepping over a breakpoint or a watchpoint hit). */
static void hw_apply(int dis_bp, int dis_wp) {
    unsigned nb = hw_num_bps(), nw = hw_num_wps();
    unsigned bi = 0, wi = 0;
    wp_chunk_t c[2];

    for (int i = 0; i < MAX_HW_BREAKPOINTS && !dis_bp; i++) {
        if (!hw_bps[i].used || bi >= nb)
            continue;
        dbgbcr_write(bi, 0);
        dbgbvr_write(bi, hw_bps[i].addr & ~3ull);
        dbgbcr_write(bi, DBGCR_E | DBGCR_PRIV | (0xfu << 5));
        bi++;
    }
    for (; bi < nb; bi++)
        dbgbcr_write(bi, 0);

    for (int i = 0; i < MAX_HW_WATCHPOINTS && !dis_wp; i++) {
        if (!hw_wps[i].used)
            continue;
        int n = wp_chunks(&hw_wps[i], c);
        for (int k = 0; k < n && wi < nw; k++, wi++) {
            dbgwcr_write(wi, 0);
            dbgwvr_write(wi, c[k].base);
            dbgwcr_write(wi, DBGCR_E | DBGCR_PRIV | (wp_lsc(hw_wps[i].type) << 3) |
                             ((uint64_t)c[k].bas << 5));
        }
    }
    for (; wi < nw; wi++)
        dbgwcr_write(wi, 0);

    __asm__ volatile("isb");
}

/* 0 = ok, -1 = bad request, -2 = out of debug registers. */
static int hw_bp_insert(uint64_t addr) {
    if (addr & 3u)
        return -1;
    if (hw_bp_at(addr))
        return 0;
    if ((unsigned)hw_bp_count() >= hw_num_bps())
        return -2;
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++) {
        if (!hw_bps[i].used) {
            hw_bps[i].addr = addr;
            hw_bps[i].used = 1;
            hw_apply(0, 0);
            return 0;
        }
    }
    return -2;
}

static int hw_bp_remove(uint64_t addr) {
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++) {
        if (hw_bps[i].used && hw_bps[i].addr == addr) {
            hw_bps[i].used = 0;
            hw_apply(0, 0);
            return 0;
        }
    }
    return -1;
}

static int hw_wp_insert(int type, uint64_t addr, uint64_t len) {
    if (len == 0 || len > 8)
        return -1;

    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++) {
        if (hw_wps[i].used && hw_wps[i].addr == addr &&
            hw_wps[i].len == len && hw_wps[i].type == type)
            return 0;
    }
    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++) {
        if (hw_wps[i].used)
            continue;
        hw_wps[i].addr = addr;
        hw_wps[i].len = (uint32_t)len;
        hw_wps[i].type = type;
        hw_wps[i].used = 1;
        if (hw_wp_regs_used() > hw_num_wps()) {
            hw_wps[i].used = 0;
            return -2;
        }
        hw_apply(0, 0);
        return 0;
    }
    return -2;
}

static int hw_wp_remove(int type, uint64_t addr, uint64_t len) {
    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++) {
        if (hw_wps[i].used && hw_wps[i].addr == addr &&
            hw_wps[i].len == len && hw_wps[i].type == type) {
            hw_wps[i].used = 0;
            hw_apply(0, 0);
            return 0;
        }
    }
    return -1;
}

/* Which watchpoint does data address far belong to? FAR is the accessed
 * address; when it falls outside every region (the architecture lets it be any
 * address of a multi-byte access) fall back to the aligned block. */
static int hw_wp_match(uint64_t far) {
    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++) {
        if (hw_wps[i].used && far >= hw_wps[i].addr && far < hw_wps[i].addr + hw_wps[i].len)
            return i;
    }
    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++) {
        if (hw_wps[i].used && (far & ~7ull) >= (hw_wps[i].addr & ~7ull) &&
            (far & ~7ull) <= ((hw_wps[i].addr + hw_wps[i].len - 1) & ~7ull))
            return i;
    }
    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++) {
        if (hw_wps[i].used)
            return i;
    }
    return -1;
}

/* ---- Resuming the target ------------------------------------------------ */

/* A watchpoint fired: the instruction has not completed yet, so it is stepped
 * over with the watchpoints disarmed and the stop is reported afterwards. */
static int watch_pending;
static uint64_t watch_pending_addr;
static int watch_pending_type;

/* Undo what resume_setup did to the saved context. */
static void undo_resume_setup(gdb_regs_t *regs) {
    if (d_cleared) {
        regs->pstate = (regs->pstate & ~(uint64_t)PSTATE_D_BIT) | saved_d_bit;
        d_cleared = 0;
    }
    regs->pstate &= ~(uint64_t)PSTATE_SS_BIT;
}

/* Arm (step) or disarm single-stepping, and unmask debug exceptions when a
 * step or a hardware breakpoint/watchpoint needs them. MDSCR_EL1: SS = software
 * step, MDE = breakpoint/watchpoint events, KDE = debug exceptions at EL1. */
static void resume_setup(gdb_regs_t *regs, int step) {
    int hw = hw_any_used();
    uint64_t mdscr;

    __asm__ volatile("mrs %0, mdscr_el1" : "=r"(mdscr));
    mdscr &= ~(MDSCR_EL1_SS | MDSCR_EL1_KDE | MDSCR_EL1_MDE);
    if (step)
        mdscr |= MDSCR_EL1_SS | MDSCR_EL1_KDE;
    if (hw)
        mdscr |= MDSCR_EL1_MDE | MDSCR_EL1_KDE;
    __asm__ volatile("msr mdscr_el1, %0" :: "r"(mdscr));

    if (step || hw) {
        saved_d_bit = regs->pstate & PSTATE_D_BIT;
        regs->pstate &= ~(uint64_t)PSTATE_D_BIT;
        d_cleared = 1;
    }
    if (step)
        regs->pstate |= PSTATE_SS_BIT;
    else
        regs->pstate &= ~(uint64_t)PSTATE_SS_BIT;

    __asm__ volatile("isb");
}

/* Execute exactly one instruction at pc with whatever breakpoint sits on it
 * out of the way. */
static void begin_hidden_step(gdb_regs_t *regs, int dis_wp) {
    int s = bp_find(regs->pc);

    stepover_sw = -1;
    if (s >= 0) {
        guarded_store(regs->pc, 4, sw_bps[s].orig_insn);
        sync_icache(regs->pc, 4);
        stepover_sw = s;
    }
    stepover_hw_bp = hw_bp_at(regs->pc);
    stepover_hw_wp = dis_wp;
    if (stepover_hw_bp || stepover_hw_wp)
        hw_apply(stepover_hw_bp, stepover_hw_wp);

    stepover_active = 1;
    resume_setup(regs, 1);
}

/* Put back what begin_hidden_step took away. */
static void finish_hidden_step(void) {
    if (!stepover_active)
        return;

    if (stepover_sw >= 0 && sw_bps[stepover_sw].used) {
        guarded_store(sw_bps[stepover_sw].addr, 4, AARCH64_GDB_BRK_INSN);
        sync_icache(sw_bps[stepover_sw].addr, 4);
    }
    if (stepover_hw_bp || stepover_hw_wp)
        hw_apply(0, 0);

    stepover_sw = -1;
    stepover_hw_bp = 0;
    stepover_hw_wp = 0;
    stepover_active = 0;
}

/* Continue or single-step the target. Returns 1 when the target should now run
 * (the caller returns from the stub), 0 when the stub stays in control because
 * the step finished without executing anything. */
static int do_resume(gdb_regs_t *regs, resume_t mode) {
    int stepover = 0;

    resume_mode = mode;

    if (!pc_written && last_stop_reason == STOP_MANUAL_BRK) {
        /* "brk #0" is a no-op as far as the debugger is concerned: move past
         * it. A step is complete right there, nothing has to be executed. */
        regs->pc += 4;
        if (mode == RESUME_STEP) {
            last_stop_reason = STOP_SINGLESTEP;
            stop_sig = SIG_TRAP;
            stop_watch_type = 0;
            pc_written = 0;
            put_stop_reply(regs);
            return 0;
        }
    } else if (!pc_written &&
               ((last_stop_reason == STOP_GDB_BRK && bp_find(regs->pc) >= 0) ||
                (last_stop_reason == STOP_HW_BRK && hw_bp_at(regs->pc)))) {
        stepover = 1;   /* resuming on the breakpoint that just hit */
    }

    if (stepover)
        begin_hidden_step(regs, 0);
    else
        resume_setup(regs, mode == RESUME_STEP);
    return 1;
}

/* ---- FP/SIMD registers ---------------------------------------------------
 * The stub is built with -mgeneral-regs-only, so it never touches v0-v31: at a
 * stop they still hold the program's values and can be read (and written)
 * directly, with no save area in the exception entry. The buffer is 16-byte
 * aligned because, with the MMU off, data accesses are to device memory and
 * must be naturally aligned. */
static uint8_t fp_buf[32 * 16] __attribute__((aligned(16)));

static int fp_enabled(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(v));
    return ((v >> 20) & 3) == 3;    /* CPACR_EL1.FPEN: no FP/SIMD trap at EL1 */
}

#define FP_PAIRS(op) \
    op " q0, q1, [%0, #0]\n\t"    op " q2, q3, [%0, #32]\n\t" \
    op " q4, q5, [%0, #64]\n\t"   op " q6, q7, [%0, #96]\n\t" \
    op " q8, q9, [%0, #128]\n\t"  op " q10, q11, [%0, #160]\n\t" \
    op " q12, q13, [%0, #192]\n\t" op " q14, q15, [%0, #224]\n\t" \
    op " q16, q17, [%0, #256]\n\t" op " q18, q19, [%0, #288]\n\t" \
    op " q20, q21, [%0, #320]\n\t" op " q22, q23, [%0, #352]\n\t" \
    op " q24, q25, [%0, #384]\n\t" op " q26, q27, [%0, #416]\n\t" \
    op " q28, q29, [%0, #448]\n\t" op " q30, q31, [%0, #480]\n\t"

static void fp_load_buf(void) {     /* v0-v31 -> fp_buf (all zero if FP is off) */
    if (!fp_enabled()) {
        for (unsigned i = 0; i < sizeof(fp_buf); i++)
            fp_buf[i] = 0;
        return;
    }
    __asm__ volatile(FP_PAIRS("stp") :: "r"(fp_buf) : "memory");
}

static void fp_store_buf(void) {    /* fp_buf -> v0-v31 */
    /* No clobbers needed: the compiler cannot be using these registers. */
    __asm__ volatile(FP_PAIRS("ldp") :: "r"(fp_buf) : "memory");
}

static uint32_t fp_read_status(int fpcr) {
    uint64_t v = 0;

    if (fp_enabled()) {
        if (fpcr)
            __asm__ volatile("mrs %0, fpcr" : "=r"(v));
        else
            __asm__ volatile("mrs %0, fpsr" : "=r"(v));
    }
    return (uint32_t)v;
}

static void fp_write_status(int fpcr, uint32_t v) {
    uint64_t x = v;

    if (fpcr)
        __asm__ volatile("msr fpcr, %0" :: "r"(x));
    else
        __asm__ volatile("msr fpsr, %0" :: "r"(x));
}

static int read_reg_by_number(const gdb_regs_t *regs, unsigned regno, char *out, size_t outsz) {
    char *p = out;

    if (outsz < 33)
        return -1;

    if (regno <= 30) {
        append_hex_u64_le(&p, regs->x[regno]);
    } else if (regno == REG_SP) {
        append_hex_u64_le(&p, regs->sp);
    } else if (regno == REG_PC) {
        append_hex_u64_le(&p, regs->pc);
    } else if (regno == REG_CPSR) {
        append_hex_u32_le(&p, (uint32_t)regs->pstate);
    } else if (regno >= REG_V0 && regno < REG_V0 + 32) {
        fp_load_buf();
        for (unsigned i = 0; i < 16; i++)
            append_hex_byte(&p, fp_buf[16 * (regno - REG_V0) + i]);
    } else if (regno == REG_FPSR) {
        append_hex_u32_le(&p, fp_read_status(0));
    } else if (regno == REG_FPCR) {
        append_hex_u32_le(&p, fp_read_status(1));
    } else {
        return -1;
    }

    *p = 0;
    return 0;
}

/* ---- Register writes (P / G packets) ----------------------------------- */

/* True if s starts with n hex digits. */
static int hex_digits_ok(const char *s, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        if (hexval(s[i]) < 0)
            return 0;
    }
    return 1;
}

/* Value of nbytes little-endian bytes given as 2*nbytes (validated) hex digits. */
static uint64_t hex_le_value(const char *s, unsigned nbytes) {
    uint64_t v = 0;
    for (unsigned i = 0; i < nbytes; i++)
        v |= (uint64_t)((hexval(s[2 * i]) << 4) | hexval(s[2 * i + 1])) << (8 * i);
    return v;
}

/* Instructions are 4-byte aligned; a misaligned pc would fault on return. */
static int set_pc(gdb_regs_t *regs, uint64_t pc) {
    if (pc & 3u)
        return -1;
    if (pc != regs->pc)
        pc_written = 1;
    regs->pc = pc;
    return 0;
}

static void set_pstate(gdb_regs_t *regs, uint32_t v) {
    regs->pstate = (regs->pstate & ~(uint64_t)PSTATE_WRITABLE_MASK) |
                   (v & PSTATE_WRITABLE_MASK);
}

/* 'P n=value': hex is the register value in target (little-endian) byte order.
 * Returns 0 on success, -1 for an unknown register or a malformed value. */
static int write_reg_by_number(gdb_regs_t *regs, unsigned regno, const char *hex) {
    unsigned nbytes = (regno == REG_CPSR || regno >= REG_FPSR) ? 4 : 8;

    if (regno >= REG_V0 && regno < REG_V0 + 32) {
        if (!fp_enabled() || !hex_digits_ok(hex, 32) || hex[32] != 0)
            return -1;
        fp_load_buf();
        for (unsigned i = 0; i < 16; i++)
            fp_buf[16 * (regno - REG_V0) + i] = (uint8_t)hex_le_value(hex + 2 * i, 1);
        fp_store_buf();
        return 0;
    }

    if (regno > REG_FPCR || !hex_digits_ok(hex, 2 * nbytes) || hex[2 * nbytes] != 0)
        return -1;

    uint64_t v = hex_le_value(hex, nbytes);

    if (regno <= 30) {
        regs->x[regno] = v;
    } else if (regno == REG_SP) {
        regs->sp = v;
    } else if (regno == REG_PC) {
        return set_pc(regs, v);
    } else if (regno == REG_CPSR) {
        set_pstate(regs, (uint32_t)v);
    } else {
        if (!fp_enabled())
            return -1;
        fp_write_status(regno == REG_FPCR, (uint32_t)v);
    }

    return 0;
}

/* 'G values': same layout as the 'g' reply. Nothing is changed unless the
 * whole packet is well formed. */
static int write_all_regs(gdb_regs_t *regs, const char *hex) {
    if (!hex_digits_ok(hex, 2 * G_PACKET_BYTES) || hex[2 * G_PACKET_BYTES] != 0)
        return -1;
    if (hex_le_value(hex + 16 * REG_PC, 8) & 3u)
        return -1;

    for (int i = 0; i < 31; i++)
        regs->x[i] = hex_le_value(hex + 16 * i, 8);
    regs->sp = hex_le_value(hex + 16 * REG_SP, 8);
    set_pc(regs, hex_le_value(hex + 16 * REG_PC, 8));
    set_pstate(regs, (uint32_t)hex_le_value(hex + 16 * REG_CPSR, 4));

    if (fp_enabled()) {
        const char *f = hex + 2 * G_CORE_BYTES;

        for (unsigned i = 0; i < sizeof(fp_buf); i++)
            fp_buf[i] = (uint8_t)hex_le_value(f + 2 * i, 1);
        fp_store_buf();
        fp_write_status(0, (uint32_t)hex_le_value(f + 2 * sizeof(fp_buf), 4));
        fp_write_status(1, (uint32_t)hex_le_value(f + 2 * sizeof(fp_buf) + 8, 4));
    }
    return 0;
}

static const char target_xml[] =
"<?xml version=\"1.0\"?>"
"<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
"<target>"
"<architecture>aarch64</architecture>"
"<feature name=\"org.gnu.gdb.aarch64.core\">"
"<reg name=\"x0\" bitsize=\"64\"/>"
"<reg name=\"x1\" bitsize=\"64\"/>"
"<reg name=\"x2\" bitsize=\"64\"/>"
"<reg name=\"x3\" bitsize=\"64\"/>"
"<reg name=\"x4\" bitsize=\"64\"/>"
"<reg name=\"x5\" bitsize=\"64\"/>"
"<reg name=\"x6\" bitsize=\"64\"/>"
"<reg name=\"x7\" bitsize=\"64\"/>"
"<reg name=\"x8\" bitsize=\"64\"/>"
"<reg name=\"x9\" bitsize=\"64\"/>"
"<reg name=\"x10\" bitsize=\"64\"/>"
"<reg name=\"x11\" bitsize=\"64\"/>"
"<reg name=\"x12\" bitsize=\"64\"/>"
"<reg name=\"x13\" bitsize=\"64\"/>"
"<reg name=\"x14\" bitsize=\"64\"/>"
"<reg name=\"x15\" bitsize=\"64\"/>"
"<reg name=\"x16\" bitsize=\"64\"/>"
"<reg name=\"x17\" bitsize=\"64\"/>"
"<reg name=\"x18\" bitsize=\"64\"/>"
"<reg name=\"x19\" bitsize=\"64\"/>"
"<reg name=\"x20\" bitsize=\"64\"/>"
"<reg name=\"x21\" bitsize=\"64\"/>"
"<reg name=\"x22\" bitsize=\"64\"/>"
"<reg name=\"x23\" bitsize=\"64\"/>"
"<reg name=\"x24\" bitsize=\"64\"/>"
"<reg name=\"x25\" bitsize=\"64\"/>"
"<reg name=\"x26\" bitsize=\"64\"/>"
"<reg name=\"x27\" bitsize=\"64\"/>"
"<reg name=\"x28\" bitsize=\"64\"/>"
"<reg name=\"x29\" bitsize=\"64\"/>"
"<reg name=\"x30\" bitsize=\"64\"/>"
"<reg name=\"sp\" bitsize=\"64\" type=\"data_ptr\"/>"
"<reg name=\"pc\" bitsize=\"64\" type=\"code_ptr\"/>"
"<reg name=\"cpsr\" bitsize=\"32\"/>"
"</feature>"
"<feature name=\"org.gnu.gdb.aarch64.fpu\">"
"<vector id=\"v2d\" type=\"ieee_double\" count=\"2\"/>"
"<vector id=\"v2u\" type=\"uint64\" count=\"2\"/>"
"<vector id=\"v2i\" type=\"int64\" count=\"2\"/>"
"<vector id=\"v4f\" type=\"ieee_single\" count=\"4\"/>"
"<vector id=\"v4u\" type=\"uint32\" count=\"4\"/>"
"<vector id=\"v4i\" type=\"int32\" count=\"4\"/>"
"<vector id=\"v8u\" type=\"uint16\" count=\"8\"/>"
"<vector id=\"v8i\" type=\"int16\" count=\"8\"/>"
"<vector id=\"v16u\" type=\"uint8\" count=\"16\"/>"
"<vector id=\"v16i\" type=\"int8\" count=\"16\"/>"
"<vector id=\"v1u\" type=\"uint128\" count=\"1\"/>"
"<vector id=\"v1i\" type=\"int128\" count=\"1\"/>"
"<union id=\"vnd\"><field name=\"f\" type=\"v2d\"/><field name=\"u\" type=\"v2u\"/><field name=\"s\" type=\"v2i\"/></union>"
"<union id=\"vns\"><field name=\"f\" type=\"v4f\"/><field name=\"u\" type=\"v4u\"/><field name=\"s\" type=\"v4i\"/></union>"
"<union id=\"vnh\"><field name=\"u\" type=\"v8u\"/><field name=\"s\" type=\"v8i\"/></union>"
"<union id=\"vnb\"><field name=\"u\" type=\"v16u\"/><field name=\"s\" type=\"v16i\"/></union>"
"<union id=\"vnq\"><field name=\"u\" type=\"v1u\"/><field name=\"s\" type=\"v1i\"/></union>"
"<union id=\"aarch64v\"><field name=\"d\" type=\"vnd\"/><field name=\"s\" type=\"vns\"/><field name=\"h\" type=\"vnh\"/><field name=\"b\" type=\"vnb\"/><field name=\"q\" type=\"vnq\"/></union>"
"<reg name=\"v0\" bitsize=\"128\" type=\"aarch64v\" regnum=\"34\"/>"
"<reg name=\"v1\" bitsize=\"128\" type=\"aarch64v\" regnum=\"35\"/>"
"<reg name=\"v2\" bitsize=\"128\" type=\"aarch64v\" regnum=\"36\"/>"
"<reg name=\"v3\" bitsize=\"128\" type=\"aarch64v\" regnum=\"37\"/>"
"<reg name=\"v4\" bitsize=\"128\" type=\"aarch64v\" regnum=\"38\"/>"
"<reg name=\"v5\" bitsize=\"128\" type=\"aarch64v\" regnum=\"39\"/>"
"<reg name=\"v6\" bitsize=\"128\" type=\"aarch64v\" regnum=\"40\"/>"
"<reg name=\"v7\" bitsize=\"128\" type=\"aarch64v\" regnum=\"41\"/>"
"<reg name=\"v8\" bitsize=\"128\" type=\"aarch64v\" regnum=\"42\"/>"
"<reg name=\"v9\" bitsize=\"128\" type=\"aarch64v\" regnum=\"43\"/>"
"<reg name=\"v10\" bitsize=\"128\" type=\"aarch64v\" regnum=\"44\"/>"
"<reg name=\"v11\" bitsize=\"128\" type=\"aarch64v\" regnum=\"45\"/>"
"<reg name=\"v12\" bitsize=\"128\" type=\"aarch64v\" regnum=\"46\"/>"
"<reg name=\"v13\" bitsize=\"128\" type=\"aarch64v\" regnum=\"47\"/>"
"<reg name=\"v14\" bitsize=\"128\" type=\"aarch64v\" regnum=\"48\"/>"
"<reg name=\"v15\" bitsize=\"128\" type=\"aarch64v\" regnum=\"49\"/>"
"<reg name=\"v16\" bitsize=\"128\" type=\"aarch64v\" regnum=\"50\"/>"
"<reg name=\"v17\" bitsize=\"128\" type=\"aarch64v\" regnum=\"51\"/>"
"<reg name=\"v18\" bitsize=\"128\" type=\"aarch64v\" regnum=\"52\"/>"
"<reg name=\"v19\" bitsize=\"128\" type=\"aarch64v\" regnum=\"53\"/>"
"<reg name=\"v20\" bitsize=\"128\" type=\"aarch64v\" regnum=\"54\"/>"
"<reg name=\"v21\" bitsize=\"128\" type=\"aarch64v\" regnum=\"55\"/>"
"<reg name=\"v22\" bitsize=\"128\" type=\"aarch64v\" regnum=\"56\"/>"
"<reg name=\"v23\" bitsize=\"128\" type=\"aarch64v\" regnum=\"57\"/>"
"<reg name=\"v24\" bitsize=\"128\" type=\"aarch64v\" regnum=\"58\"/>"
"<reg name=\"v25\" bitsize=\"128\" type=\"aarch64v\" regnum=\"59\"/>"
"<reg name=\"v26\" bitsize=\"128\" type=\"aarch64v\" regnum=\"60\"/>"
"<reg name=\"v27\" bitsize=\"128\" type=\"aarch64v\" regnum=\"61\"/>"
"<reg name=\"v28\" bitsize=\"128\" type=\"aarch64v\" regnum=\"62\"/>"
"<reg name=\"v29\" bitsize=\"128\" type=\"aarch64v\" regnum=\"63\"/>"
"<reg name=\"v30\" bitsize=\"128\" type=\"aarch64v\" regnum=\"64\"/>"
"<reg name=\"v31\" bitsize=\"128\" type=\"aarch64v\" regnum=\"65\"/>"
"<reg name=\"fpsr\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
"<reg name=\"fpcr\" bitsize=\"32\" type=\"int\" group=\"float\"/>"
"</feature>"
"</target>";

static void reply_qxfer_features_read(const char *cmd) {
    static const char prefix[] = "qXfer:features:read:";
    const char *p = cmd + sizeof(prefix) - 1;
    const char *annex = p;
    const char *colon = 0;

    while (*p) {
        if (*p == ':') {
            colon = p;
            break;
        }
        p++;
    }

    if (!colon) {
        put_empty();
        return;
    }

    {
        const char *s = "target.xml";
        const char *q = annex;
        int ok = 1;

        while (q < colon && *s) {
            if (*q++ != *s++) {
                ok = 0;
                break;
            }
        }

        if (!(ok && q == colon && *s == 0)) {
            put_empty();
            return;
        }
    }

    int used = 0;
    uint64_t off = parse_hex_u64(colon + 1, &used);
    if (used <= 0 || *(colon + 1 + used) != ',') {
        put_empty();
        return;
    }

    uint64_t len = parse_hex_u64(colon + 1 + used + 1, &used);
    if (len == 0) {
        put_packet("l");
        return;
    }

    uint64_t xml_len = 0;
    while (target_xml[xml_len]) xml_len++;

    if (off >= xml_len) {
        put_packet("l");
        return;
    }

    if (len > 1024) len = 1024;
    if (off + len > xml_len) len = xml_len - off;

    static char out[1040];
    char *o = out;
    *o++ = ((off + len) < xml_len) ? 'm' : 'l';

    for (uint64_t i = 0; i < len; i++)
        *o++ = target_xml[off + i];

    *o = 0;
    put_packet(out);
}

/* 'm addr,len' */
static void reply_mem_read(const char *cmd) {
    int used = 0;
    uint64_t addr = parse_hex_u64(cmd + 1, &used);
    const char *p = cmd + 1 + used;

    if (used <= 0 || *p != ',') {
        put_empty();
        return;
    }

    uint64_t len = parse_hex_u64(p + 1, &used);
    if (used <= 0 || len > MAX_MEM_XFER) {
        put_packet("E01");
        return;
    }

    /* Static, not on the stack: the stub runs on the small exception stack. */
    static uint8_t data[MAX_MEM_XFER];
    static char out[2 * MAX_MEM_XFER + 1];
    char *o = out;

    /* A partially readable range gives a short reply; nothing readable is an
     * error (E14 = EFAULT). */
    uint64_t got = mem_read(addr, data, len);
    if (got == 0 && len != 0) {
        put_packet("E14");
        return;
    }
    for (uint64_t j = 0; j < got; j++)
        append_hex_byte(&o, data[j]);
    *o = 0;
    put_packet(out);
}

/* 'M addr,len:hexbytes' (binary == 0) or 'X addr,len:binary' (binary == 1).
 * In the binary form '}' escapes the next byte (which is XORed with 0x20).
 * 'X addr,0:' is how GDB probes for binary-write support; it must answer OK. */
static void reply_mem_write(const char *cmd, int cmd_len, int binary) {
    int used = 0;
    uint64_t addr = parse_hex_u64(cmd + 1, &used);
    const char *p = cmd + 1 + used;

    if (used <= 0 || *p != ',') {
        put_packet("E01");
        return;
    }

    uint64_t len = parse_hex_u64(p + 1, &used);
    p += 1 + used;
    if (used <= 0 || *p != ':' || len > MAX_MEM_XFER) {
        put_packet("E01");
        return;
    }
    p++;

    static uint8_t data[MAX_MEM_XFER];

    if (binary) {
        const char *end = cmd + cmd_len;

        for (uint64_t n = 0; n < len; n++) {
            if (p >= end) {
                put_packet("E01");
                return;
            }
            uint8_t c = (uint8_t)*p++;
            if (c == '}') {
                if (p >= end) {
                    put_packet("E01");
                    return;
                }
                c = (uint8_t)*p++ ^ 0x20;
            }
            data[n] = c;
        }
        if (p != end) {
            put_packet("E01");
            return;
        }
    } else {
        if (!hex_digits_ok(p, 2 * (unsigned)len) || p[2 * len] != 0) {
            put_packet("E01");
            return;
        }
        for (uint64_t n = 0; n < len; n++)
            data[n] = (uint8_t)hex_le_value(p + 2 * n, 1);
    }

    if (mem_write(addr, data, len) < 0)
        put_packet("E14");
    else
        put_packet("OK");
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* Instructions are removed/cleared here, not left behind in the target, when
 * GDB detaches or kills the session. */
static void remove_all_breakpoints(void) {
    for (int i = 0; i < MAX_SW_BREAKPOINTS; i++) {
        if (sw_bps[i].used)
            bp_remove(sw_bps[i].addr);
    }
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++)
        hw_bps[i].used = 0;
    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++)
        hw_wps[i].used = 0;
    hw_apply(0, 0);
}

/* Optional "ADDR" argument of 'c' / 's': resume at another address. */
static void set_resume_addr(gdb_regs_t *regs, const char *arg) {
    int used = 0;

    if (!arg[0])
        return;
    uint64_t addr = parse_hex_u64(arg, &used);
    if (used > 0 && set_pc(regs, addr) == 0)
        pc_written = 1;   /* explicit resume address: never skip a brk */
}

/* "vCont;c", "vCont;C05", "vCont;s:1", ... Only the first action matters (one
 * thread); a signal argument is accepted and ignored. */
static int parse_vcont(const char *cmd, resume_t *mode) {
    const char *p = cmd + 5;

    if (*p++ != ';')
        return -1;
    switch (*p) {
    case 'c':
    case 'C':
        *mode = RESUME_CONT;
        return 0;
    case 's':
    case 'S':
        *mode = RESUME_STEP;
        return 0;
    default:
        return -1;
    }
}

/* 'Z<type>,addr,kind' / 'z<type>,addr,kind'.
 *   0 software breakpoint   1 hardware breakpoint   (kind = 4, an A64 insn)
 *   2 write watchpoint  3 read watchpoint  4 access watchpoint (kind = length) */
static void reply_breakpoint(const char *cmd, int insert) {
    int type = cmd[1] - '0';
    int used = 0;

    if (type < 0 || type > 4 || cmd[2] != ',') {
        put_empty();
        return;
    }

    uint64_t addr = parse_hex_u64(cmd + 3, &used);
    const char *p = cmd + 3 + used;
    if (used <= 0 || *p != ',') {
        put_packet("E01");
        return;
    }

    uint64_t kind = parse_hex_u64(p + 1, &used);
    if (used <= 0) {
        put_packet("E01");
        return;
    }

    int rc;
    if (type == 0) {
        if (kind != 4) {
            put_packet("E11");
            return;
        }
        rc = insert ? bp_insert(addr) : bp_remove(addr);
    } else if (type == 1) {
        if (kind != 4) {
            put_packet("E11");
            return;
        }
        rc = insert ? hw_bp_insert(addr) : hw_bp_remove(addr);
    } else {
        rc = insert ? hw_wp_insert(type, addr, kind) : hw_wp_remove(type, addr, kind);
    }

    if (rc == 0)
        put_packet("OK");
    else if (!insert)
        put_packet("E02");
    else if (rc == -1)
        put_packet("E11");
    else if (rc == -2)
        put_packet("E12");
    else
        put_packet("E14");
}

/* ---- "monitor" commands (qRcmd) ------------------------------------------ */

/* Send text to the GDB console as O packets. */
static void mon_puts(const char *s) {
    char pkt[2 * 100 + 2];

    while (*s) {
        char *p = pkt;
        *p++ = 'O';
        for (int n = 0; *s && n < 100; n++)
            append_hex_byte(&p, (uint8_t)*s++);
        *p = 0;
        put_packet(pkt);
    }
}

static void mon_hex(const char *label, uint64_t v) {
    char line[64];
    char *p = line;

    append_str(&p, label);
    append_str(&p, "0x");
    append_hex_num(&p, v);
    *p++ = '\n';
    *p = 0;
    mon_puts(line);
}

static const char *stop_name(void) {
    switch (last_stop_reason) {
    case STOP_MANUAL_BRK: return "manual brk #0";
    case STOP_GDB_BRK:    return "software breakpoint";
    case STOP_SINGLESTEP: return "single step";
    case STOP_HW_BRK:     return "hardware breakpoint";
    case STOP_WATCH:      return "watchpoint";
    case STOP_FAULT:      return "unexpected exception";
    default:              return "trap";
    }
}

static const char *ec_name(unsigned ec) {
    switch (ec) {
    case EC_UNKNOWN:       return "unknown reason / undefined instruction";
    case EC_FP_TRAP:       return "FP/SIMD access trap";
    case EC_ILLEGAL_STATE: return "illegal execution state";
    case EC_SVC64:         return "SVC";
    case EC_IABORT_LOW:
    case EC_IABORT_CUR:    return "instruction abort";
    case EC_PC_ALIGN:      return "PC alignment fault";
    case EC_DABORT_LOW:
    case EC_DABORT_CUR:    return "data abort";
    case EC_SP_ALIGN:      return "SP alignment fault";
    case EC_FP_EXC:        return "FP exception";
    case EC_SERROR:        return "SError";
    case EC_HWBP_LOW:
    case EC_HWBP_CUR:      return "hardware breakpoint";
    case EC_STEP_LOW:
    case EC_STEP_CUR:      return "software step";
    case EC_WATCH_LOW:
    case EC_WATCH_CUR:     return "watchpoint";
    case EC_BRK64:         return "BRK instruction";
    default:               return "other";
    }
}

static void mon_info(const gdb_regs_t *regs) {
    uint64_t v;

    mon_puts("stop reason: ");
    mon_puts(stop_name());
    mon_puts("\n");
    mon_hex("signal:      ", (uint64_t)stop_sig);
    mon_hex("ESR_EL1:     ", exc_esr);
    mon_puts("  class: ");
    mon_puts(ec_name((unsigned)((exc_esr >> 26) & 0x3f)));
    mon_puts("\n");
    mon_hex("FAR_EL1:     ", exc_far);
    mon_hex("ELR_EL1:     ", exc_elr);
    mon_hex("SPSR (cpsr): ", regs->pstate);
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(v));
    mon_hex("CurrentEL:   ", v >> 2);
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(v));
    mon_hex("SCTLR_EL1:   ", v);
    __asm__ volatile("mrs %0, mdscr_el1" : "=r"(v));
    mon_hex("MDSCR_EL1:   ", v);
    __asm__ volatile("mrs %0, vbar_el1" : "=r"(v));
    mon_hex("VBAR_EL1:    ", v);
}

static void mon_bp(void) {
    int any = 0;

    for (int i = 0; i < MAX_SW_BREAKPOINTS; i++) {
        if (sw_bps[i].used) {
            mon_hex("sw breakpoint @ ", sw_bps[i].addr);
            any = 1;
        }
    }
    for (int i = 0; i < MAX_HW_BREAKPOINTS; i++) {
        if (hw_bps[i].used) {
            mon_hex("hw breakpoint @ ", hw_bps[i].addr);
            any = 1;
        }
    }
    for (int i = 0; i < MAX_HW_WATCHPOINTS; i++) {
        if (!hw_wps[i].used)
            continue;
        mon_puts(hw_wps[i].type == 2 ? "watch  " :
                 hw_wps[i].type == 3 ? "rwatch " : "awatch ");
        mon_hex("@ ", hw_wps[i].addr);
        mon_hex("  length ", hw_wps[i].len);
        any = 1;
    }
    if (!any)
        mon_puts("no breakpoints or watchpoints set\n");
    mon_hex("hw breakpoint registers: ", hw_num_bps());
    mon_hex("hw watchpoint registers: ", hw_num_wps());
}

static void reply_monitor(gdb_regs_t *regs, const char *hex) {
    char cmd[64];
    int n = 0;

    while (hex[0] && hex[1] && n < (int)sizeof(cmd) - 1) {
        if (hexval(hex[0]) < 0 || hexval(hex[1]) < 0) {
            put_packet("E01");
            return;
        }
        cmd[n++] = (char)hex_le_value(hex, 1);
        hex += 2;
    }
    cmd[n] = 0;

    if (streq(cmd, "info")) {
        mon_info(regs);
    } else if (streq(cmd, "bp")) {
        mon_bp();
    } else if (streq(cmd, "reset")) {
        mon_puts("resetting the target: the bootloader is back in a few seconds\n");
        put_packet("OK");
        system_reset();
    } else if (streq(cmd, "help") || streq(cmd, "")) {
        mon_puts("monitor commands:\n"
                 "  info   stop reason, exception syndrome (ESR/FAR/ELR), key system registers\n"
                 "  bp     list software/hardware breakpoints and watchpoints\n"
                 "  reset  reset the target through the SoC watchdog (back to the bootloader)\n"
                 "  help   this text\n");
    } else {
        mon_puts("unknown monitor command (try: monitor help)\n");
    }
    put_packet("OK");
}

/* The packet loop. Returns when GDB resumes the target. */
static void stub_session(gdb_regs_t *regs) {
    static char cmd_buf[GDB_BUF_SIZE];   /* static: see reply_mem_read */
    int cmd_len;

    for (;;) {
        if (recv_packet(cmd_buf, sizeof(cmd_buf), &cmd_len) < 0)
            continue;

        if (cmd_buf[0] == '?') {
            put_stop_reply(regs);
        }
        else if (startswith(cmd_buf, "qSupported")) {
            put_packet("PacketSize=800;swbreak+;hwbreak+;qXfer:features:read+");
        }
        else if (startswith(cmd_buf, "qXfer:features:read:")) {
            reply_qxfer_features_read(cmd_buf);
        }
        else if (startswith(cmd_buf, "qRcmd,")) {
            reply_monitor(regs, cmd_buf + 6);
        }
        else if (startswith(cmd_buf, "vMustReplyEmpty")) {
            put_empty();
        }
        else if (streq(cmd_buf, "vCont?")) {
            put_packet("vCont;c;C;s;S");
        }
        else if (startswith(cmd_buf, "vCont;")) {
            resume_t mode;
            if (parse_vcont(cmd_buf, &mode) < 0)
                put_empty();
            else if (do_resume(regs, mode))
                return;
        }
        else if (streq(cmd_buf, "qAttached")) {
            put_packet("1");
        }
        else if (startswith(cmd_buf, "qOffsets")) {
            put_packet("Text=0;Data=0;Bss=0");
        }
        else if (streq(cmd_buf, "qfThreadInfo")) {
            put_packet("m1");
        }
        else if (streq(cmd_buf, "qsThreadInfo")) {
            put_packet("l");
        }
        else if (streq(cmd_buf, "qC")) {
            put_packet("QC1");
        }
        else if (cmd_buf[0] == 'H') {
            put_packet("OK");
        }
        else if (cmd_buf[0] == 'T') {
            put_packet("OK");
        }
        else if (cmd_buf[0] == 'g') {
            static char reg_hex[2 * G_PACKET_BYTES + 1];
            char *p = reg_hex;

            for (int i = 0; i < 31; i++)
                append_hex_u64_le(&p, regs->x[i]);

            append_hex_u64_le(&p, regs->sp);
            append_hex_u64_le(&p, regs->pc);
            append_hex_u32_le(&p, (uint32_t)regs->pstate);

            fp_load_buf();
            for (unsigned i = 0; i < sizeof(fp_buf); i++)
                append_hex_byte(&p, fp_buf[i]);
            append_hex_u32_le(&p, fp_read_status(0));
            append_hex_u32_le(&p, fp_read_status(1));

            *p = 0;
            put_packet(reg_hex);
        }
        else if (cmd_buf[0] == 'p') {
            int used = 0;
            unsigned regno = (unsigned)parse_hex_u64(cmd_buf + 1, &used);
            char out[40];
            if (used > 0 && read_reg_by_number(regs, regno, out, sizeof(out)) == 0)
                put_packet(out);
            else
                put_packet("E01");
        }
        else if (cmd_buf[0] == 'P') {
            int used = 0;
            unsigned regno = (unsigned)parse_hex_u64(cmd_buf + 1, &used);
            if (used > 0 && cmd_buf[1 + used] == '=' &&
                write_reg_by_number(regs, regno, cmd_buf + 2 + used) == 0)
                put_packet("OK");
            else
                put_packet("E01");
        }
        else if (cmd_buf[0] == 'G') {
            if (write_all_regs(regs, cmd_buf + 1) == 0)
                put_packet("OK");
            else
                put_packet("E01");
        }
        else if (cmd_buf[0] == 'm') {
            reply_mem_read(cmd_buf);
        }
        else if (cmd_buf[0] == 'M') {
            reply_mem_write(cmd_buf, cmd_len, 0);
        }
        else if (cmd_buf[0] == 'X') {
            reply_mem_write(cmd_buf, cmd_len, 1);
        }
        else if (cmd_buf[0] == 'Z') {
            reply_breakpoint(cmd_buf, 1);
        }
        else if (cmd_buf[0] == 'z') {
            reply_breakpoint(cmd_buf, 0);
        }
        else if (cmd_buf[0] == 's' || cmd_buf[0] == 'c') {
            set_resume_addr(regs, cmd_buf + 1);
            if (do_resume(regs, cmd_buf[0] == 's' ? RESUME_STEP : RESUME_CONT))
                return;
        }
        else if (cmd_buf[0] == 'D') {
            /* Detach: leave nothing planted in the program, then let it run. */
            remove_all_breakpoints();
            put_packet("OK");
            if (do_resume(regs, RESUME_CONT))
                return;
        }
        else if (cmd_buf[0] == 'k') {
            /* Kill: GDB expects no reply. The target stays stopped, ready for a
             * new connection, but without the old session's breakpoints. */
            remove_all_breakpoints();
        }
        else {
            put_empty();
        }
    }
}

/* Entered from the exception vectors with the interrupted context in *regs.
 * vec says which vector (VEC_SYNC / VEC_FIQ / VEC_SERROR). */
void gdb_stub_main(gdb_regs_t *regs, unsigned vec) {
    uint64_t esr, far;

    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(far));

    if (stub_active) {
        /* The stub itself faulted. A guarded memory access is expected to; any
         * other fault is a stub bug, and skipping the instruction beats
         * re-faulting forever. Either way the interrupted stub code resumes. */
        if (vec == VEC_SYNC) {
            if (mem_guard)
                mem_fault = 1;
            regs->pc += 4;
        }
        /* An asynchronous SError or FIQ has no faulting instruction: ELR is
         * already the next one, so skipping 4 bytes would lose an innocent
         * instruction. Just return. */
        return;
    }
    if (exception_hook && !is_debug_event(vec, esr) && exception_hook(regs, vec, esr))
        return;     /* the kernel handled it (a system call, a page fault, ...) */

    stub_active = 1;

    if (!gdb_stub_state_inited)
        gdb_stub_init_state();

    exc_esr = esr;
    exc_far = far;
    exc_elr = regs->pc;
    exc_vec = vec;
    pc_written = 0;

    undo_resume_setup(regs);
    classify_exception(esr, vec);

    int hidden = stepover_active;
    int wp_done = watch_pending;
    finish_hidden_step();
    watch_pending = 0;

    if (last_stop_reason == STOP_WATCH) {
        /* The access has not happened yet: execute it with the watchpoints
         * out of the way and report the stop when that step completes. */
        int i = hw_wp_match(far);
        if (i >= 0) {
            watch_pending = 1;
            watch_pending_type = hw_wps[i].type;
            watch_pending_addr = (far >= hw_wps[i].addr && far < hw_wps[i].addr + hw_wps[i].len)
                                     ? far : hw_wps[i].addr;
            begin_hidden_step(regs, 1);
            stub_active = 0;
            return;
        }
        last_stop_reason = STOP_UNKNOWN;   /* no watchpoint set: plain trap */
    } else if (last_stop_reason == STOP_SINGLESTEP && wp_done) {
        last_stop_reason = STOP_WATCH;
        stop_watch_type = watch_pending_type;
        stop_watch_addr = watch_pending_addr;
    } else if (last_stop_reason == STOP_SINGLESTEP && hidden && resume_mode == RESUME_CONT) {
        /* Only stepping over a breakpoint on the way to a continue. */
        resume_setup(regs, 0);
        stub_active = 0;
        return;
    }

    if (!silent_entry)
        put_stop_reply(regs);
    silent_entry = 0;
    stub_session(regs);
    stub_active = 0;
}
