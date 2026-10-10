#!/usr/bin/env python3
"""Talk to stan-claw's settings console over USB, without resetting it.

    tools/stan-claw.py status
    tools/stan-claw.py key claude sk-ant-...
    tools/stan-claw.py mcp url https://example.com/mcp
    tools/stan-claw.py wifi add "Office WiFi" 'password'

Needs only pyserial. Settings live in the board's NVS and survive reflashes.
"""
import glob, sys, time
import serial

ports = glob.glob("/dev/cu.usbmodem5B91*") or glob.glob("/dev/cu.wchusbserial*")
if not ports:
    sys.exit("the board is not on USB (no CH343 port)")
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
end, out = time.time() + 2.5, b""
while time.time() < end:
    out += s.read(4096)
for l in out.decode("utf-8", "replace").splitlines():
    if not l.startswith(("I (", "W (", "E (")):
        print(l)
