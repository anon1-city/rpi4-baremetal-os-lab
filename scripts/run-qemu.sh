#!/usr/bin/env bash
# Run the bootloader / test kernels on QEMU's Raspberry Pi 4 model, optionally
# with GDB attached to the *kernel's own* GDB stub (src/gdb_stub.c) over UART0.
set -euo pipefail
cd "$(dirname "$0")/.."

MODE="${1:-stdio}"
PORT="${PORT:-5555}"                    # TCP port that carries UART0 in stub/debug mode
QEMU=(qemu-system-aarch64 -cpu cortex-a72 -M raspi4b -m 2G -smp 4 -nographic -monitor none)

usage() {
  cat <<USAGE
Usage: $0 MODE [IMAGE]

Plain QEMU (runs kernel8.bin, or \$KERNEL):
  stdio          UART0 on this terminal (default)
  pty            UART0 on a pseudo-terminal (/dev/pts/N is printed)
  gdb            QEMU's own gdbstub (-S -s): debug the bootloader itself on :1234

The real workflow on QEMU (two channels, like the board: UART0 = upload + GDB
over TCP port \$PORT, console = QEMU's second serial port = this terminal):
  boot           start the bootloader (build-qemu/kernel8.bin)         [terminal 1]
  upload [BIN]   send BIN (default build-qemu/kernel_payload.bin) to UART0  [terminal 2]
  attach         gdb-multiarch on kernel_payload.elf -> target remote       [terminal 2]
  then type  gdbbreak  at the "loaded>" prompt in terminal 1 to stop in GDB.
  Quit QEMU with Ctrl-A x.  (make qemu builds build-qemu/; done for you.)

Debugging a kernel through its own GDB stub (UART0 <-> GDB over TCP port \$PORT):
  stub  [IMAGE]  start QEMU only; connect with:  gdb-multiarch -nx ELF -ex 'target remote :\$PORT'
  debug [IMAGE]  start QEMU and open gdb-multiarch attached to it

IMAGE is 'test' (default: loops over a few functions after a "brk #0"), 'crash'
(faults on purpose), or a raw .bin linked for 0x80000 (its .elf, if next to it
or given in \$ELF, supplies symbols). The stub only runs when the kernel executes
a brk or takes a fault, so the kernel must do so (the test kernels do), or call
gdb_poll() so Ctrl-C / "target remote" can break in.

Environment: PORT (default 5555), CROSS (toolchain prefix for building the test
kernels), GDB (debugger to use), BOOT_WAIT (seconds to let the kernel boot first).
USAGE
}

# Build the QEMU variant (console on the mini UART) into build-qemu/.
build_qemu_variant() {
  local cross="${CROSS-}"
  [ -n "${CROSS+x}" ] || { [ "$(uname -m)" = aarch64 ] || cross="aarch64-linux-gnu-"; }
  make -s qemu CROSS="$cross" >&2
}

# gdb must know AArch64: on x86 that is gdb-multiarch, not plain gdb.
find_gdb() {
  local g
  for g in "${GDB:-}" gdb-multiarch aarch64-linux-gnu-gdb gdb; do
    [ -n "$g" ] && command -v "$g" >/dev/null 2>&1 || continue
    if "$g" -q -nx -batch -ex 'set architecture aarch64' >/dev/null 2>&1; then
      echo "$g"
      return 0
    fi
  done
  echo "error: no GDB with AArch64 support found (try: sudo apt install gdb-multiarch)" >&2
  return 1
}

# Sets KERNEL (raw binary) and ELF (symbols, may be empty) for stub/debug modes.
resolve_image() {
  local image="${1:-test}"
  case "$image" in
    test|crash)
      python3 tests/run_tests.py --build-only >&2
      KERNEL="tests/build/${image}_main.bin"
      ELF="tests/build/${image}_main.elf"
      ;;
    *)
      KERNEL="$image"
      ELF="${ELF:-${image%.bin}.elf}"
      [ -f "$ELF" ] || ELF=""
      ;;
  esac
  [ -f "$KERNEL" ] || { echo "error: $KERNEL not found" >&2; exit 1; }
}

