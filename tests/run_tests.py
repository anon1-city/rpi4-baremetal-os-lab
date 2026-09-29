#!/usr/bin/env python3
"""Tests for the GDB stub (src/gdb_stub.c), run on QEMU's raspi4b.

    python3 tests/run_tests.py [-k SUBSTRING] [--cross PREFIX] [--build-only]

Needs qemu-system-aarch64 and an AArch64 cross compiler (default prefix
aarch64-linux-gnu-, or set CROSS / --cross). Each test group boots a small test
kernel (tests/kernels/) with the repo's start.S, uart.c, gdb.c and gdb_stub.c,
then talks to the stub over UART0 with the GDB remote protocol, playing the
part of GDB.
"""
import argparse
import os
import platform
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(HERE, "build")
sys.path.insert(0, HERE)
from rsp import Target, le64, parse_stop, stop_pc   # noqa: E402

failures = 0


def check(name, cond, info=""):
    global failures
    print(("  PASS " if cond else "  FAIL ") + name + ("" if cond else "   -> %s" % (info,)))
    if not cond:
        failures += 1


# ---------------------------------------------------------------- building --

KERNELS = ("test_main", "crash_main", "hook_main", "badsp_main", "stack_main")


def build(cross):
    """Compile each kernel in tests/kernels/ into tests/build/<name>.{bin,elf}."""
    makefile = open(os.path.join(ROOT, "Makefile")).read()
    cflags = re.search(r"^CFLAGS := (.*)$", makefile, re.M).group(1).split()
    cflags = [c if not c.startswith("-Iinclude") else "-I" + os.path.join(ROOT, "include")
              for c in cflags]
    os.makedirs(BUILD, exist_ok=True)

    def cc(src, obj):
        subprocess.check_call([cross + "gcc"] + cflags + ["-c", src, "-o", obj])

    common = []
    for f in ("start.S", "uart.c", "power.c", "gdb.c", "gdb_stub.c"):
        obj = os.path.join(BUILD, f + ".o")
        cc(os.path.join(ROOT, "src", f), obj)
        common.append(obj)

    for name in KERNELS:
        obj = os.path.join(BUILD, name + ".o")
        cc(os.path.join(HERE, "kernels", name + ".c"), obj)
        elf = os.path.join(BUILD, name + ".elf")
        subprocess.check_call([cross + "ld", "-T", os.path.join(ROOT, "linker.ld"),
                               "-o", elf, obj] + common,
                              stderr=subprocess.DEVNULL)
        # QEMU must be given the raw binary: with an ELF it starts every core.
        subprocess.check_call([cross + "objcopy", "-O", "binary", elf,
                               os.path.join(BUILD, name + ".bin")])


def image(name):
    return os.path.join(BUILD, name + ".bin")


def symbols(name, cross, elf=None):
    out = subprocess.check_output([cross + "nm", elf or os.path.join(BUILD, name + ".elf")]).decode()
    return {l.split()[2]: int(l.split()[0], 16) for l in out.splitlines() if len(l.split()) == 3}


def boot(name, banner):
    t = Target(image(name))
    t.wait_text(banner)
    return t, t.recv_pkt()      # the stub reports the boot "brk #0" by itself


def resume(t, how="c", timeout=5):
    t.send(how)
    return t.recv_pkt(timeout)


# --------------------------------------------------- bootloader / upload helpers --

MAGIC = 0x4C424B52


def upload_bytes(img, crc=None):
    """The upload protocol: magic, size, CRC-32 (all little-endian), then the image."""
    if crc is None:
        crc = zlib.crc32(img) & 0xFFFFFFFF
    return struct.pack("<III", MAGIC, len(img), crc) + img


def build_qemu(cross):
    subprocess.check_call(["make", "-s", "-C", ROOT, "qemu", "CROSS=" + cross],
                          stderr=subprocess.DEVNULL)


def drain(t, seconds):
    """Everything the target sends on UART0 during the next `seconds`."""
    out = b""
    end = time.time() + seconds
    while time.time() < end:
        t.s.settimeout(max(0.05, end - time.time()))
        try:
            d = t.s.recv(4096)
        except socket.timeout:
            break
        if not d:
            break
        out += d
    return out.decode("latin-1")


