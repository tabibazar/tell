#!/usr/bin/env python3
"""Carry speaker's noise level to watch's Sound page, over USB.

    tools/noise-relay.py                  relay until Ctrl-C
    tools/noise-relay.py --dry-run        read speaker, print what watch would get
    tools/noise-relay.py --verbose        also echo both boards' serial output
    tools/noise-relay.py --self-test      check the parser and the pacing; no ports

speaker prints a line a second on its USB serial ("noise: LAF 47.1 LAeq3 45.8
dBA (est) LAeq1 46.2 | red 0s | 19:59:24 ring on chime off sd ok today 46.9").
The boards never talk to each other -- a house rule, not a limitation -- so
the Mac carries it: this reads that line, keeps the numbers, and writes
"!noise <laf> <laeq3> <today|--> <est|cal>" to watch, no more than once a
second. watch hands the line to the same handler its BLE messages go to.
A line with no level in it is not passed on at all, and neither is anything
old: when nothing arrives, watch says NO SIGNAL, which is the truth.

The boards are found by USB serial number, which on these ESP32-S3s is the
chip's MAC, and never by port name. Both are /dev/cu.usbmodem*, and which gets
which number depends on the order they were plugged in, so the name says
nothing about which board is on the other end (see tools/board-guard.sh). The
MACs come from tools/boards.tsv, with built-in copies for when it is missing.
Unplug either board and the relay waits for it and carries on when it is back.

Opening a port must not reset the board, and on a USB-Serial-JTAG port the
modem lines ARE the reset: the chip resets while the host holds RTS without
DTR (esptool's USBJTAGSerialReset and HardReset lean on exactly that). So the
lines are brought up and down in an order that never passes through that
state; open_quietly() says how. Nothing is ever written to speaker.

pyserial is only installed in ESP-IDF's Python here (neither /usr/bin/python3
nor the python3 on PATH has it), so when it is missing the relay runs itself
again under ~/.espressif/python_env/idf5.5_py3.13_env/bin/python; after
`. tools/idf-env.sh` that interpreter is first on PATH anyway. The self-test
needs no pyserial and runs under any python3.

It holds both ports while it runs, locked the way esptool and idf.py monitor
lock them, so stop it before flashing or monitoring either board. Left
running, it makes the flash scripts report that they "cannot read the MAC".
"""

import argparse
import datetime as dt
import errno
import glob
import os
import re
import select
import signal
import sys
import time
from collections import namedtuple

HERE = os.path.dirname(os.path.abspath(__file__))
BOARDS_TSV = os.path.join(HERE, "boards.tsv")

# The same two rows as tools/boards.tsv, for a copy of this script that has
# travelled without it. The self-test checks that the two agree.
SPEAKER_MAC = "28:84:85:56:f6:b0"
WATCH_MAC = "80:45:6b:35:11:d4"

IDF_PYTHON = os.path.expanduser("~/.espressif/python_env/idf5.5_py3.13_env/bin/python")

GAP_S = 1.0         # at most one line a second to watch
FRESH_S = 1.5       # a reading this old is not worth sending
SILENT_S = 5.0      # speaker connected but no noise line for this long
LOOK_S = 1.0        # how often to look for a board that is not there
HOLD_OFF_S = 5.0    # after watch stops taking lines, before trying again
TRUST_S = 60.0      # without a timeout, before watch is said to be taking lines again
STALL_S = 5.0       # a loop this late means the Mac slept, or we did
WRITE_TIMEOUT_S = 0.25
MAX_LINE = 4096     # a "line" longer than this is noise of the other kind

serial = None       # pyserial, imported by need_pyserial() when ports are wanted


# ---- the parser: speaker's line in, watch's line out -----------------------

Reading = namedtuple("Reading", "laf laeq3 today calibrated")

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

# A level is what speaker's lvl() prints, "%.1f". Where there is none it
# prints "--.-", or -- when the level is not finite -- nothing at all, which
# leaves "LAF  LAeq3 45.8". So a level is matched as a number that ends at a
# space or the end of the line, and anything else in its place means "none";
# a looser \S+ would read that empty LAF as "LAeq3".
LEVEL = r"(-?\d+(?:\.\d+)?|--(?:\.-)?)(?!\S)"
LAF = re.compile(r"\bLAF\s+" + LEVEL)
LAEQ3 = re.compile(r"\bLAeq3\s+" + LEVEL)
MODE = re.compile(r"\bdBA\s+\((est|cal)\)")
TODAY = re.compile(r"\btoday\s+" + LEVEL)


