#!/usr/bin/env python3
"""Talk to panel1's settings console over USB, without resetting it.

    tools/panel1.py status
    tools/panel1.py wifi list
    tools/panel1.py wifi add "Office WiFi" 'the password'
    tools/panel1.py wifi forget Tabriz
    tools/panel1.py relay https://script.google.com/macros/s/.../exec

Needs only pyserial (python3 -m pip install --user pyserial), not ESP-IDF.
The settings live in panel1's NVS: they stay through restarts and reflashes.
"""
import glob, sys, time
import serial

ports = glob.glob("/dev/cu.usbmodem5B91*") or glob.glob("/dev/cu.wchusbserial*")
if not ports:
    sys.exit("panel1 is not on USB (no CH343 port)")
args = sys.argv[1:] or ["status"]
# Words with spaces go across quoted, as the console reads them.
line = " ".join(a if a.startswith("https://") or " " not in a else '"%s"' % a for a in args)

s = serial.Serial()
s.port, s.baudrate, s.timeout = ports[0], 115200, 0.2
s.dtr = s.rts = True          # held, so opening the port does not reset it
s.open()
s.rts = s.dtr = False
time.sleep(0.2)
s.reset_input_buffer()
s.write((line + "\n").encode())
end, out = time.time() + 2.5, b""
while time.time() < end:
    out += s.read(4096)
for l in out.decode("utf-8", "replace").splitlines():
    if not l.startswith(("I (", "W (", "E (")):   # the board's own log in between
        print(l)