def bootloader(console=False):
    """A fresh QEMU running the bootloader, waiting for an upload."""
    t = Target(os.path.join(ROOT, "build-qemu", "kernel8.bin"), console=console)
    t.wait_text("Waiting for transfer")
    return t


def accepted(text):
    return "OK " in text and "Jumping to loaded entry" in text


# ------------------------------------------------------------------- tests --

def test_registers_and_memory(cross):
    sym = symbols("test_main", cross)
    t, first = boot("test_main", "TEST READY")
    try:
        sig, f = parse_stop(first)
        check("initial stop is T05 with pc, sp, fp and thread",
              sig == 5 and all(k in f for k in ("20", "1f", "1d")) and f.get("thread") == "1", first)
        check("'?' repeats the stop reply", t.cmd("?") == first)

        g = t.cmd("g")
        check("g reply has core + FP layout (788 bytes)", len(g) == 2 * 788, len(g))
        xml, off = "", 0
        while True:
            r = t.cmd("qXfer:features:read:target.xml:%x,400" % off)
            xml += r[1:]
            off += len(r) - 1
            if r[0] == "l":
                break
        check("target.xml has the core and fpu features",
              "org.gnu.gdb.aarch64.core" in xml and "org.gnu.gdb.aarch64.fpu" in xml
              and xml.endswith("</target>"), len(xml))
        check("p out of range -> E01", t.cmd("p100") == "E01")
        check("P/p x5", t.cmd("P5=efcdab8967452301") == "OK" and t.cmd("p5") == "efcdab8967452301")
        v = "00112233445566778899aabbccddeeff"
        check("P/p v3 (FP register)", t.cmd("P25=" + v) == "OK" and t.cmd("p25") == v)
        g2 = t.cmd("g")
        check("v3 shows up in g", g2[2 * 268 + 2 * 48:2 * 268 + 2 * 64] == v)
        check("P fpsr / fpcr", t.cmd("P42=00000000") == "OK" and t.cmd("P43=00000000") == "OK")
        check("G round trip", t.cmd("G" + g2) == "OK" and t.cmd("g") == g2)

        gv = sym["g_var"]
        check("M / m", t.cmd("M%x,4:deadbeef" % (gv + 0x100)) == "OK"
              and t.cmd("m%x,4" % (gv + 0x100)) == "deadbeef")
        # '}' escapes the next byte (XOR 0x20): "}\x03" is '#', "}]" is '}'
        check("X with escaped bytes ('#' and '}')",
              t.cmd("X%x,2:\x7d\x03\x7d\x5d" % (gv + 0x110)) == "OK"
              and t.cmd("m%x,2" % (gv + 0x110)) == "237d", t.cmd("m%x,2" % (gv + 0x110)))
        check("read of an unmapped address -> E14", t.cmd("m1000000000000,8") == "E14")
        check("write to an unmapped address -> E14", t.cmd("M1000000000000,4:deadbeef") == "E14")
        check("stub still alive after both", t.cmd("?") == first)
        check("1024-byte read", len(t.cmd("m%x,400" % sym["target_fn"])) == 2048)

        t.s.sendall(b"$m0,4#00")                     # deliberately wrong checksum
        check("bad checksum is NAKed", t.s.recv(1) == b"-")
        t.send("m%x,4" % sym["target_fn"])
        r1 = t.recv_pkt()
        t.s.sendall(b"-")
        check("'-' from GDB retransmits the last reply", r1 == t.recv_pkt() and len(r1) == 8)

        t.interrupt()
        time.sleep(0.2)
        check("stray Ctrl-C while stopped is ignored", t.cmd("?") == first)

        brk_pc = stop_pc(f)
        sig, f = parse_stop(resume(t, "vCont;s"))
        check("step at 'brk #0' lands on pc+4 without executing anything",
              sig == 5 and stop_pc(f) == brk_pc + 4)
        check("vCont? lists c, C, s, S", t.cmd("vCont?") == "vCont;c;C;s;S")

        out = t.monitor("info")
        check("monitor info", "stop reason" in out and "ESR_EL1" in out, out)
        out = t.monitor("bp")
        check("monitor bp", "hw watchpoint registers" in out, out)
    finally:
        t.close()


