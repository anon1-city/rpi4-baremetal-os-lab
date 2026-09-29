#!/usr/bin/env python3
"""Upload a kernel image to the Pi 4 bootloader over UART0.

    python3 scripts/uart_send_pi4.py [--port PORT_OR_URL] [--retries N] IMAGE

IMAGE is a raw .bin (loaded at 0x200000) or an ELF (its PT_LOAD segments are loaded
at their physical addresses; prefer the ELF, it carries its own link address).

Needs pyserial:  sudo apt install python3-serial   (or  pip install pyserial)

Protocol (little-endian): magic 0x4C424B52 "RKBL", u32 size, u32 CRC-32 of the
image (the value zlib.crc32 gives), then the image. The bootloader skips any stray
bytes before the magic, gives up on a transfer after 1 s of silence, and answers
"OK <size> <entry>" or "ERR <reason>".

Exit status: 0 uploaded, 1 the bootloader refused it or did not answer, 2 the
image or the serial port could not be used.
"""
import argparse
import os
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: sudo apt install python3-serial   (or: pip install pyserial)")

MAGIC = 0x4C424B52
BAUD = 115200                  # fixed in the bootloader
CHUNK = 1024
ANSWER_TIMEOUT_S = 5.0         # after the last byte, wait this long for OK / ERR
BOOTLOADER_RX_TIMEOUT_S = 1.0  # the bootloader abandons a stalled transfer after this


def default_port():
    return os.environ.get("UART_PORT", "/dev/ttyUSB0")


def read_line(ser, deadline):
    """One text line from the bootloader, or None at the deadline."""
    buf = bytearray()
    while time.monotonic() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b == b"\n":
            return buf.decode("ascii", errors="replace").strip("\r")
        buf += b
        if len(buf) > 512:
            del buf[:-512]
    return None


def upload_once(ser, image):
    """Send the image once. Returns (True, entry_line) or (False, reason)."""
    header = struct.pack("<III", MAGIC, len(image), zlib.crc32(image) & 0xFFFFFFFF)
    ser.reset_input_buffer()
    ser.write(header)
    sent, last_pct = 0, -1
    while sent < len(image):
        n = ser.write(image[sent:sent + CHUNK])
        sent += n
        pct = 100 * sent // len(image)
        if pct // 10 != last_pct // 10 and sys.stderr.isatty():
            print(f"\r  sending... {pct}%", end="", file=sys.stderr, flush=True)
            last_pct = pct
    ser.flush()
    if sys.stderr.isatty():
        print(file=sys.stderr)

    deadline = time.monotonic() + ANSWER_TIMEOUT_S
    while True:
        line = read_line(ser, deadline)
        if line is None:
            return False, f"no OK/ERR within {ANSWER_TIMEOUT_S:.0f} s (is the bootloader running? power-cycle the target)"
        if line:
            print(f"[pi4] {line}")
        if line.startswith("OK "):
            return True, line
        if line.startswith("ERR "):
            return False, line


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("image", help="kernel .bin (raw) or .elf")
    ap.add_argument("--port", default=default_port(),
                    help="serial device or pyserial URL, e.g. socket://localhost:5555 "
                         "(default: $UART_PORT or /dev/ttyUSB0)")
    ap.add_argument("--retries", type=int, default=2, help="extra attempts after a failure (default 2)")
    args = ap.parse_args()

    try:
        with open(args.image, "rb") as f:
            image = f.read()
    except OSError as e:
        print(f"error: cannot read {args.image}: {e.strerror}", file=sys.stderr)
        return 2
    if not image:
        print(f"error: {args.image} is empty", file=sys.stderr)
        return 2

    try:
        ser = serial.serial_for_url(args.port, baudrate=BAUD, timeout=0.2)
    except (serial.SerialException, ValueError, OSError) as e:
        print(f"error: cannot open {args.port}: {e}", file=sys.stderr)
        by_path = "/dev/serial/by-path"
        if os.path.isdir(by_path):
            print("serial devices present: " + ", ".join(sorted(os.listdir(by_path))), file=sys.stderr)
        print("(close minicom/screen first; see the README about stable device names)", file=sys.stderr)
        return 2

    with ser:
        print(f"--- {args.image}: {len(image)} bytes, crc32 0x{zlib.crc32(image) & 0xFFFFFFFF:08X}, via {args.port} ---")
        for attempt in range(args.retries + 1):
            ok, msg = upload_once(ser, image)
            if ok:
                print(f"[OK] the bootloader accepted the image: {msg}")
                return 0
            print(f"[FAIL] {msg}", file=sys.stderr)
            if attempt < args.retries:
                # Let the bootloader's own timeout abandon a half-received transfer,
                # otherwise the retry would be taken for the rest of the old one.
                print(f"retrying ({attempt + 2}/{args.retries + 1})...", file=sys.stderr)
                time.sleep(BOOTLOADER_RX_TIMEOUT_S + 0.5)
        return 1


if __name__ == "__main__":
    sys.exit(main())
