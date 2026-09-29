"""Minimal GDB remote-protocol client and QEMU launcher for the stub tests."""
import re
import socket
import subprocess
import time


class Target:
    """Runs a raw AArch64 image on QEMU's raspi4b with UART0 on a TCP socket."""

    def __init__(self, image, console=False):
        """console=True also attaches QEMU's second serial port (the mini UART,
        which the QEMU build uses as the console) to a second socket."""
        socks = [socket.socket() for _ in range(2 if console else 1)]
        for s in socks:                     # pick free ports (held open until both chosen)
            s.bind(("127.0.0.1", 0))
        ports = [s.getsockname()[1] for s in socks]
        for s in socks:
            s.close()
        args = ["qemu-system-aarch64", "-cpu", "cortex-a72", "-M", "raspi4b",
                "-kernel", image, "-m", "2G", "-smp", "4", "-display", "none",
                "-monitor", "none"]
        for port in ports:
            args += ["-serial", f"tcp:127.0.0.1:{port},server=on,wait=on"]
        self.q = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        conns = []
        for port in ports:                  # QEMU accepts them in -serial order
            c = None
            for _ in range(100):
                try:
                    c = socket.create_connection(("127.0.0.1", port))
                    break
                except OSError:
                    time.sleep(0.1)
            if c is None:
                self.q.kill()
                raise RuntimeError("could not connect to QEMU")
            conns.append(c)
        self.s = conns[0]                   # UART0: upload + GDB
        self.con = conns[1] if console else None
        self.buf = b""
        self.cbuf = b""

    def console_send(self, text):
        self.con.sendall(text.encode())

    def console_wait(self, text, timeout=10):
        """Read the console (second channel) until `text` appears and return
        everything up to and including it; later output stays buffered. On a
        timeout, returns whatever arrived."""
        want = text.encode()
        end = time.time() + timeout
        self.con.settimeout(timeout)
        while want not in self.cbuf and time.time() < end:
            d = self.con.recv(4096)
            if not d:
                break
            self.cbuf += d
        i = self.cbuf.find(want)
        i = len(self.cbuf) if i < 0 else i + len(want)
        out, self.cbuf = self.cbuf[:i], self.cbuf[i:]
        return out.decode("latin-1")

    def close(self):
        for s in (self.s, self.con):
            try:
                s.close()
            except Exception:
                pass
        self.q.kill()
        self.q.wait()

    def _byte(self):
        if self.buf:
            b, self.buf = self.buf[:1], self.buf[1:]
            return b
        d = self.s.recv(4096)
        if not d:
            raise EOFError
        self.buf = d[1:]
        return d[:1]

    def send(self, payload):
        if isinstance(payload, str):
            payload = payload.encode("latin-1")
        self.s.sendall(b"$" + payload + b"#%02x" % (sum(payload) & 0xff))

    def recv_pkt(self, timeout=5):
        self.s.settimeout(timeout)
        while self._byte() != b"$":
            pass
        data = b""
        while True:
            b = self._byte()
            if b == b"#":
                break
            data += b
        self._byte()
        self._byte()
        self.s.sendall(b"+")
        return data.decode("latin-1")

    def cmd(self, payload, timeout=5):
        self.send(payload)
        return self.recv_pkt(timeout)

    def interrupt(self):
        self.s.sendall(b"\x03")

    def wait_text(self, text, timeout=10):
        """Read UART text until `text` shows up (used for the boot banner)."""
        end = time.time() + timeout
        acc = b""
        self.s.settimeout(timeout)
        while time.time() < end and text.encode() not in acc:
            acc += self._byte()
        return acc

    def quiet(self, seconds=0.7):
        """True if the target sends nothing (apart from acks) for a while."""
        self.s.settimeout(seconds)
        try:
            return self.s.recv(100).replace(b"+", b"") == b""
        except socket.timeout:
            return True

    # -- convenience wrappers -------------------------------------------------
    def regs(self):
        g = self.cmd("g")
        return [le64(g[i * 16:(i + 1) * 16]) for i in range(33)]   # x0-x30, sp, pc

    def pc(self):
        return self.regs()[32]

    def read64(self, addr):
        return le64(self.cmd("m%x,8" % addr))

    def monitor(self, text):
        self.send("qRcmd," + text.encode().hex())
        out = ""
        while True:
            r = self.recv_pkt()
            if r == "OK":
                return out
            out += bytes.fromhex(r[1:]).decode()


def le64(h):
    return int.from_bytes(bytes.fromhex(h), "little")


def parse_stop(p):
    """'T05a:b;c:d;' -> (5, {'a': 'b', 'c': 'd'})."""
    m = re.match(r"T(..)(.*)", p)
    if not m:
        return None, {}
    fields = {}
    for f in m.group(2).split(";"):
        if ":" in f:
            k, v = f.split(":", 1)
            fields[k] = v
    return int(m.group(1), 16), fields


def stop_pc(fields):
    return le64(fields["20"])