def test_breakpoints(cross):
    sym = symbols("test_main", cross)
    fn, wr, gv = sym["target_fn"], sym["write_var"], sym["g_var"]
    t, _ = boot("test_main", "TEST READY")
    try:
        check("Z0", t.cmd("Z0,%x,4" % fn) == "OK")
        check("Z0 unaligned -> E11", t.cmd("Z0,%x,4" % (fn + 1)) == "E11")
        check("Z0 on an unmapped address -> E14", t.cmd("Z0,1000000000000,4") == "E14")
        check("reads hide the planted brk", t.cmd("m%x,4" % fn) != "200020d4")

        sig, f = parse_stop(resume(t, "vCont;c"))
        check("software breakpoint stop has swbreak",
              sig == 5 and stop_pc(f) == fn and "swbreak" in f, f)
        v1 = t.read64(gv)
        sig, f = parse_stop(resume(t, "c"))      # breakpoint still planted
        check("continue from it steps over silently and hits it in the next loop",
              sig == 5 and stop_pc(f) == fn and "swbreak" in f, f)
        check("exactly one loop ran", (v1, t.read64(gv)) == (0, 0x1234))
        sig, f = parse_stop(resume(t, "vCont;s"))
        check("step off the breakpoint", sig == 5 and stop_pc(f) == fn + 4 and "swbreak" not in f, f)
        orig = t.cmd("m%x,4" % fn)
        check("z0", t.cmd("z0,%x,4" % fn) == "OK" and t.cmd("m%x,4" % fn) == orig)
        check("z0 of an unknown breakpoint -> E02", t.cmd("z0,%x,4" % fn) == "E02")

        check("Z1", t.cmd("Z1,%x,4" % wr) == "OK")
        check("Z1 misaligned -> E11", t.cmd("Z1,%x,4" % (wr + 2)) == "E11")
        sig, f = parse_stop(resume(t))
        check("hardware breakpoint stop has hwbreak",
              sig == 5 and stop_pc(f) == wr and "hwbreak" in f, f)
        sig, f = parse_stop(resume(t))
        check("continue from it steps over it and hits it in the next loop",
              sig == 5 and stop_pc(f) == wr and "hwbreak" in f, f)
        check("z1", t.cmd("z1,%x,4" % wr) == "OK")

        t.cmd("Z0,%x,4" % fn)
        t.send("k")
        time.sleep(0.2)
        check("k (kill) removes planted breakpoints, no reply", t.cmd("m%x,4" % fn) == orig)
    finally:
        t.close()


def test_watchpoints(cross):
    sym = symbols("test_main", cross)
    fn, wr, rd, gv = sym["target_fn"], sym["write_var"], sym["read_var"], sym["g_var"]
    t, _ = boot("test_main", "TEST READY")
    try:
        check("Z2 (write watch)", t.cmd("Z2,%x,8" % gv) == "OK")
        for i in range(6):                       # loop alternates: += 1, then = 0x1234
            before = t.read64(gv)
            sig, f = parse_stop(resume(t))
            pc, after = stop_pc(f), t.read64(gv)
            check("hit %d reports watch:addr" % i, sig == 5 and int(f.get("watch", "0"), 16) == gv, f)
            if i % 2 == 0:
                check("  in target_fn, after the store (value incremented)",
                      fn < pc < wr and after == before + 1, (hex(pc), hex(before), hex(after)))
            else:
                check("  in write_var, after the store (value 0x1234)",
                      wr < pc < rd and after == 0x1234, (hex(pc), hex(after)))
        sig, f = parse_stop(resume(t, "vCont;s"))
        check("step after a watch stop is a plain step", sig == 5 and "watch" not in f, f)

        t.cmd("z2,%x,8" % gv)
        t.cmd("Z2,%x,8" % gv)
        found = False
        for _ in range(150):
            sig, f = parse_stop(resume(t, "vCont;s"))
            if "watch" in f:
                found = True
                break
        check("a watchpoint that fires during a single step is reported", found)
        t.cmd("z2,%x,8" % gv)

        check("Z3 (read watch)", t.cmd("Z3,%x,8" % gv) == "OK")
        sig, f = parse_stop(resume(t))
        check("rwatch reported", int(f.get("rwatch", "0"), 16) == gv, f)
        t.cmd("z3,%x,8" % gv)
        check("Z4 (access watch, 4 bytes)", t.cmd("Z4,%x,4" % gv) == "OK")
        sig, f = parse_stop(resume(t))
        check("awatch reported", int(f.get("awatch", "0"), 16) == gv, f)
        t.cmd("z4,%x,4" % gv)

        check("Z2 longer than 8 bytes -> E11", t.cmd("Z2,%x,10" % gv) == "E11")
        ok = [t.cmd("Z2,%x,8" % (gv + 8 * i)) for i in range(6)]
        check("running out of watch registers -> E12", ok[0] == "OK" and "E12" in ok, ok)
        for i in range(6):
            t.cmd("z2,%x,8" % (gv + 8 * i))
        check("region straddling two 8-byte blocks",
              t.cmd("Z2,%x,8" % (gv + 4)) == "OK" and t.cmd("z2,%x,8" % (gv + 4)) == "OK")

        t.send("c")
        check("no stop after all watchpoints are removed", t.quiet())
    finally:
        t.close()


