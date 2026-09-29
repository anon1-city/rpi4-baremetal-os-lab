# Raspberry Pi 4 UART bootloader with a GDB stub

A small bare-metal environment for teaching OS development on a Raspberry Pi 4.
A Raspberry Pi 5 (or any Linux machine) is the **host**; the Pi 4 is the
**target**. Students never rewrite the SD card to try a change:

1. **Bootloader** (`kernel8.bin`, on the SD card): initialises UART0, prints the
   board revision and VideoCore base address from the mailbox, then waits for a
   kernel image over UART0, verifies its CRC-32 and jumps to it.
2. **Demo kernel** (`kernel_payload.bin`, sent over the serial line): a tiny
   command shell on a second UART, with a built-in **GDB stub**.
3. **GDB stub** (`src/gdb_stub.c`): lets a normal GDB on the host debug the
   kernel over UART0: breakpoints, watchpoints, stepping, registers, memory,
   and crash reports.

Everything also runs on **QEMU** (no Pi needed) with the same two serial
channels; see [Simulating the board on QEMU](#simulating-the-board-on-qemu).

## Serial channels

| Channel | Pi 4 pins | Host device | Carries |
|---|---|---|---|
| UART0 (PL011) | GPIO14/15 | `/dev/ttyUSB0` | bootloader upload **and** the GDB protocol |
| UART2 (PL011) | GPIO0/1 | `/dev/ttyUSB1` | the kernel's console (its display and keyboard) |

Both run at 115200 baud, 8N1. Keeping GDB and the console on different UARTs means
console output and debugger traffic never mix, and a *crash* in console code (a bad
register address, say) is caught by the stub and reported to GDB. It does not make the
debugger independent of the kernel: the stub runs inside it, so a console driver that
*hangs* in a loop is not reachable unless the loop calls `gdb_poll()` (see
[Limits](#limits)).

## Repository layout

| Path | What it is |
|---|---|
| `src/start.S`, `src/main.c`, `src/mailbox.c` | bootloader (linked at `0x80000`) |
| `src/loaded_start.S`, `src/loaded_kernel.c`, `src/gdb.c` | demo kernel: shell, `gdb_break()`, `gdb_poll()` (linked at `0x200000`) |
| `include/exception_frame.h` | the vector table and exception entry code, shared by both start files |
| `src/gdb_stub.c`, `include/gdb_stub.h` | the GDB stub, linked into both images |
| `src/power.c` | `system_reset()`: SoC reset through the watchdog (`reboot`, `monitor reset`) |
| `src/gic.c`, `src/timer.c` | interrupt-controller and timer helpers for the later interrupt lab. **Not built by default**, and not covered by the tests |
| `src/uart.c` | UART0 / UART2 drivers (`-DQEMU_CONSOLE` moves the console to QEMU's mini UART) |
| `boot/` | SD-card files: `config.txt`, `fixup4.dat`, device tree |
| `scripts/uart_send_pi4.py` | uploads an image to the bootloader |
| `scripts/run-qemu.sh` | runs everything on QEMU |
| `tests/` | automated tests for the stub (run on QEMU) |
| `.gdbinit` | GDB setup for the real board |

## Build

```bash
make                 # kernel8.bin (bootloader) + kernel_payload.bin / .elf (demo kernel)
make qemu            # the same, with the console on QEMU's mini UART, in build-qemu/
make test            # run the stub tests on QEMU
make clean
```

The toolchain prefix is `CROSS`: empty (the native compiler) on an AArch64 machine such
as the Pi 5 host, `aarch64-linux-gnu-` elsewhere. Override with `make CROSS=prefix-`.
The build tracks header dependencies (`-MMD`); objects go to `build/` and `build-qemu/`.

Host requirements: `python3-serial` (pyserial) for the upload script, `gdb-multiarch`,
and for the QEMU workflow **QEMU 9.0 or newer** (the `raspi4b` machine appeared in 9.0;
Debian/Raspberry Pi OS releases that ship an older QEMU need a newer build).

`kernel_payload.elf` carries the symbols GDB needs. The code is built with
`-mgeneral-regs-only`, so the stub never touches the FP/SIMD registers.

## Run on the Pi 4

### Step 0: put the bootloader on the SD card

```bash
# SD card mounted at /media/pi/BOOT/
cp kernel8.bin /media/pi/BOOT/
```

`boot/config.txt` enables UART, sets `kernel=kernel8.bin`, and locks the core
clock so the UART baud rate stays stable. The card also needs the usual Pi 4
firmware files (`boot/` has `fixup4.dat` and the device tree).

### Step 1: power on the Pi 4

### Step 2: send the kernel over UART0

```
$ python3 scripts/uart_send_pi4.py kernel_payload.bin
--- kernel_payload.bin: 42044 bytes, crc32 0x9B2F1F9C, via /dev/ttyUSB0 ---
[pi4] RAW image detected, loading at 0x00200000
[pi4] OK 0000A43C 00200000
[OK] the bootloader accepted the image: OK 0000A43C 00200000
```

(Size and CRC depend on your build. The upload takes about 3.7 s at 115200 baud.)
Uploading `kernel_payload.elf` instead works too and is safer: the ELF carries its
own link address, and the bootloader refuses one that would land outside
`0x200000-0x1000000` (for example a kernel linked with the bootloader's `linker.ld`).
The script exits 0 on success, 1 if the bootloader refuses the image or does not
answer, 2 if the image or the serial port cannot be used; it retries twice by default.

**After every change you need the bootloader back.** It runs only until it jumps to
your kernel, so to upload the next image, restart the target: type `reboot` at the
`loaded>` prompt (or `monitor reset` in GDB), or power-cycle it. `reboot` resets the
SoC through the watchdog. It is tested on QEMU only; see [What is tested where](#what-is-tested-where).

### Step 3: open the console on UART2

```
$ screen /dev/ttyUSB1 115200
*** LOADED KERNEL: command shell ready ***
Console: UART2
GDB: UART0
Type help for commands.
loaded>
```

| Command | What it does |
|---|---|
| `help` | list the commands |
| `ping` | prints `pong` |
| `info` | prints where the kernel is loaded |
| `gdbbreak` | executes `brk #0`: the CPU stops in the GDB stub |
| `gdbwait` | same, with a "Kernel paused. Attach GDB now..." message |
| `reboot` | resets the board (watchdog); the bootloader is back and waits for a new image |

### Step 4: stop in the debugger

```
loaded> gdbbreak
[gdb] BRK now
```

### Step 5: attach GDB

From this directory, `.gdbinit` loads the symbols, sets the serial speed (`set serial
baud 115200`; GDB does not do this on its own), connects to `/dev/ttyUSB0` and sets a
breakpoint on `process_cmd`. GDB starts stopped in `gdb_break()`; continue, then type
a command at the `loaded>` prompt:

```
$ gdb
gdb_break () at src/gdb.c:7
7	  __asm__ volatile ("brk #0");
(gdb) continue
Continuing.
```

GDB stops when the command runs (example output; addresses vary with the build):

```
Breakpoint 1, process_cmd (cmd=0x20eb78 "ping") at src/loaded_kernel.c:38
38	  if (cmd[0] == 0) {
(gdb) bt
#0  process_cmd (cmd=0x20eb78 "ping") at src/loaded_kernel.c:38
#1  0x00000000002016c0 in loaded_main () at src/loaded_kernel.c:108
#2  0x0000000000200050 in _start () at src/loaded_start.S:50
```

Use `gdb-multiarch` on an x86 host (plain `gdb` there has no AArch64 support).
If GDB reports that auto-loading `.gdbinit` was declined, start it with
`gdb -x .gdbinit`.

If the kernel is *running* rather than stopped (for example sitting at the
`loaded>` prompt), you do not need `gdbbreak`: connecting GDB breaks in, and
Ctrl-C stops it later (see [gdb_poll](#stopping-a-running-kernel-gdb_poll)).

## The GDB stub

`src/gdb_stub.c` speaks the GDB remote serial protocol. It runs whenever the
CPU takes an exception (`brk`, single step, hardware breakpoint/watchpoint, or
a fault), saves the registers, talks to GDB over UART0, and returns to the
program when GDB resumes it.

| Feature | Notes |
|---|---|
| Registers | `x0-x30`, `sp`, `pc`, `cpsr`, and FP/SIMD `v0-v31`, `fpsr`, `fpcr`; read and write |
| Memory | read and write (`m`, `M`, `X`). A bad address gives `E14` instead of crashing the stub; a partly readable range gives a short reply; planted breakpoints are hidden from reads. Device registers are read 32 bits at a time, and UART0's data register reads as 0 (reading it would remove a byte of GDB's own traffic from the receive FIFO) |
| Software breakpoints | `break` (`Z0`), up to 32; continuing from one steps over it silently |
| Hardware breakpoints | `hbreak` (`Z1`), up to the CPU's limit (6 on the Cortex-A72) |
| Hardware watchpoints | `watch`, `rwatch`, `awatch` (`Z2`/`Z3`/`Z4`), regions up to 8 bytes, 4 registers on the Cortex-A72. The stop is reported after the access completes |
| Stepping | `step`, `stepi`, `next`, `continue` (`s`, `c`, `vCont`) |
| Crashes | an undefined instruction, data or instruction abort, alignment fault, SError or FIQ stops in GDB as `SIGILL`/`SIGSEGV`/`SIGBUS` instead of hanging; `monitor info` shows ESR/FAR/ELR |
| Break-in | Ctrl-C stops a running kernel, and `target remote` can attach to one (see below) |
| `monitor` | `monitor info` (stop reason, exception syndrome, key system registers), `monitor bp` (list breakpoints and watchpoints), `monitor reset` (reset the board through the watchdog), `monitor help` |
| Kernel exceptions | a kernel registers `gdb_stub_set_hook()` to handle system calls, page faults, ... itself; anything the hook does not handle is reported to GDB (see below) |
| Own stack | the stub runs on a dedicated exception stack, so a stop needs none of the program's stack, and a fault caused by a corrupted `sp` is still reported |
| Detach / kill | `detach` and `kill` remove every breakpoint from the target first |
| Robustness | a `-` from GDB retransmits the last reply; a `$` inside a packet restarts it; the instruction cache is synchronised (`dc cvau`, `ic ivau`) after code is written |

Checked against a real GDB on QEMU: `break`, `hbreak`, `watch`, `continue`, `stepi`,
`next`, `finish`, `bt`, `info registers`, `p $v3`, `x/3i $pc`, `set var`,
`monitor info`, and Ctrl-C (`Program received signal SIGINT`).

### Stopping a running kernel: `gdb_poll()`

The stub only runs when the CPU takes an exception, so a kernel that never
executes `brk` cannot be interrupted by itself. Call `gdb_poll()` from any loop
that runs regularly (the shell's input wait in `loaded_kernel.c` already does):

```c
#include "gdb.h"

for (;;) {
    do_work();
    gdb_poll();      /* Ctrl-C from GDB, or a connecting GDB, stops here */
}
```

`gdb_poll()` only reads UART0 and needs no interrupt setup, so it does not
conflict with interrupt handlers written in later labs.

### Using the stub in your own kernel

- Link `src/gdb_stub.c` and `src/power.c`, and a UART driver providing `uart_putc`,
  `uart_getc` and `uart_rx_ready`.
- Take the vector table and entry code from `include/exception_frame.h` (as
  `src/loaded_start.S` does): it saves the registers into a `gdb_regs_t`, calls
  `gdb_stub_main(regs, vec)`, restores them and `eret`s.
- Stacks: the program runs at EL1t on `SP_EL0`; exceptions are taken on `SP_EL1`, which
  `_start` points at `_exc_stack_top` (8 KB in the linker scripts). Interrupt handlers
  run there too. If you write your own start code, do the same (`msr spsel, #1`, set
  `sp`, `msr spsel, #0`, set `sp`), and clear `.bss`, which holds the stub's state.
- The kernel must run at EL1 with `CPACR_EL1` allowing FP/SIMD (both start files
  do this) and with the OS lock released (`osdlr_el1`, `oslar_el1`), which the
  debug registers need.
- Call `gdb_break()` (`brk #0`) or `gdb_poll()` to enter the stub.
- **System calls, page faults and other exceptions of your own:** register a hook,
  `gdb_stub_set_hook(fn)`. For every exception that is not a debug event (`brk`, single
  step, hardware breakpoint or watchpoint) the stub calls
  `int fn(gdb_regs_t *regs, unsigned vec, uint64_t esr)` first; return non-zero to
  handle it (the program resumes at `regs->pc`, already after an `svc`), zero to let
  the stub report it to GDB as a fault. So you can debug your exception code with the
  debugger instead of losing it when you install your own handlers.

### How the stub uses `brk`

| Instruction | Meaning | Resumes |
|---|---|---|
| `brk #0` | `gdb_break()`: a stop compiled into the program | after the `brk` |
| `brk #1` | a software breakpoint planted by GDB | on the original instruction |
| `brk #2` | `gdb_interrupt()`: Ctrl-C, reported as `SIGINT` | after the `brk` |
| `brk #3` | `gdb_poll()` saw GDB connect; no stop reply is sent | after the `brk` |

### Limits

- **A hung program cannot be interrupted.** The stub runs only when the CPU takes an
  exception. A crash or fault anywhere, console driver included, is caught and reported;
  an *infinite loop* that never calls `gdb_poll()` and never executes `brk` is not, and
  neither Ctrl-C nor `target remote` can reach it. Put `gdb_poll()` in every loop that can
  wait on hardware. Then the only way out is a power cycle. (The stub cannot be made
  independent of the program without an interrupt: the design option, a UART0 receive
  interrupt delivered as FIQ, is not implemented and would need testing on the board.)
- An asynchronous SError caused by a debugger memory access is delivered after the
  program resumes and is reported against the program. Not testable on QEMU.
- One core, one thread (`thread 1`).
- Hardware watchpoints cover at most 8 bytes each; larger regions fall back to
  GDB's (slow) software watchpoints.
- The demo kernel is about 42 KB (built at `-O0` for debugging), so an upload takes about
  3.7 s at 115200 baud, and every edit-run cycle adds the reset and the firmware boot.

## Simulating the board on QEMU

QEMU's `raspi4b` has no PL011 UART2, but it has a second serial port, the mini
UART. `make qemu` builds the same code with `-DQEMU_CONSOLE` (into
`build-qemu/`), which puts the console on the mini UART, so QEMU reproduces both
channels:

| Board | QEMU |
|---|---|
| UART0 (`/dev/ttyUSB0`): upload + GDB | TCP port 5555 (`PORT=...` to change) |
| UART2 (`/dev/ttyUSB1`): console | the terminal running `run-qemu.sh boot` |

The steps are the same as on the board:

```bash
./scripts/run-qemu.sh boot      # terminal 1: the bootloader; the console appears here (Ctrl-A x quits)
./scripts/run-qemu.sh upload    # terminal 2: uart_send_pi4.py over the UART0 socket
# terminal 1 now shows "loaded>"
./scripts/run-qemu.sh attach    # terminal 2: gdb-multiarch on kernel_payload.elf, target remote
```

In GDB, `break process_cmd`, `continue`, then type a command at the console in
terminal 1: the breakpoint hits. `attach` needs no `gdbbreak` because the shell
polls (`gdb_poll()`), but typing `gdbbreak` works too.

`scripts/uart_send_pi4.py` also takes `--port socket://localhost:5555` (or the
`UART_PORT` environment variable); without it, it uses `/dev/ttyUSB0`.
`run-qemu.sh` uses `gdb-multiarch` (or another GDB that knows AArch64) and skips
`.gdbinit`, which points at the real board.

### Test kernels

For experiments without the demo kernel, two small kernels from
`tests/kernels/` run with UART0 on the TCP port:

```bash
./scripts/run-qemu.sh debug          # QEMU + gdb-multiarch; a kernel that loops over a few functions
./scripts/run-qemu.sh debug crash    # a kernel that faults on purpose
./scripts/run-qemu.sh stub test      # QEMU only; attach GDB yourself
```

### Other modes

`./scripts/run-qemu.sh help` lists everything. `stdio`, `pty` and `gdb` run the
bootloader with UART0 on the terminal, on a pseudo-terminal, or with QEMU's own
gdbstub (`-S -s`, for debugging the bootloader itself). `make run` and
`make run-pty` are shortcuts for the first two. Set `CROSS` if your toolchain
prefix differs.

## Testing the stub

```bash
make test                              # everything, about 1.5 minutes
python3 tests/run_tests.py -k watch    # only tests whose name contains "watch"
```

`tests/run_tests.py` builds two small test kernels together with the repo's
`start.S`, `uart.c`, `gdb.c` and `gdb_stub.c`, boots each on QEMU with UART0 on a
TCP socket, and plays the part of GDB with a small protocol client
(`tests/rsp.py`). The groups are:

| Group | Covers |
|---|---|
| `test_registers_and_memory` | registers (including FP), memory access, bad addresses, retransmission, stray Ctrl-C |
| `test_breakpoints` | software and hardware breakpoints, stepping over them, `kill` |
| `test_watchpoints` | write/read/access watchpoints, stop after the access, watch during a step |
| `test_crash_reporting` | data abort and undefined instruction |
| `test_interrupt_and_attach` | Ctrl-C, detach, connecting to a running kernel |
| `test_real_gdb` | a real `gdb-multiarch` session (skipped if none is installed) |
| `test_payload_two_channels` | the full flow on both QEMU channels: upload, the shell on the console, `gdbbreak`, a breakpoint hit from a console command, Ctrl-C |
| `test_bootloader_robustness` | stray bytes, truncated / wrong-CRC / oversize uploads, ELFs that would overwrite the bootloader or wrap their offsets, a valid ELF, `reboot` and a second upload |
| `test_debugger_safety` | a stop on a 4 KB stack leaves it untouched, a fault with a corrupted `sp` is reported, the kernel's exception hook, `x/` on UART0's data register |
| `test_reset` | `monitor reset` restarts the target |

Needs `qemu-system-aarch64` (with `raspi4b`), the cross compiler and Python 3;
prints `SKIP` if QEMU or the compiler is missing.

## How the bootloader works

- The firmware loads `kernel8.bin` at `0x80000`; `start.S` drops from EL2 to EL1
  if needed and installs the vectors.
- **Upload protocol** (host to Pi, little-endian): a 12-byte header (magic
  `0x4C424B52` "RKBL", size, CRC-32 of the image, the value Python's `zlib.crc32`
  gives) followed by the image. The bootloader hunts for the magic byte by byte, so
  stray bytes (a glitch on the line at power-up, the tail of an aborted upload) are
  skipped instead of putting every later header out of step. Once a header has
  started, 1 s of silence abandons the transfer (`ERR timeout ...`) and it goes back
  to hunting. It buffers the image at `0x01000000`, checks the size (at most 16 MiB)
  and the CRC, then copies a raw binary (at most 14 MiB) to `0x00200000` or loads the
  `PT_LOAD` segments of an ELF64 AArch64 file at their physical addresses.
- ELF files are checked completely before anything is copied: fixed-address
  executable, every segment inside `0x200000-0x1000000` (so never over the
  bootloader, the receive buffer or device registers), sizes that cannot wrap, and an
  entry point inside a loaded segment. Otherwise `ERR ELF: <reason>`.
- It answers `OK <size> <entry>` followed by an `A` byte, then jumps to the entry.
  Errors are reported as `ERR ...` (with an `N` byte for a rejected image).
- The demo kernel's `_start` sets its own stack, clears `.bss`, installs its own
  vector table and starts the shell.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| GDB: `unknown architecture "aarch64"` | you are using an x86-only `gdb`; use `gdb-multiarch` |
| GDB hangs in `target remote` | the kernel is running and never calls `gdb_poll()` or `brk`; type `gdbbreak` at the console (or add `gdb_poll()` to a loop) |
| GDB: auto-loading `.gdbinit` declined | run `gdb -x .gdbinit`, or allow the path in `~/.config/gdb/gdbinit` |
| QEMU prints garbage / all cores run the code | give QEMU the raw `.bin`, not the `.elf` |
| QEMU: `Invalid SMP CPUs` | `raspi4b` needs `-smp 4` |
| `uart_send_pi4.py`: "no OK/ERR within 5 s" | the bootloader is not running (your kernel is: reset the target with `reboot` / `monitor reset` or power-cycle it), or the wiring / device name is wrong (it prints a banner on UART0) |
| `ERR ELF: segment outside the load area` | the kernel was linked with the bootloader's `linker.ld`; use `linker_kernel.ld` (load address `0x200000`) |
| `ERR checksum mismatch` / `ERR timeout ...` | the transfer was corrupted or stalled; just run the script again (it retries by itself) |
| GDB prints garbage / `Ignoring packet error` | the serial speed: the `.gdbinit` sets `set serial baud 115200`; if you connect by hand, do the same |
| GDB or the upload talks to the console (nothing works) | `ttyUSB0`/`ttyUSB1` swapped: use stable names, see below |
| Ctrl-C / `target remote` has no effect | the program is stuck in a loop that never calls `gdb_poll()`; see the limits above |

## Stable device names

With two identical USB-serial adapters, `ttyUSB0` and `ttyUSB1` are handed out in
plug-in order and can swap after a re-plug or reboot. Then GDB and the upload script talk
to the console UART and nothing works. Use the name that depends on the USB port instead:

```bash
ls -l /dev/serial/by-path/      # one entry per adapter, named after the USB port it is in
python3 scripts/uart_send_pi4.py --port /dev/serial/by-path/<the UART0 adapter> kernel_payload.bin
```

and put the same path in `.gdbinit` (`target remote ...`). Adapters with distinct serial
numbers can also be pinned with a udev rule (`/dev/serial/by-id/`).

## What is tested where

| | QEMU (`make test`) | Real Pi 4 |
|---|---|---|
| Upload protocol, ELF loading, resync, timeouts | tested | tested |
| Stub: registers, memory, breakpoints, watchpoints, stepping, crashes, Ctrl-C | tested | **Manual tested**: Cortex-A72 debug registers and stepping behave differently from QEMU's model in details |
| Console on the second channel | tested with the **mini UART** (`-DQEMU_CONSOLE`) | the board's console is **PL011 UART2** (GPIO0/1, ALT4): a different driver, manually test |
| `reboot` / `monitor reset` (watchdog) | tested | tested |
| Asynchronous SError from a debugger access | cannot be produced | tested |

Before relying on the board, run this once on it and note the result: upload the
payload, open the console on UART2, `gdbbreak`, attach GDB, then `break` /
`continue` / `step`, `watch` a variable, `x/4x` a mapped and an unmapped address,
Ctrl-C at the prompt, `monitor reset`, and upload again.


The QEMU simulation is tested on x86/aarch64 environment. 
