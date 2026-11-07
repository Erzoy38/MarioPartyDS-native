"""Network simulator: a TCP proxy that delays, jitters and breaks connections, for the online tests.

Usage: python tools/netsim.py --listen PORT --target HOST:PORT [--delay MS] [--jitter MS]
                              [--loss P] [--loss-wait MS] [--control FILE] [--log FILE] [--seed N]

Every byte that comes in on one side goes out on the other after --delay ms, plus a random part
of up to --jitter ms. With probability --loss a piece waits --loss-wait ms more (default 200), which
is what a lost TCP segment costs: TCP itself sends it again, so nothing is lost, only late. The order
of the bytes in each direction stays as it was, as on a real connection.

--control FILE is read every 50 ms. A line written there is done once:
  cut [SECONDS]     closes all connections now and refuses new ones for SECONDS (default 0)
  silent SECONDS    passes nothing on for SECONDS but keeps the connections open (a dead line)
  delay MS [JITTER] changes the delay from now on
Each command is reported in the log with the time.
"""
import argparse
import asyncio
import os
import random
import sys
import time


class Proxy:
    def __init__(self, args):
        self.target_host, self.target_port = args.target.rsplit(":", 1)
        self.target_port = int(self.target_port)
        self.delay = args.delay / 1000
        self.jitter = args.jitter / 1000
        self.loss = args.loss
        self.loss_wait = args.loss_wait / 1000
        self.random = random.Random(args.seed)
        self.control = args.control
        self.refuse_until = 0.0
        self.silent_until = 0.0
        self.connections = set()
        self.log_file = open(args.log, "a", encoding="utf-8") if args.log else sys.stdout
        self.started = time.monotonic()
        self.count = 0

    def log(self, text):
        self.log_file.write(f"{time.monotonic() - self.started:8.2f} {text}\n")
        self.log_file.flush()

    def late(self):
        wait = self.delay + self.random.uniform(0, self.jitter)
        if self.loss > 0 and self.random.random() < self.loss:
            wait += self.loss_wait
        return wait

    async def pipe(self, reader, writer, name):
        """Passes bytes on, each piece at its own time, never before the one in front of it."""
        queue = asyncio.Queue()

        async def send():
            while True:
                due, data = await queue.get()
                if data is None:
                    break
                now = time.monotonic()
                if due > now:
                    await asyncio.sleep(due - now)
                while time.monotonic() < self.silent_until:
                    await asyncio.sleep(0.02)
                writer.write(data)
                await writer.drain()

        sender = asyncio.create_task(send())
        last_due = 0.0
        try:
            while True:
                data = await reader.read(65536)
                if not data:
                    break
                due = max(time.monotonic() + self.late(), last_due)
                last_due = due
                queue.put_nowait((due, data))
        except (ConnectionError, OSError):
            pass
        queue.put_nowait((0, None))
        try:
            await sender
        except (ConnectionError, OSError):
            pass
        try:
            writer.close()
        except OSError:
            pass

    async def handle(self, client_reader, client_writer):
        if time.monotonic() < self.refuse_until:
            client_writer.close()
            return
        self.count += 1
        number = self.count
        try:
            server_reader, server_writer = await asyncio.open_connection(self.target_host, self.target_port)
        except OSError:
            self.log(f"connection {number}: target not reachable")
            client_writer.close()
            return
        self.log(f"connection {number} open")
        pair = (client_writer, server_writer)
        self.connections.add(pair)
        await asyncio.gather(self.pipe(client_reader, server_writer, "up"), self.pipe(server_reader, client_writer, "down"))
        self.connections.discard(pair)
        self.log(f"connection {number} closed")

    def cut(self, seconds):
        self.refuse_until = time.monotonic() + seconds
        for pair in list(self.connections):
            for writer in pair:
                try:
                    writer.transport.abort()
                except OSError:
                    pass
        self.log(f"cut: {len(self.connections)} connections, new ones refused for {seconds:g} s")

    async def watch_control(self):
        while self.control:
            await asyncio.sleep(0.05)
            try:
                with open(self.control, encoding="utf-8") as f:
                    text = f.read().strip()
            except OSError:
                continue
            if not text:
                continue
            os.remove(self.control)
            for line in text.splitlines():
                parts = line.split()
                if not parts:
                    continue
                if parts[0] == "cut":
                    self.cut(float(parts[1]) if len(parts) > 1 else 0)
                elif parts[0] == "silent":
                    self.silent_until = time.monotonic() + float(parts[1])
                    self.log(f"silent for {parts[1]} s")
                elif parts[0] == "delay":
                    self.delay = float(parts[1]) / 1000
                    if len(parts) > 2:
                        self.jitter = float(parts[2]) / 1000
                    self.log(f"delay now {parts[1]} ms, jitter {self.jitter * 1000:g} ms")
                else:
                    self.log(f"unknown command {line!r}")


async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--listen", type=int, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--delay", type=float, default=0, help="ms, one way")
    parser.add_argument("--jitter", type=float, default=0, help="ms, one way, added at random")
    parser.add_argument("--loss", type=float, default=0, help="share of pieces that come late as if lost")
    parser.add_argument("--loss-wait", type=float, default=200, help="ms a lost piece costs")
    parser.add_argument("--control")
    parser.add_argument("--log")
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()
    proxy = Proxy(args)
    server = await asyncio.start_server(proxy.handle, "127.0.0.1", args.listen)
    proxy.log(f"listening on {args.listen} -> {args.target}, delay {args.delay:g} ms, jitter {args.jitter:g} ms, "
              f"loss {args.loss:g}")
    async with server:
        await asyncio.gather(server.serve_forever(), proxy.watch_control())


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