def level(m):
    if m is None or m.group(1).startswith("--"):
        return None
    return float(m.group(1))


def parse_noise(line):
    """speaker's serial line -> Reading, or None when it is not a noise line.

    The line arrives as ESP_LOG prints it, "I (123456) speaker: noise: ...",
    so "noise:" is looked for anywhere, as a word of its own. The levels come
    back as None where speaker had none. "today" is looked for anywhere after
    it and may be missing altogether: firmware from before it was added has no
    such field, and the day's figure is then simply unknown.
    """
    line = ANSI.sub("", line).strip()
    at = line.find("noise:")
    if at < 0 or (at > 0 and line[at - 1] not in " \t:"):
        return None
    rest = line[at + len("noise:"):]
    mode = MODE.search(rest)
    if mode is None or re.search(r"\bLAF\b", rest) is None:
        return None
    return Reading(laf=level(LAF.search(rest)), laeq3=level(LAEQ3.search(rest)),
                   today=level(TODAY.search(rest)), calibrated=mode.group(1) == "cal")


def command(r):
    """A Reading -> the line watch takes, or None when there is nothing to say.

    Without a level there is no line: watch's page would have to show some
    number for it, and "NO SIGNAL" after ten quiet seconds is the honest one.
    """
    if r is None or r.laf is None or r.laeq3 is None:
        return None
    today = "--" if r.today is None else "%.1f" % r.today
    return "!noise %.1f %.1f %s %s\n" % (r.laf, r.laeq3, today, "cal" if r.calibrated else "est")


class Lines:
    """Bytes in, complete lines out.

    The first fragment after a (re)connect is thrown away, because the port
    was opened in the middle of whatever the board was printing and the front
    of that line is gone. A run with no newline in it at all is dropped once
    it passes MAX_LINE rather than held forever.
    """

    def __init__(self):
        self.buf = b""
        self.skip = True

    def restart(self):
        self.buf = b""
        self.skip = True

    def feed(self, data):
        self.buf += data
        *done, self.buf = self.buf.split(b"\n")
        if len(self.buf) > MAX_LINE:
            self.buf = b""
            self.skip = True
        if done and self.skip:
            done = done[1:]
            self.skip = False
        return [d.decode("utf-8", "replace").rstrip("\r") for d in done]


class Pacer:
    """Newest wins, and no more than one line a second goes out.

    Each reading from speaker replaces the one waiting. It goes as soon as a
    second has passed since the last one went, and not at all once it is
    FRESH_S old. The pace follows speaker's own clock rather than a tick of
    ours: a fixed tick in step with speaker's lines would see each one arrive
    a hair before or a hair after it, and so skip one second in four for as
    long as the two clocks stay in step, which can be hours.
    """

    def __init__(self):
        self.pending = None
        self.pending_at = 0.0
        self.sent_at = float("-inf")

    def offer(self, cmd, now):
        self.pending, self.pending_at = cmd, now

    def forget(self):
        self.pending = None

    def due(self, now):
        if self.pending is None:
            return None
        if now - self.pending_at > FRESH_S:
            self.pending = None
            return None
        if now - self.sent_at < GAP_S:
            return None
        cmd, self.pending, self.sent_at = self.pending, None, now
        return cmd

    def wait(self, now, idle):
        """Seconds until due() could have something, at most idle."""
        if self.pending is None:
            return idle
        return max(0.0, min(idle, self.sent_at + GAP_S - now))


# ---- finding and opening the boards ----------------------------------------

def mac_key(s):
    return re.sub(r"[^0-9a-f]", "", (s or "").lower())


def find_port(mac, ports):
    """The device whose USB serial number is this MAC, or None.

    The OS reports it upper case with colons ("80:45:6B:35:11:D4") and
    boards.tsv has it lower case, so both are reduced to their hex digits.
    """
    want = mac_key(mac)
    found = sorted(p.device for p in ports if p.serial_number and mac_key(p.serial_number) == want)
    return found[0] if found else None