def test_crash_reporting(cross):
    t, _ = boot("crash_main", "CRASH READY")
    try:
        sig, f = parse_stop(resume(t))
        check("data abort stops with SIGSEGV or SIGBUS", sig in (11, 10), sig)
        out = t.monitor("info")
        check("monitor info describes it", "data abort" in out and "0x1000000000000" in out, out)
        pc = stop_pc(f)
        check("P pc to skip the faulting store",
              t.cmd("P20=" + (pc + 4).to_bytes(8, "little").hex()) == "OK")
        sig, f = parse_stop(resume(t))
        check("undefined instruction stops with SIGILL", sig == 4, sig)
        check("the stub still answers", t.cmd("g") != "")
    finally:
        t.close()


def test_interrupt_and_attach(cross):
    t, first = boot("test_main", "TEST READY")
    try:
        brk_pc = stop_pc(parse_stop(first)[1])
        t.send("c")
        time.sleep(0.5)
        t.interrupt()
        sig, f = parse_stop(t.recv_pkt())
        check("Ctrl-C stops the running target with T02", sig == 2, f)
        check("  inside the program loop", stop_pc(f) > brk_pc)
        t.send("c")
        time.sleep(0.5)
        t.interrupt()
        check("it resumes past the interrupt and can be interrupted again",
              parse_stop(t.recv_pkt())[0] == 2)

        check("D detaches", t.cmd("D") == "OK")
        time.sleep(0.5)
        t.s.sendall(b"$?#3f")                    # GDB connecting to a running kernel
        r = t.recv_pkt()
        check("a connecting GDB breaks in and gets a stop reply", r.startswith("T05"), r)
        check("registers readable after the break-in", len(t.cmd("g")) == 2 * 788)
        t.send("c")
        time.sleep(0.4)
        t.interrupt()
        check("and Ctrl-C works again", t.recv_pkt().startswith("T02"))
    finally:
        t.close()


def test_payload_two_channels(cross):
    """The real workflow on QEMU: UART0 carries the upload and GDB, the console
    (mini UART in the QEMU build) is a separate channel."""
    qdir = os.path.join(ROOT, "build-qemu")
    build_qemu(cross)
    sym = symbols(None, cross, os.path.join(qdir, "kernel_payload.elf"))
    img = open(os.path.join(qdir, "kernel_payload.bin"), "rb").read()

    t = Target(os.path.join(qdir, "kernel8.bin"), console=True)
    try:
        t.wait_text("Waiting for transfer")
        t.s.sendall(upload_bytes(img))
        out = t.wait_text("Jumping to loaded entry", 30)
        check("bootloader accepts the upload over UART0", b"OK " in out, out[-200:])

        banner = t.console_wait("loaded>")
        check("payload console comes up on the second channel",
              "command shell ready" in banner and "GDB: UART0" in banner, banner)
        t.console_send("ping\r")
        check("console command works", "pong" in t.console_wait("pong"))
        t.console_wait("loaded>")

        t.console_send("gdbbreak\r")
        check("gdbbreak prints on the console", "[gdb] BRK now" in t.console_wait("BRK now"))
        sig, f = parse_stop(t.recv_pkt(10))
        check("...and the stop arrives on UART0 (T05)", sig == 5 and "20" in f, f)
        check("stopped inside gdb_break()",
              sym["gdb_break"] <= stop_pc(f) < sym["gdb_break"] + 16, hex(stop_pc(f)))

        check("Z0 on process_cmd", t.cmd("Z0,%x,4" % sym["process_cmd"]) == "OK")
        t.send("c")
        check("continue resumes the shell",
              "resumed after break" in t.console_wait("resumed after break"))
        t.console_wait("loaded>")
        t.console_send("ping\r")
        sig, f = parse_stop(t.recv_pkt(10))
        check("typing a command hits the breakpoint (T05 swbreak)",
              sig == 5 and stop_pc(f) == sym["process_cmd"] and "swbreak" in f, f)
        t.send("c")
        check("continue: the command finishes on the console", "pong" in t.console_wait("pong"))
        t.console_wait("loaded>")

        time.sleep(0.3)
        t.interrupt()                       # Ctrl-C while the shell waits for input
        sig, f = parse_stop(t.recv_pkt(10))
        check("Ctrl-C interrupts the idle shell (T02)", sig == 2, f)
        check("z0 removes the breakpoint", t.cmd("z0,%x,4" % sym["process_cmd"]) == "OK")
        t.send("c")
        t.console_send("ping\r")
        check("shell still works after the interrupt", "pong" in t.console_wait("pong"))
    finally:
        t.close()



