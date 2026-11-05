#!/usr/bin/env python3
"""Minimal GDB remote protocol client for melonDS's stub. Standard library only.

Gdb talks to the stub directly; only tools/gdb_server.py should use it, because the stub
allows one connection per melonDS start. Tools use Session, which goes through the server.

Usage: python tools/gdb_client.py probe    read registers and a little memory
       python tools/gdb_client.py resume   let the game run if a tool left it stopped

As a library: with Session() as g: g.interrupt(); g.read(addr, n)
"""
import argparse
import socket
import struct
import sys

REG_NAMES = [f"r{i}" for i in range(13)] + ["sp", "lr", "pc"]
CHUNK = 0x200  # the stub's PacketSize is 0x47F, and each byte takes two hex digits
SERVER_PORT = 3400


class Gdb:
    def __init__(self, host="127.0.0.1", port=3333, timeout=5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = b""
        self.running = True
        # melonDS handshake: the first byte must be '+', within 1 second. It answers
        # with '+'. A packet sent before that answer loses its '$' and ends the session.
        self.sock.sendall(b"+")
        while b"+" not in self.buffer:
            self._fill()
        self.buffer = self.buffer[self.buffer.index(b"+") + 1:]
        self.features = self.command("qSupported:swbreak+;hwbreak+")

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()

    def close(self):
        try:
            self.send(b"D")  # detach: the stub lets the game run on
            self.receive(timeout=2.0)
        except OSError:
            pass
        self.sock.close()

    def send(self, payload):
        checksum = sum(payload) & 0xFF
        self.sock.sendall(b"$" + payload + b"#" + f"{checksum:02x}".encode())

    def _fill(self):
        chunk = self.sock.recv(65536)
        if not chunk:
            raise ConnectionError("stub closed the connection")
        self.buffer += chunk

    def receive(self, timeout=None):
        """Return the next packet's payload, acknowledging it. Skips '+' markers.

        The stub sends no '+' for commands it does not know, so none is required here.
        """
        old = self.sock.gettimeout()
        if timeout is not None:
            self.sock.settimeout(timeout)
        try:
            while True:
                start = self.buffer.find(b"$")
                end = self.buffer.find(b"#", start + 1) if start >= 0 else -1
                if start >= 0 and end >= 0 and len(self.buffer) >= end + 3:
                    payload = self.buffer[start + 1:end]
                    self.buffer = self.buffer[end + 3:]
                    self.sock.sendall(b"+")
                    return payload
                self._fill()
        finally:
            self.sock.settimeout(old)

    def command(self, text):
        self.send(text.encode() if isinstance(text, str) else text)
        return self.receive()

    # Run control ---------------------------------------------------------------
    def status(self):
        return self.command("?")

    def drain(self, quiet=0.25):
        """Discard pending packets until the stub stays silent. Returns what was dropped.

        After stopping, melonDS sends a second stop reply (S05, then S02). Left in the
        queue, it would shift every later reply by one.
        """
        dropped = []
        while True:
            try:
                dropped.append(self.receive(timeout=quiet))
            except (socket.timeout, TimeoutError):
                return dropped

    def interrupt(self):
        self.sock.sendall(b"\x03")
        reply = self.receive()
        self.drain()
        self.running = False
        return reply

    def resume(self):
        self.send(b"c")
        self.running = True

    def wait_stop(self, timeout=30.0):
        reply = self.receive(timeout=timeout)
        self.running = False
        return reply

    def step(self):
        return self.command("s")

    def set_break(self, addr, kind=4):
        return self.command(f"Z0,{addr:x},{kind}")

    def clear_break(self, addr, kind=4):
        return self.command(f"z0,{addr:x},{kind}")

    # Registers and memory ------------------------------------------------------
    def registers(self):
        reply = self.command("g").decode()
        if len(reply) < 16 * 8 or reply[0] in "ST":
            raise IOError(f"unexpected reply to register read: {reply[:40]!r}")
        values = [int.from_bytes(bytes.fromhex(reply[i:i + 8]), "little")
                  for i in range(0, min(len(reply), 16 * 8), 8)]
        return dict(zip(REG_NAMES, values)), reply

    def read(self, addr, count):
        out = bytearray()
        while count:
            size = min(count, CHUNK)
            reply = self.command(f"m{addr:x},{size:x}")
            if not reply or reply.startswith(b"E"):
                raise IOError(f"read failed at {addr:#x}: {reply!r}")
            chunk = bytes.fromhex(reply.decode())
            if len(chunk) != size:
                raise IOError(f"short read at {addr:#x}: {len(chunk):#x} of {size:#x}")
            out += chunk
            addr += size
            count -= size
        return bytes(out)

    def write(self, addr, data):
        reply = self.command(f"M{addr:x},{len(data):x}:{data.hex()}")
        if reply != b"OK":
            raise IOError(f"write failed at {addr:#x}: {reply!r}")

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4))[0]

    def u16(self, addr):
        return struct.unpack("<H", self.read(addr, 2))[0]


class Session(Gdb):
    """Same interface as Gdb, but talks to tools/gdb_server.py.

    melonDS's stub does not notice a client leaving and then accepts no new one, so only
    the server holds the real connection. Tools come and go through it. Leaving the "with"
    block resumes the game if this session stopped it.
    """

    def __init__(self, port=SERVER_PORT, timeout=120.0):
        try:
            self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        except OSError as exc:
            raise SystemExit(f"gdb_server is not running on port {port}: {exc}")
        self.file = self.sock.makefile("rwb")
        self.running = True
        self.halted_here = False
        self.features = b""
        dropped = self.drain()  # replies a crashed tool left behind
        if dropped:
            print(f"dropped {len(dropped)} stale replies", file=sys.stderr)

    def _call(self, line):
        self.file.write(line.encode("latin-1") + b"\n")
        self.file.flush()
        reply = self.file.readline().decode("latin-1").rstrip("\n")
        if not reply:
            raise ConnectionError("gdb_server closed the connection")
        word, _, rest = reply.partition(" ")
        if word == "TIMEOUT":
            raise TimeoutError("no stop reply from the stub")
        if word != "OK":
            raise ConnectionError(f"gdb_server: {reply}")
        return bytes.fromhex(rest)

    def command(self, text):
        text = text.decode("latin-1") if isinstance(text, bytes) else text
        return self._call(f"CMD {text}")

    def send(self, payload):
        self._call(f"SEND {payload.decode('latin-1')}")

    def receive(self, timeout=None):
        return self._call(f"RECV {timeout if timeout is not None else 30.0}")

    def interrupt(self):
        reply = self._call("INT")
        self.drain()
        self.running = False
        self.halted_here = True
        return reply

    def resume(self):
        super().resume()
        self.halted_here = False

    def close(self):
        if self.halted_here:
            self.resume()
        self.sock.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("action", choices=["probe", "resume"])
    args = ap.parse_args()
    with Session() as gdb:
        if args.action == "resume":
            gdb.resume()
            print("resumed")
            return 0
        print("interrupt:", gdb.interrupt())
        regs, raw = gdb.registers()
        print("registers:", " ".join(f"{k}={v:08x}" for k, v in regs.items()))
        print("g packet hex length:", len(raw))
        print("0x02000800:", gdb.read(0x02000800, 16).hex(" "))
    print("done; the game should keep running")
    return 0


if __name__ == "__main__":
    sys.exit(main())