def registry(path=BOARDS_TSV):
    """name -> MAC from tools/boards.tsv; empty when it cannot be read."""
    macs = {}
    try:
        with open(path) as f:
            for row in f:
                if row.startswith("#") or not row.strip():
                    continue
                cols = row.rstrip("\n").split("\t")
                if len(cols) >= 2:
                    macs[cols[0]] = cols[1]
    except OSError:
        pass
    return macs


def need_pyserial():
    """Import pyserial, or run this script again under a Python that has it."""
    global serial
    try:
        import serial as s
        import serial.tools.list_ports  # noqa: F401  (a submodule, imported to be there)
        serial = s
        return
    except ImportError:
        pass
    if not os.environ.get("NOISE_RELAY_REEXEC"):
        pythons = [IDF_PYTHON] + sorted(glob.glob(os.path.expanduser(
            "~/.espressif/python_env/idf*_env/bin/python")), reverse=True)
        for py in pythons:
            if os.path.exists(py) and os.path.realpath(py) != os.path.realpath(sys.executable):
                os.environ["NOISE_RELAY_REEXEC"] = "1"
                os.execv(py, [py, os.path.abspath(__file__)] + sys.argv[1:])
    sys.exit("noise-relay: needs pyserial, which this Python (%s) does not have.\n"
             "Run it with %s, or `. tools/idf-env.sh` first." % (sys.executable, IDF_PYTHON))


def set_line(port, name, value):
    """Set DTR or RTS on an open port, shrugging off a port that has none
    (a pty, say), as pyserial's own open() does."""
    try:
        setattr(port, name, value)
    except OSError as e:
        if e.errno not in (errno.EINVAL, errno.ENOTTY):
            raise


def open_quietly(device):
    """Open a board's port without resetting it.

    The chip resets while it sees RTS held without DTR. macOS raises both
    lines as the port opens, and pyserial's open() then sets DTR and after it
    RTS to whatever they were set to beforehand. Set to False, as one might
    expect of "idle", that goes (1,1) -> DTR off -> (0,1): a reset, every time
    the relay connects. So both start True, which makes pyserial's pass no
    change at all whatever the OS did, and are then let go RTS first: (1,1) ->
    (1,0) -> (0,0). DTR alone only straps IO0, which matters to a chip coming
    out of reset and to nothing else. esp_idf_monitor opens the same way (its
    LOW is True). Once they are both off, closing the port changes nothing.

    exclusive=True takes the same lock esptool and idf.py monitor take, so
    each of them refuses a port the relay holds, and the relay refuses one
    they hold, instead of two programs taking turns at one stream of bytes.
    """
    port = serial.Serial()
    port.port = device
    port.baudrate = 115200          # USB-Serial-JTAG ignores it
    port.timeout = 0
    port.write_timeout = WRITE_TIMEOUT_S
    port.dsrdtr = False
    port.rtscts = False
    port.xonxoff = False
    port.exclusive = True
    port.dtr = True
    port.rts = True
    port.open()
    try:
        set_line(port, "rts", False)
        set_line(port, "dtr", False)
    except BaseException:
        port.close()
        raise
    return port


# ---- the relay --------------------------------------------------------------

def log(msg):
    print("%s %s" % (dt.datetime.now().strftime("%H:%M:%S"), msg), file=sys.stderr, flush=True)


