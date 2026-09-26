#!/usr/bin/env python3
"""Relay between the iOS app (WebSocket) and the rig (USB serial).

One WebSocket text message = one protocol line, no newline. Lines pass through
unchanged in both directions. The bridge adds its own status lines:
    bridge serial=connected port=<path>
    bridge serial=disconnected

    .venv/bin/python tools/bridge.py [--port /dev/cu.usbmodemXXXX] [-v]
"""

import argparse
import asyncio
import glob
import threading
import time

import serial
import websockets

WS_HOST, WS_PORT = "127.0.0.1", 8765
BAUD = 115200


def log(msg):
    print(f"{time.strftime('%H:%M:%S')}  {msg}", flush=True)


class SerialLink:
    """Owns the USB port on a background thread and reopens it after drops."""

    def __init__(self, wanted, on_line, on_state):
        self.wanted = wanted
        self.on_line = on_line
        self.on_state = on_state
        self.lock = threading.Lock()
        self.port = None
        self.path = None

    def status_line(self):
        if self.port:
            return f"bridge serial=connected port={self.path}"
        return "bridge serial=disconnected"

    def write(self, line):
        with self.lock:
            if not self.port:
                return False
            try:
                self.port.write(line.encode("ascii", "replace") + b"\n")
                return True
            except (serial.SerialException, OSError):
                return False

    def run(self):
        while True:
            path = self.wanted or next(iter(sorted(glob.glob("/dev/cu.usbmodem*"))), None)
            try:
                p = serial.Serial(path, BAUD, timeout=0.2) if path else None
            except (serial.SerialException, OSError):
                p = None
            if not p:
                time.sleep(0.5)
                continue

            with self.lock:
                self.port, self.path = p, path
            log(f"serial connected {path}")
            self.on_state(self.status_line())
            buf = b""
            try:
                while True:
                    buf += p.read(p.in_waiting or 1)
                    while b"\n" in buf:
                        raw, buf = buf.split(b"\n", 1)
                        line = raw.decode(errors="replace").strip()
                        if line:
                            self.on_line(line)
            except (serial.SerialException, OSError):
                pass
            with self.lock:
                self.port = None
            try:
                p.close()
            except Exception:
                pass
            log("serial disconnected")
            self.on_state(self.status_line())


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="serial device (default: first /dev/cu.usbmodem*)")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every line")
    args = ap.parse_args()

    loop = asyncio.get_running_loop()
    outbox = asyncio.Queue()
    client = None

    def from_thread(line):
        loop.call_soon_threadsafe(outbox.put_nowait, line)

    def on_rig_line(line):
        if args.verbose or line.startswith(("err", "hello")) or "BROWNOUT" in line:
            log(f"rig < {line}")
        from_thread(line)

    link = SerialLink(args.port, on_rig_line, from_thread)
    threading.Thread(target=link.run, daemon=True).start()

    async def pump():
        while True:
            line = await outbox.get()
            ws = client
            if ws is None:
                continue
            try:
                await ws.send(line)
            except websockets.ConnectionClosed:
                pass

    async def handle(ws):
        nonlocal client
        if client is not None:
            log("new app connection replaces the old one")
            await client.close()
        client = ws
        log("app connected")
        await ws.send(link.status_line())
        try:
            async for msg in ws:
                if not isinstance(msg, str):
                    continue
                line = msg.strip()
                if not line:
                    continue
                if args.verbose:
                    log(f"app > {line}")
                link.write(line)
        except websockets.ConnectionClosed:
            pass
        finally:
            if client is ws:
                client = None
                log("app disconnected")

    asyncio.create_task(pump())
    async with websockets.serve(handle, WS_HOST, WS_PORT):
        log(f"bridge on ws://{WS_HOST}:{WS_PORT}")
        await asyncio.Future()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
