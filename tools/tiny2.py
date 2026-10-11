#!/usr/bin/env python3
"""Talk to tiny2-voice over its USB console (the CP2102 port), without resetting it.

    tools/tiny2.py status
    tools/tiny2.py say "Hello, I am tiny two."
    tools/tiny2.py volume 60
    tools/tiny2.py tone

Needs only pyserial. Settings live in the board's NVS and survive reflashes.
"""
import glob, sys, time
import serial

ports = [p for p in glob.glob("/dev/cu.usbserial-*") if not p.endswith("-0001")]   # -0001 is blinky1
if not ports:
    sys.exit("tiny2 is not on USB (no CP2102 port besides blinky1)")
args = sys.argv[1:] or ["status"]
line = " ".join(a if a.startswith("https://") or (" " not in a and a) else '"%s"' % a for a in args)
s = serial.Serial()
s.port, s.baudrate, s.timeout = ports[0], 115200, 0.2
s.dtr = s.rts = True          # held, so opening the port does not reset it
s.open()
s.rts = s.dtr = False
time.sleep(0.2)
s.reset_input_buffer()
s.write((line + "\n").encode())
# Replies end with an "ok:"/"no:" line; status and the help have none, so
# those end after half a second of quiet. `rec` and `slots` take up to 5 s.
LOG = ("I (", "W (", "E (")
start, out, last, said = time.time(), b"", time.time(), False
while time.time() < start + 7:
    got = s.read(4096)
    if got:
        out, last = out + got, time.time()
    lines = [l for l in out.decode("utf-8", "replace").splitlines() if l.strip() and not l.startswith(LOG)]
    said = said or bool(lines)
    if any(l.startswith(("ok:", "no:")) for l in lines) or (said and time.time() - last > 0.5):
        break
out += s.read(4096)           # the rest of the last line
for l in out.decode("utf-8", "replace").splitlines():
    if not l.startswith(LOG):
        print(l)