class Board:
    """One board's port, and what has last been said about it.

    note() logs only when the state changes, so a relay left running says a
    handful of lines a day rather than one a second.
    """

    def __init__(self, name, mac, verbose):
        self.name, self.mac, self.verbose = name, mac, verbose
        self.port = None
        self.device = None
        self.lines = Lines()
        self.state = None
        self.next_look = 0.0

    def note(self, state, msg):
        if state != self.state:
            self.state = state
            log("%s: %s" % (self.name, msg))

    def connect(self, now):
        self.next_look = now + LOOK_S
        device = find_port(self.mac, serial.tools.list_ports.comports())
        if device is None:
            self.note("absent", "not plugged in (looking for %s)" % self.mac.upper())
            return False
        try:
            self.port = open_quietly(device)
        except (serial.SerialException, OSError) as e:
            self.note("unopenable " + str(e), "cannot open %s: %s" % (device, e))
            return False
        self.device = device
        self.lines.restart()
        self.note("open", "on %s" % device)
        return True

    def close(self):
        if self.port is not None:
            try:
                self.port.close()
            except (serial.SerialException, OSError):
                pass
        self.port = None

    def lost(self, why, now):
        self.close()
        self.next_look = now + LOOK_S
        self.note("lost", "gone from %s (%s)" % (self.device, why))

    def flush(self):
        if self.port is not None:
            try:
                self.port.reset_input_buffer()
            except (serial.SerialException, OSError):
                pass
        self.lines.restart()

    def read(self, now):
        try:
            data = self.port.read(self.port.in_waiting or 1)
        except (serial.SerialException, OSError) as e:
            self.lost(e, now)
            return []
        out = self.lines.feed(data)
        if self.verbose:
            for line in out:
                log("%s| %s" % (self.name, line))
        return out


class Speaker(Board):
    def __init__(self, mac, verbose):
        super().__init__("speaker", mac, verbose)
        self.heard_at = None

    def connect(self, now):
        if not super().connect(now):
            return False
        self.heard_at = now
        return True

    def take(self, lines, now, pacer):
        for line in lines:
            r = parse_noise(line)
            if r is None:
                continue
            self.heard_at = now
            cmd = command(r)
            if cmd is None:
                self.note("no level", "on the line but has no level yet")
                continue
            self.note("flowing", "levels coming in (%s)" % ("calibrated" if r.calibrated else "estimated"))
            pacer.offer(cmd, now)

    def check(self, now):
        if self.port is not None and self.heard_at is not None and now - self.heard_at > SILENT_S:
            self.note("silent", "connected, but no noise line for %.0f s" % SILENT_S)


class Watch(Board):
    def __init__(self, mac, verbose):
        super().__init__("watch", mac, verbose)
        self.hold_until = 0.0
        self.stuck_at = None
        self.torn = False

    def connect(self, now):
        if not super().connect(now):
            return False
        self.hold_until = 0.0
        self.stuck_at = None
        self.torn = False
        return True

    def send(self, cmd, now):
        """Write one line.

        A write that times out means watch is not reading its port: its
        firmware predates the Sound page, or it is stuck. What is still queued
        for it then is thrown away, because every line in that queue is old
        and would otherwise reach the page in a burst the moment watch reads
        again. Writes stop for HOLD_OFF_S rather than stalling the relay a
        quarter of a second on every line, and as the timed-out write may have
        left half a line behind, the next one starts with a newline to finish
        it off. A write going through proves only that there was room, not
        that watch read it, so "taking lines" is not said again until a minute
        has passed without a timeout. A watch that never reads then logs a
        pair of lines each time the Mac's queue for it fills up again, minutes
        apart, rather than every few seconds.
        """
        if self.port is None or now < self.hold_until:
            return
        data = ("\n" + cmd if self.torn else cmd).encode("ascii")
        try:
            self.port.write(data)
        except serial.SerialTimeoutException:
            try:
                self.port.reset_output_buffer()
            except (serial.SerialException, OSError):
                pass
            self.torn = True
            self.stuck_at = now
            self.hold_until = now + HOLD_OFF_S
            self.note("stuck", "not taking lines (firmware without the Sound page?)")
            return
        except (serial.SerialException, OSError) as e:
            self.lost(e, now)
            return
        self.torn = False
        if self.stuck_at is None or now - self.stuck_at >= TRUST_S:
            self.note("taking", "taking lines")