def patched_elf(elf, field_offset, value):
    """Copy of an ELF with one field of its first PT_LOAD program header replaced."""
    b = bytearray(elf)
    phoff = struct.unpack_from("<Q", b, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", b, 54)
    for i in range(phnum):
        o = phoff + i * phentsize
        if struct.unpack_from("<I", b, o)[0] == 1:          # PT_LOAD
            struct.pack_into("<Q", b, o + field_offset, value)
            break
    return bytes(b)


def test_bootloader_robustness(cross):
    """What a student (or a noisy cable) can do to the upload, and that the
    bootloader survives it without a power cycle."""
    build_qemu(cross)
    qdir = os.path.join(ROOT, "build-qemu")
    img = open(os.path.join(qdir, "kernel_payload.bin"), "rb").read()
    payload_elf = open(os.path.join(qdir, "kernel_payload.elf"), "rb").read()
    wrong_elf = open(os.path.join(BUILD, "test_main.elf"), "rb").read()   # linked at 0x80000

    def send(t, data, seconds=4):
        t.s.sendall(data)
        return drain(t, seconds)

    cases = []

    t = bootloader()
    try:
        out = send(t, b"\x00\xff\x52" + upload_bytes(img))          # junk, incl. half a magic
        check("stray bytes before the header are skipped", accepted(out), out[-200:])
    finally:
        t.close()

    t = bootloader()
    try:
        out = send(t, upload_bytes(img)[: len(img) // 2], 2.5)
        check("a truncated upload times out with an error", "ERR timeout after" in out, out[-200:])
        out = send(t, upload_bytes(img))
        check("...and the next upload works", accepted(out), out[-200:])
    finally:
        t.close()

    t = bootloader()
    try:
        out = send(t, upload_bytes(img, crc=0xDEADBEEF))
        check("a wrong checksum is rejected", "ERR checksum mismatch" in out and not accepted(out))
        out = send(t, upload_bytes(img))
        check("...and the next upload works", accepted(out), out[-200:])
    finally:
        t.close()

    t = bootloader()
    try:
        out = send(t, struct.pack("<III", MAGIC, 0x02000000, 0) + img)   # size > 16 MiB, then data
        check("an oversize header is rejected", "ERR invalid size" in out, out[-200:])
        out = send(t, upload_bytes(img))
        check("...the stray image bytes are skipped and the next upload works", accepted(out), out[-200:])
    finally:
        t.close()

    t = bootloader()
    try:
        out = send(t, upload_bytes(wrong_elf))
        check("an ELF linked for the bootloader's own address is refused",
              "ERR ELF: segment outside the load area" in out and not accepted(out), out[-200:])
        out = send(t, upload_bytes(patched_elf(payload_elf, 8, (1 << 64) - 16)))   # p_offset wraps
        check("an ELF whose p_offset + p_filesz wraps is refused",
              "ERR ELF: segment data outside the file" in out and not accepted(out), out[-200:])
        out = send(t, upload_bytes(patched_elf(payload_elf, 24, 0x1000)))          # p_paddr = 0x1000
        check("an ELF loading at a low address is refused",
              "ERR ELF: segment outside the load area" in out and not accepted(out), out[-200:])
        out = send(t, upload_bytes(img))
        check("the bootloader is still alive after all of them", accepted(out), out[-200:])
    finally:
        t.close()

    t = bootloader(console=True)
    try:
        out = send(t, upload_bytes(payload_elf))
        check("a valid ELF is loaded (PT_LOAD segments)", "ELF image detected" in out and accepted(out), out[-200:])
        check("...and runs", "loaded>" in t.console_wait("loaded>"))
        t.console_send("reboot\r")
        check("reboot resets the board back into the bootloader",
              b"UART Bootloader" in t.wait_text("UART Bootloader", 15))
        t.wait_text("Waiting for transfer", 5)
        out = send(t, upload_bytes(img))
        check("...and a new image can be uploaded without a power cycle", accepted(out), out[-200:])
    finally:
        t.close()


def test_debugger_safety(cross):
    """Things a student kernel can do to the debugger (and the debugger to it)."""
    # 1. a stop on a small stack leaves the stack and its neighbours alone
    sym = symbols("stack_main", cross)
    t, _ = boot("stack_main", "STACK READY")
    try:
        for _ in range(3):
            t.cmd("m%x,400" % sym["mem"])
        t.monitor("info")
        expect = b"\xaa" * 4096 + b"\xbb" * 4096 + b"\xcc" * 16
        got = b""
        for off in range(0, len(expect), 1024):
            got += bytes.fromhex(t.cmd("m%x,%x" % (sym["mem"] + off, min(1024, len(expect) - off))))
        check("a stop on a 4 KB stack does not touch it or its neighbours", got == expect,
              sum(1 for a, b in zip(got, expect) if a != b))
    finally:
        t.close()

    # 2. a fault caused by a corrupted sp is reported
    t, _ = boot("badsp_main", "BADSP READY")
    try:
        sig, f = parse_stop(resume(t))
        check("a fault with a corrupted sp is reported (SIGSEGV/SIGBUS)", sig in (10, 11), sig)
        check("...with the bad sp visible", le64(t.cmd("p1f")) == 1 << 48)
        pc = stop_pc(f)
        check("P sp / P pc repair the program",
              t.cmd("P1f=" + t.cmd("p9")) == "OK" and t.cmd("P20=" + (pc + 4).to_bytes(8, "little").hex()) == "OK")
        t.send("c")
        time.sleep(0.5)
        t.interrupt()
        check("...and it runs on, still interruptible", parse_stop(t.recv_pkt())[0] == 2)
    finally:
        t.close()

    # 3. the kernel's own exceptions are not the debugger's
    sym = symbols("hook_main", cross)
    t, _ = boot("hook_main", "HOOK READY")
    try:
        sig, f = parse_stop(resume(t))
        check("an exception nobody handles (svc #7) stops with SIGSYS", sig == 12, sig)
        check("svc #0 was handled by the kernel's hook without stopping",
              t.read64(sym["svc_count"]) == 2 and t.read64(sym["progress"]) == 1)
        t.send("c")
        time.sleep(0.5)
        t.interrupt()
        check("the kernel runs on after the stop", parse_stop(t.recv_pkt())[0] == 2 and t.read64(sym["progress"]) == 2)
    finally:
        t.close()

    # 4. reading UART0's data register through GDB must not eat GDB's own bytes
    t, _ = boot("test_main", "TEST READY")
    try:
        body = b"mfe201000,4"
        t.s.sendall(b"$" + body + b"#%02x" % (sum(body) & 0xff) + b"X")     # a byte is queued in the FIFO
        check("x/ on UART0 DR returns 0 and leaves the FIFO alone", t.recv_pkt() == "00000000")
        check("other UART0 registers are readable (FR)", t.cmd("mfe201018,4") not in ("E14", ""))
        check("a 16-byte read of device registers works", len(t.cmd("mfe201010,10")) == 32)
    finally:
        t.close()


def test_reset(cross):
    """monitor reset: the watchdog resets the SoC and the program starts over."""
    t, first = boot("test_main", "TEST READY")
    try:
        t.send("qRcmd," + b"reset".hex())
        seen = ""
        while True:
            r = t.recv_pkt(10)
            if r == "OK":
                break
            seen += bytes.fromhex(r[1:]).decode()
        check("monitor reset announces itself", "resetting" in seen, seen)
        out = t.wait_text("TEST READY", 15)
        check("the target restarts from the beginning", b"TEST READY" in out)
        check("...and stops at its first brk again", parse_stop(t.recv_pkt(10))[0] == 5)
    finally:
        t.close()


def find_gdb():
    """A GDB that knows AArch64 (gdb-multiarch; plain gdb on x86 does not)."""
    for g in (os.environ.get("GDB", ""), "gdb-multiarch", "aarch64-linux-gnu-gdb", "gdb"):
        if g and shutil.which(g) and subprocess.call(
                [g, "-q", "-nx", "-batch", "-ex", "set architecture aarch64"],
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0:
            return g
    return None


def test_real_gdb(cross):
    """Drive a real GDB against the stub (skipped if no AArch64 GDB is installed)."""
    import socket
    gdb = find_gdb()
    if gdb is None:
        print("  SKIP: no GDB with AArch64 support (sudo apt install gdb-multiarch)")
        return
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    q = subprocess.Popen(
        ["qemu-system-aarch64", "-cpu", "cortex-a72", "-M", "raspi4b", "-kernel", image("test_main"),
         "-m", "2G", "-smp", "4", "-display", "none", "-monitor", "none",
         "-serial", "tcp:127.0.0.1:%d,server=on,wait=off" % port],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(2)       # let the kernel reach its first stop before GDB connects
        cmds = ["file " + os.path.join(BUILD, "test_main.elf"),
                "set remote hardware-breakpoint-limit 6", "set remote hardware-watchpoint-limit 4",
                "target remote localhost:%d" % port,
                "break write_var", "continue", "bt",
                "delete 1", "hbreak read_var", "watch g_var", "continue", "continue", "continue",
                "p $v3", "info registers cpsr", "monitor info", "detach"]
        args = [gdb, "-q", "-nx", "-batch"]
        for c in cmds:
            args += ["-ex", c]
        out = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=90).stdout
        check("GDB connects and accepts the target description", "main () at" in out, out[:300])
        check("break (Z0) stops in write_var", "Breakpoint 1, write_var" in out)
        check("backtrace reaches main and ends cleanly",
              "#1  0x" in out and "in main ()" in out and "corrupt stack" not in out, out)
        check("hbreak (Z1) is a hardware breakpoint", "Hardware assisted breakpoint 2" in out)
        check("watch (Z2) is a hardware watchpoint", "Hardware watchpoint 3: g_var" in out)
        check("watchpoint stop shows Old/New value", "Old value = " in out and "New value = " in out, out)
        check("FP register prints (p $v3)", "$1 = {d = {f = {" in out, out)
        check("monitor info works through GDB", "stop reason:" in out and "ESR_EL1:" in out)
    finally:
        q.kill()
        q.wait()


TESTS = [test_registers_and_memory, test_breakpoints, test_watchpoints,
         test_crash_reporting, test_interrupt_and_attach, test_real_gdb,
         test_payload_two_channels, test_bootloader_robustness,
         test_debugger_safety, test_reset]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("-k", default="", help="only run tests whose name contains this")
    native = "" if platform.machine() == "aarch64" else "aarch64-linux-gnu-"
    ap.add_argument("--cross", default=os.environ.get("CROSS", native),
                    help="cross-toolchain prefix (default: %(default)r)")
    ap.add_argument("--build-only", action="store_true",
                    help="only build the test kernels into tests/build/ (used by scripts/run-qemu.sh)")
    args = ap.parse_args()

    if shutil.which("qemu-system-aarch64") is None or shutil.which(args.cross + "gcc") is None:
        print("SKIP: needs qemu-system-aarch64 and %sgcc" % args.cross)
        return 0

    build(args.cross)
    if args.build_only:
        return 0
    for fn in TESTS:
        if args.k in fn.__name__:
            print(fn.__name__)
            fn(args.cross)
    print("\n%s (%d failure%s)" % ("FAILED" if failures else "OK", failures, "" if failures == 1 else "s"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
