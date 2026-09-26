#!/usr/bin/env python3
"""Serial monitor for the servo bring-up that survives USB drops.

Timestamps every line, flags brownouts and reboots, reconnects on its own,
and logs the session to tools/logs/. Type commands and press Enter to send.

    python3 tools/monitor.py [port]
"""

import glob
import os
import sys
import threading
import time

import serial

BAUD = 115200
LOG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")

port_lock = threading.Lock()
port = None


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports[0] if ports else None


def stamp():
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + f".{int(t * 1000) % 1000:03d}"


def emit(log, text):
    line = f"{stamp()}  {text}"
    print(line, flush=True)
    log.write(line + "\n")
    log.flush()


def send_input():
    for line in sys.stdin:
        with port_lock:
            if port is None:
                print("(not connected, dropped)", flush=True)
                continue
            try:
                port.write(line.strip().encode() + b"\n")
            except serial.SerialException:
                pass


def main():
    global port
    wanted = sys.argv[1] if len(sys.argv) > 1 else None
    os.makedirs(LOG_DIR, exist_ok=True)
    log_path = os.path.join(LOG_DIR, time.strftime("session-%Y%m%d-%H%M%S.log"))
    log = open(log_path, "w")
    print(f"logging to {log_path}")
    threading.Thread(target=send_input, daemon=True).start()

    dropped_at = None
    last_uptime = None
    while True:
        path = wanted or find_port()
        if not path:
            time.sleep(0.2)
            continue
        try:
            p = serial.Serial(path, BAUD, timeout=0.5)
        except serial.SerialException:
            time.sleep(0.2)
            continue
        with port_lock:
            port = p
        if dropped_at is None:
            emit(log, f"-- connected {path}")
        else:
            emit(log, f"-- reconnected {path} after {time.time() - dropped_at:.1f}s")

        try:
            while True:
                raw = p.readline()
                if not raw:
                    continue
                text = raw.decode(errors="replace").rstrip()
                if not text:
                    continue
                if "BROWNOUT" in text:
                    text = "!!! " + text
                if text.startswith("hb ") and "up=" in text:
                    try:
                        up = float(text.split("up=")[1].split("s")[0])
                    except ValueError:
                        up = last_uptime
                    if up is not None and last_uptime is not None and up < last_uptime:
                        emit(log, "!!! uptime went backwards: the board rebooted")
                    last_uptime = up
                emit(log, text)
        except (serial.SerialException, OSError):
            with port_lock:
                port = None
            dropped_at = time.time()
            emit(log, "!!! serial dropped")
            try:
                p.close()
            except Exception:
                pass


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
