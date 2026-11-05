#!/usr/bin/env python3
"""Holds the single connection to melonDS's GDB stub and serves tools one at a time.

The stub does not notice a client leaving and then accepts no new one, so without this
each tool run would need a melonDS restart. Start it once after starting melonDS; it stops
when the stub connection ends. Listens on 127.0.0.1 only.

Line protocol (payloads are latin-1 text): CMD <packet>, SEND <packet>, RECV <seconds>, INT.
Replies: OK <hex>, TIMEOUT, ERR <text>.

Usage: python tools/gdb_server.py [--stub-port 3333] [--port 3400]
"""
import argparse
import os
import socket
import socketserver
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gdb_client import SERVER_PORT, Gdb  # noqa: E402


def make_handler(stub, stop):
    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            for raw in self.rfile:
                line = raw.decode("latin-1").rstrip("\n")
                word, _, rest = line.partition(" ")
                try:
                    if word == "CMD":
                        reply = "OK " + stub.command(rest.encode("latin-1")).hex()
                    elif word == "SEND":
                        stub.send(rest.encode("latin-1"))
                        reply = "OK "
                    elif word == "RECV":
                        reply = "OK " + stub.receive(timeout=float(rest)).hex()
                    elif word == "INT":
                        reply = "OK " + stub.interrupt().hex()
                    else:
                        reply = f"ERR unknown request {word}"
                except socket.timeout:
                    reply = "TIMEOUT"
                except (OSError, ConnectionError) as exc:
                    self.wfile.write(f"ERR stub connection lost: {exc}\n".encode("latin-1"))
                    self.wfile.flush()
                    stop()
                    return
                self.wfile.write(reply.encode("latin-1") + b"\n")
                self.wfile.flush()

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stub-port", type=int, default=3333)
    ap.add_argument("--port", type=int, default=SERVER_PORT)
    args = ap.parse_args()

    stub = Gdb(port=args.stub_port, timeout=15.0)
    print("connected to stub:", stub.features.decode(errors="replace"), flush=True)
    print("stub status:", stub.status(), flush=True)
    # Connecting stops the game; let it run until a tool asks otherwise.
    stub.resume()
    print("game resumed", flush=True)

    socketserver.TCPServer.allow_reuse_address = True
    server = socketserver.TCPServer(("127.0.0.1", args.port), None)

    def stop():
        # Called from the serving thread, where server.shutdown() would wait for itself.
        # Without the stub there is nothing left to serve, so end the process.
        print("stub connection lost; stopping", flush=True)
        os._exit(1)

    server.RequestHandlerClass = make_handler(stub, stop)
    print(f"serving on 127.0.0.1:{args.port}", flush=True)
    import threading
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    thread.join()
    return 0


if __name__ == "__main__":
    sys.exit(main())
