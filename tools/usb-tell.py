#!/usr/bin/env python3
"""Send a board a message over its USB serial, for a board with no BLE (envo).

    printf '!clock\\ndate ...\\n' | tools/usb-tell.py --device envo

What BLE's tell carries, as lines: first "!sync N", the time as N seconds past
local midnight (BLE's time characteristic); then the message from stdin, line
by line; then a lone "." to end it. The board (main.c, envo_usb_task) sets its
clock from the sync and hands the message to the same handler a BLE write
reaches, so tools/push-clock.sh's "!clock" block goes unchanged.

The board is found by its MAC in tools/boards.tsv, whatever its port is
called, and the port is opened without the reset a plain open would cause
(the same care tools/noise-relay.py takes). Exits 1 if the board is absent or
the port is held (by idf.py monitor, say).
"""
import argparse
import datetime
import importlib.util
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def relay():
    """noise-relay.py's port helpers, loaded from its file (its name has a dash)."""
    spec = importlib.util.spec_from_file_location("noise_relay", os.path.join(HERE, "noise-relay.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--device", required=True, help="the board's name in tools/boards.tsv")
    args = ap.parse_args()
    body = sys.stdin.read().rstrip("\n")

    r = relay()
    r.need_pyserial()
    import serial.tools.list_ports
    mac = r.registry().get(args.device)
    if not mac:
        sys.exit("usb-tell: no %s in tools/boards.tsv" % args.device)
    dev = r.find_port(mac.upper(), list(serial.tools.list_ports.comports()))
    if not dev:
        print("usb-tell: %s (%s) is not on USB" % (args.device, mac), file=sys.stderr)
        return 1
    try:
        port = r.open_quietly(dev)
    except Exception as e:                     # held, or gone between list and open
        print("usb-tell: %s: %s" % (dev, e), file=sys.stderr)
        return 1
    # A moment before writing: bytes sent the instant the port opens were
    # lost (2026-09-27, !toff and !rhoff never arrived), the board's end not
    # yet reading; and a moment after, so the close does not cut them off.
    time.sleep(0.5)
    now = datetime.datetime.now()
    secs = now.hour * 3600 + now.minute * 60 + now.second
    lines = ["!sync %d" % secs] + [l for l in body.split("\n") if l != "."] + ["."]
    port.write(("\n".join(lines) + "\n").encode())
    port.flush()
    time.sleep(0.5)
    port.close()
    print("usb-tell: %s on %s, %d line(s)" % (args.device, dev, len(lines)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