# Give QEMU the raw binary: handed an ELF it starts every core on the same code.
uart0_tcp() { echo "-serial" "tcp:127.0.0.1:${PORT},server=on,wait=off"; }

case "$MODE" in
  stdio)
    echo "[run-qemu] mode=stdio"
    exec "${QEMU[@]}" -kernel "${KERNEL:-kernel8.bin}" -serial stdio ;;
  pty)
    echo "[run-qemu] mode=pty"
    echo "[run-qemu] QEMU will print PTY path (/dev/pts/X)"
    exec "${QEMU[@]}" -kernel "${KERNEL:-kernel8.bin}" -serial pty ;;
  gdb)
    echo "[run-qemu] mode=gdb (-serial pty -S -s)"
    echo "[run-qemu] Attach GDB to :1234 and continue before uart_send.py"
    exec "${QEMU[@]}" -kernel "${KERNEL:-kernel8.bin}" -serial pty -S -s ;;
  stub)
    resolve_image "${2:-}"
    echo "[run-qemu] mode=stub  kernel=$KERNEL  UART0 -> tcp:127.0.0.1:$PORT"
    echo "[run-qemu] connect:   gdb-multiarch -nx ${ELF:-<elf>} -ex 'target remote localhost:$PORT'"
    echo "[run-qemu] connect after the kernel has booted (about a second); Ctrl-C here quits QEMU"
    # shellcheck disable=SC2046
    exec "${QEMU[@]}" -kernel "$KERNEL" $(uart0_tcp) ;;
  debug)
    resolve_image "${2:-}"
    GDB_BIN="$(find_gdb)"
    echo "[run-qemu] mode=debug  kernel=$KERNEL  gdb=$GDB_BIN"
    # shellcheck disable=SC2046
    "${QEMU[@]}" -kernel "$KERNEL" $(uart0_tcp) >/dev/null &
    QPID=$!
    trap 'kill "$QPID" 2>/dev/null || true' EXIT
    trap 'exit 1' HUP TERM PIPE     # still run the EXIT trap (Ctrl-C belongs to GDB)
    # Let the kernel boot and reach its first stop before GDB connects: the
    # stub's opening stop reply must not land in the middle of GDB's handshake.
    sleep "${BOOT_WAIT:-2}"
    kill -0 "$QPID" 2>/dev/null || { echo "error: QEMU exited" >&2; exit 1; }
    # -nx: skip .gdbinit, which points at the real board (/dev/ttyUSB0).
    "$GDB_BIN" -q -nx ${ELF:+"$ELF"} \
      -ex "set remote hardware-breakpoint-limit 6" \
      -ex "set remote hardware-watchpoint-limit 4" \
      -ex "target remote localhost:$PORT" ;;
  boot)
    build_qemu_variant
    echo "[run-qemu] mode=boot  UART0 (upload + GDB) -> tcp:127.0.0.1:$PORT   console -> this terminal"
    echo "[run-qemu] next: ./scripts/run-qemu.sh upload   then   ./scripts/run-qemu.sh attach"
    echo "[run-qemu] quit QEMU with Ctrl-A x"
    # serial 0 = PL011 UART0, serial 1 = mini UART (the console in the QEMU build)
    exec "${QEMU[@]}" -kernel build-qemu/kernel8.bin \
      -serial "tcp:127.0.0.1:${PORT},server=on,wait=off" -serial mon:stdio ;;
  upload)
    build_qemu_variant
    exec python3 scripts/uart_send_pi4.py --port "socket://localhost:${PORT}" \
      "${2:-build-qemu/kernel_payload.bin}" ;;
  attach)
    build_qemu_variant
    GDB_BIN="$(find_gdb)"
    # -nx: skip .gdbinit, which points at the real board (/dev/ttyUSB0).
    exec "$GDB_BIN" -q -nx build-qemu/kernel_payload.elf \
      -ex "set remote hardware-breakpoint-limit 6" \
      -ex "set remote hardware-watchpoint-limit 4" \
      -ex "target remote localhost:$PORT" ;;
  -h|--help|help)
    usage ;;
  *)
    usage >&2
    exit 1 ;;
esac