def run(args):
    need_pyserial()
    macs = registry()
    speaker = Speaker(args.speaker or macs.get("speaker", SPEAKER_MAC), args.verbose)
    watch = None if args.dry_run else Watch(args.watch or macs.get("watch", WATCH_MAC), args.verbose)
    boards = [b for b in (speaker, watch) if b is not None]
    pacer = Pacer()

    def stop(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    log("relaying speaker %s -> %s" % (speaker.mac.upper(),
                                       "stdout (dry run)" if watch is None else "watch " + watch.mac.upper()))
    last_wall = time.time()
    try:
        while True:
            now = time.monotonic()
            for b in boards:
                if b.port is None and now >= b.next_look:
                    b.connect(now)

            # Wait for either board to say something, or for the waiting
            # reading's second to be up. watch's own output is read only to
            # be thrown away: left unread it would back up into its console.
            open_ports = [b for b in boards if b.port is not None]
            timeout = pacer.wait(now, 0.5)
            if open_ports:
                ready, _, _ = select.select([b.port for b in open_ports], [], [], timeout)
            else:
                time.sleep(timeout)
                ready = []
            now = time.monotonic()

            # The Mac asleep, or this process stopped: whatever queued up in
            # the ports meanwhile is old, and none of it may reach the page.
            # The wall clock is the one that runs on through a sleep, and it
            # is asked here, between the wait and the reading, because the
            # first thing the wait returns after a wake is that very backlog.
            wall = time.time()
            if wall - last_wall > STALL_S:
                log("nothing for %.0f s (the Mac slept?); dropping what queued up" % (wall - last_wall))
                for b in boards:
                    b.flush()
                pacer.forget()
                speaker.heard_at = now
                ready = []
            last_wall = wall

            for b in open_ports:
                if b.port in ready:
                    lines = b.read(now)
                    if b is speaker:
                        speaker.take(lines, now, pacer)
            speaker.check(now)

            cmd = pacer.due(now)
            if cmd is not None:
                if watch is None:
                    print(cmd.rstrip("\n"), flush=True)
                else:
                    watch.send(cmd, now)
    except KeyboardInterrupt:
        pass
    finally:
        for b in boards:
            b.close()
    log("stopped")
    return 0


# ---- the self-test ------------------------------------------------------------

LINE = ("noise: LAF 47.1 LAeq3 45.8 dBA (est) LAeq1 46.2 | red 0s | "
        "19:59:24 ring on chime off sd ok")


def self_test():
    fails = []
    count = [0]

    def check(what, got, want):
        count[0] += 1
        if got != want:
            fails.append("%s: got %r, want %r" % (what, got, want))

    def relayed(line):
        return command(parse_noise(line))

    # speaker's line, as ESP_LOG prints it and as it is written today.
    check("bare, today absent", relayed(LINE), "!noise 47.1 45.8 -- est\n")
    check("log prefix", relayed("I (123456) speaker: " + LINE), "!noise 47.1 45.8 -- est\n")
    check("today at the end", relayed(LINE + " today 46.9"), "!noise 47.1 45.8 46.9 est\n")
    check("today unknown", relayed(LINE + " today --"), "!noise 47.1 45.8 -- est\n")
    check("today --.-", relayed(LINE + " today --.-"), "!noise 47.1 45.8 -- est\n")
    check("today empty", relayed(LINE + " today "), "!noise 47.1 45.8 -- est\n")
    check("today mid-line", relayed(LINE.replace("ring on", "today 52.0 ring on")),
          "!noise 47.1 45.8 52.0 est\n")
    check("calibrated", relayed(LINE.replace("(est)", "(cal)") + " today 44.0"),
          "!noise 47.1 45.8 44.0 cal\n")
    check("CRLF", relayed("I (1) speaker: " + LINE + " today 46.9\r"), "!noise 47.1 45.8 46.9 est\n")
    check("ANSI colour", relayed("\x1b[0;32mI (1) speaker: " + LINE + " today 46.9\x1b[0m"),
          "!noise 47.1 45.8 46.9 est\n")
    check("gated, night, no date",
          relayed("I (9) speaker: noise: LAF 80.4 LAeq3 78.0 dBA (cal) LAeq1 79.1 | red 12s GATED | "
                  "02:10:00 (no date) night ring off chime off sd lost today 61.3"),
          "!noise 80.4 78.0 61.3 cal\n")
    check("quiet room", relayed("noise: LAF 9.0 LAeq3 10.5 dBA (est) LAeq1 10.0 | red 0s"),
          "!noise 9.0 10.5 -- est\n")

    # Lines that are speaker's but carry no level: parsed, and not sent.
    none = parse_noise("noise: LAF --.- LAeq3 --.- dBA (est) LAeq1 --.- | red 0s | --:--:-- ring on")
    check("no level parses", none, Reading(None, None, None, False))
    check("no level sends nothing", command(none), None)
    empty = parse_noise("noise: LAF  LAeq3 45.8 dBA (est) LAeq1 46.2 | red 0s")
    check("empty LAF is no LAF", empty, Reading(None, 45.8, None, False))
    check("empty LAF sends nothing", command(empty), None)
    check("empty LAeq3", relayed("noise: LAF 45.0 LAeq3  dBA (est) LAeq1 46.2"), None)

    # And lines that are not speaker's noise line at all.
    check("status line", parse_noise("I (5) speaker: status: now LAF 47.1 LAeq3 45.8 dBA (est)"), None)
    check("word inside a word", parse_noise("I (5) speaker: xnoise: LAF 47.1 LAeq3 45.8 dBA (est)"), None)
    check("no mode", parse_noise("noise: LAF 47.1 LAeq3 45.8 LAeq1 46.2"), None)
    check("garbage", parse_noise("\x00\xff ets Jun  8 2016 00:22:57"), None)
    check("empty", parse_noise(""), None)
    check("None reading", command(None), None)

    # Lines from bytes: split anywhere, first fragment dropped, runaway dropped.
    lb = Lines()
    check("first fragment", lb.feed(b"e: LAF 1\nI (1) a"), [])
    check("split line", lb.feed(b"bc\r\nnext"), ["I (1) abc"])
    check("held tail", lb.feed(b" one\n"), ["next one"])
    check("bad utf-8", lb.feed(b"\xffok\n"), ["�ok"])
    lb.feed(b"x" * (MAX_LINE + 1))
    check("runaway dropped", lb.feed(b"tail\nnew\n"), ["new"])

    # Pacing, on a clock of our own.
    p = Pacer()
    p.offer("a", 0.0)
    check("first goes at once", p.due(0.0), "a")
    p.offer("b", 0.999)
    check("not within a second", p.due(0.999), None)
    check("waits the rest", round(p.wait(0.999, 0.5), 3), 0.001)
    check("then goes", p.due(1.0), "b")
    check("once only", p.due(1.2), None)
    check("idle wait", p.wait(1.2, 0.5), 0.5)
    for t, c in ((2.0, "c"), (2.0, "d"), (2.0, "e")):
        p.offer(c, t)
    check("newest wins", p.due(2.0), "e")
    p.offer("f", 2.1)
    check("stale is dropped", p.due(3.7), None)
    check("and stays dropped", p.due(3.8), None)
    p.offer("g", 4.0)
    p.forget()
    check("forget", p.due(5.0), None)

    # Finding a board by MAC, never by name.
    Port = namedtuple("Port", "device serial_number")
    ports = [Port("/dev/cu.debug-console", None),
             Port("/dev/cu.usbmodem1101", "28:84:85:56:F6:B0"),
             Port("/dev/cu.usbmodem101", "80:45:6B:35:11:D4")]
    check("speaker by MAC", find_port(SPEAKER_MAC, ports), "/dev/cu.usbmodem1101")
    check("watch by MAC", find_port(WATCH_MAC.upper(), ports), "/dev/cu.usbmodem101")
    check("absent", find_port("aa:bb:cc:dd:ee:ff", ports), None)

    # The built-in MACs are the registry's, while there is a registry.
    macs = registry()
    if macs:
        check("speaker in boards.tsv", mac_key(macs.get("speaker")), mac_key(SPEAKER_MAC))
        check("watch in boards.tsv", mac_key(macs.get("watch")), mac_key(WATCH_MAC))

    for f in fails:
        print("FAIL " + f)
    print("self-test: %d of %d checks passed" % (count[0] - len(fails), count[0]))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description="Relay speaker's noise level to watch over USB.")
    ap.add_argument("--dry-run", action="store_true",
                    help="read speaker and print what would be sent; watch is not opened")
    ap.add_argument("--verbose", action="store_true", help="echo both boards' serial output")
    ap.add_argument("--self-test", action="store_true", help="check the parser and pacing, then exit")
    ap.add_argument("--speaker", metavar="MAC", help="speaker's MAC, instead of boards.tsv's")
    ap.add_argument("--watch", metavar="MAC", help="watch's MAC, instead of boards.tsv's")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    try:
        return run(args)
    except BrokenPipeError:
        return 0        # --dry-run into head, say


if __name__ == "__main__":
    sys.exit(main())
