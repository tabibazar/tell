#!/usr/bin/env python3
"""Carry speaker's noise level to watch's Sound page, over USB; and carry the
Mac's commands and speech to speaker.

    tools/noise-relay.py                  relay until Ctrl-C
    tools/noise-relay.py --dry-run        read speaker, print what watch would get
    tools/noise-relay.py --verbose        also echo both boards' serial output
    tools/noise-relay.py --self-test      check the parser, pacing and socket; no ports

speaker prints a line a second on its USB serial ("noise: LAF 47.1 LAeq3 45.8
dBA (est) LAeq1 46.2 | red 0s | 19:59:24 ring on chime off sd ok today 46.9").
The boards never talk to each other -- a house rule, not a limitation -- so
the Mac carries it: this reads that line, keeps the numbers, and writes
"!noise <laf> <laeq3> <today|--> <est|cal>" to watch, no more than once a
second. watch hands the line to the same handler its BLE messages go to.
A line with no level in it is not passed on at all, and neither is anything
old: when nothing arrives, watch says NO SIGNAL, which is the truth.

Once a minute, and straight after a !cal, speaker also prints its day-by-day
history, "days: <cal|est> 2026-09-24=49.8/36.0 2026-09-25=52.4/38.1*", up to
35 days of LAeq/L90 ending today. watch's USB line is at most 127 characters,
so the set goes as "!noisedays <k>/<n> <cal|est> <e1> .. <e4>", four days a
line, the lines spaced out so the 1 Hz noise line is never held up behind
them. watch merges them by date.

The other way, the relay is the one program on the Mac that holds speaker's
port, so anything else that wants speaker goes through it: a Unix socket at
~/.tell/speaker.sock takes one request per connection --

    PLAY <nbytes> 16000 <normal|test>   and then the PCM (s16le, mono)
    CMD <!cal NN | !chime on|off | !ring on|off | !night HH-HH>
    STATUS

-- and answers in lines ending with "END". Jobs run one at a time, in the
order they came: a PLAY goes to speaker as "!play <nbytes> 16000 <mode>" and
the bytes straight after it, written in pieces between reads so speaker's own
lines are never left waiting; the answer is speaker's "play: ..." lines. A
CMD goes as its one line, and the answer is what speaker logs about it.
tools/speak and tools/cal-speaker are the clients.

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
state; open_quietly() says how. Writing to the port touches neither line.

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
import socket
import stat
import sys
import time
from collections import deque, namedtuple

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
CLOCK_EVERY_S = 120.0   # at most one plug-in clock push per board in this long
HOLD_OFF_S = 5.0    # after watch stops taking lines, before trying again
TRUST_S = 60.0      # without a timeout, before watch is said to be taking lines again
STALL_S = 5.0       # a loop this late means the Mac slept, or we did
WRITE_TIMEOUT_S = 0.25
MAX_LINE = 4096     # a "line" longer than this is noise of the other kind

TAG = "speaker"     # speaker's ESP_LOG tag
DAYS_MAX = 35       # days speaker keeps, and the most a set is taken to hold
DAYS_PER_LINE = 4
WATCH_LINE = 127    # watch reads into char line[128]; longer lines are dropped whole
DAYS_GAP_S = 0.05   # between one !noisedays line and the next
DAYS_AGAIN_S = 3.0  # after watch turns up, before the latest set goes to it again

# $SPEAKER_SOCK moves it, for trying a relay out beside the real one.
SOCK_PATH = os.environ.get("SPEAKER_SOCK") or os.path.expanduser("~/.tell/speaker.sock")
PLAY_RATE = 16000
PLAY_MAX = 960000   # 30 s of 16-bit mono at 16 kHz
CHUNK = 4096        # PCM bytes per write to speaker
PACE_BPS = 192000   # bytes a second to speaker: six times as fast as it plays
PACE_BURST = 8192   # and ahead of that pace, at most; under speaker's 12 kB ring
PLAY_LISTEN_S = 5.0     # after the last byte, how long speaker's play: lines are waited for
CMD_LISTEN_S = 3.0      # after a command, how long its answer is waited for
CMD_GRACE_S = 0.2       # after the first line of the answer, for a second
WRITE_STALL_S = 5.0     # speaker taking no bytes for this long ends the job
CLIENT_IDLE_S = 10.0    # a client this long without sending what it said it would
HEADER_MAX = 256
JOBS_MAX = 4        # running and waiting, together

serial = None       # pyserial, imported by need_pyserial() when ports are wanted
QUIET = False       # the self-test keeps log() to itself


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


# ---- speaker's other lines: its log messages, and the days ------------------

LOG_LINE = re.compile(r"^[EWIDV] \([^)]*\) ([^:\s]+): (.*)$")


def speaker_message(line):
    """speaker's serial line -> the message in it, or None.

    "I (123) speaker: cal: the room is ..." gives "cal: the room is ...". A
    line with another tag's prefix is some driver talking and gives None; a
    line with no prefix at all is taken as it stands, as parse_noise() takes
    it, so the self-test and a firmware built without log prefixes both work.
    """
    line = ANSI.sub("", line).strip()
    m = LOG_LINE.match(line)
    if m is None:
        return line or None
    return m.group(2).strip() if m.group(1) == TAG else None


def first_word(msg):
    """"cal: the room ..." -> "cal"; "chime on (never ...)" -> "chime".

    speaker's answers to !chime and !ring have no colon ("chime on"), while
    its complaint about them does ("chime: say on or off"), so the colon is
    not what makes a word.
    """
    parts = (msg or "").split(None, 1)
    return parts[0].rstrip(":") if parts else ""


Day = namedtuple("Day", "date laeq l90 partial")

DAY_LEVEL = r"(--(?:\.-)?|\d{1,3}(?:\.\d+)?)"
DAY_ENTRY = re.compile(r"^(\d{4}-\d\d-\d\d)=" + DAY_LEVEL + r"(?:/" + DAY_LEVEL + r")?(\*?)$")


def day_level(s):
    if s is None or s.startswith("--"):
        return None
    v = float(s)
    return v if 0.0 <= v <= 150.0 else None


def parse_days(msg):
    """speaker's "days: ..." message -> (calibrated, [Day]), or None.

    Entries come oldest first, and the one with a "*" is today, still
    running. One that does not parse -- a torn line, a date that is not one
    -- is left out rather than sinking the rest. Should a date come twice,
    the later wins; the set goes out in date order, at most DAYS_MAX of the
    newest.
    """
    if msg is None or not msg.startswith("days:"):
        return None
    words = msg[len("days:"):].split()
    if not words or words[0] not in ("cal", "est"):
        return None
    by_date = {}
    for w in words[1:]:
        m = DAY_ENTRY.match(w)
        if m is None:
            continue
        try:
            dt.date.fromisoformat(m.group(1))
        except ValueError:
            continue
        by_date[m.group(1)] = Day(m.group(1), day_level(m.group(2)), day_level(m.group(3)),
                                  m.group(4) == "*")
    days = [by_date[d] for d in sorted(by_date)][-DAYS_MAX:]
    return words[0] == "cal", days


def day_entry(d):
    """A Day -> its entry, the way speaker writes one."""
    if d.laeq is None and d.l90 is None:
        text = "%s=--" % d.date
    else:
        text = "%s=%s/%s" % (d.date, "--" if d.laeq is None else "%.1f" % d.laeq,
                             "--" if d.l90 is None else "%.1f" % d.l90)
    return text + ("*" if d.partial else "")


def days_commands(calibrated, days):
    """The days -> watch's "!noisedays k/n <cal|est> e1 .. e4" lines, in order."""
    entries = [day_entry(d) for d in days]
    groups = [entries[i:i + DAYS_PER_LINE] for i in range(0, len(entries), DAYS_PER_LINE)]
    mode = "cal" if calibrated else "est"
    return ["!noisedays %d/%d %s %s\n" % (k + 1, len(groups), mode, " ".join(g))
            for k, g in enumerate(groups)]


class DaysOut:
    """The latest days set, going out a line at a time.

    Nine lines at once would be a thousand bytes into watch's 1024-byte USB
    buffer, on top of whatever else is there, so they are spaced DAYS_GAP_S
    apart; the noise line, being paced on its own, goes between them as it
    falls due. A new set replaces whatever of the last one has not gone yet
    (the new one holds all of it anyway), and again() sends the latest set
    once more, for a watch that has just been plugged in.
    """

    def __init__(self):
        self.lines = []
        self.queue = deque()
        self.next_at = 0.0

    def set(self, lines):
        self.lines = list(lines)
        self.queue = deque(self.lines)

    def again(self, at):
        if self.lines:
            self.queue = deque(self.lines)
            self.next_at = max(self.next_at, at)

    def due(self, now):
        if not self.queue or now < self.next_at:
            return None
        self.next_at = now + DAYS_GAP_S
        return self.queue.popleft()

    def wait(self, now, idle):
        if not self.queue:
            return idle
        return max(0.0, min(idle, self.next_at - now))


# ---- requests on the socket ---------------------------------------------------

def check_command(text):
    """A command line for speaker -> the line as sent, or ValueError saying why not.

    Only the four that change a setting are carried, each in exactly the
    form speaker's handler takes, so nothing arrives there that it would
    have to guess at. !status is not among them: STATUS answers from what
    speaker has already said.
    """
    if any(ord(ch) < 32 or ord(ch) > 126 for ch in text):
        raise ValueError("a command is one line of plain text")
    parts = text.split()
    if not parts:
        raise ValueError("no command")
    word, args = parts[0], parts[1:]
    if word == "!cal":
        if len(args) != 1 or not re.match(r"^\d{1,3}(?:\.\d+)?$", args[0]):
            raise ValueError('say "!cal NN", the room\'s level in dBA now')
        if not 10.0 <= float(args[0]) <= 130.0:
            raise ValueError("%s dBA is not a room (10-130)" % args[0])
    elif word in ("!chime", "!ring"):
        if args not in (["on"], ["off"]):
            raise ValueError('say "%s on" or "%s off"' % (word, word))
    elif word == "!night":
        m = re.match(r"^(\d{1,2})-(\d{1,2})$", args[0]) if len(args) == 1 else None
        if m is None or int(m.group(1)) > 23 or int(m.group(2)) > 23:
            raise ValueError('say "!night HH-HH", e.g. !night 22-07')
    else:
        raise ValueError("%s is not carried (only !cal, !chime, !ring, !night)" % word[:20])
    return " ".join(parts)


def parse_request(line):
    """A request's first line -> ("PLAY", nbytes, mode) | ("CMD", line) | ("STATUS",),
    or ValueError with what goes after "ERR"."""
    try:
        text = line.decode("ascii").rstrip("\r")
    except UnicodeDecodeError:
        raise ValueError("bad header (not ASCII)")
    word, _, rest = text.partition(" ")
    if word == "STATUS" and not rest.strip():
        return ("STATUS",)
    if word == "CMD":
        return ("CMD", check_command(rest))
    if word == "PLAY":
        parts = rest.split()
        if len(parts) != 3 or not parts[0].isdigit() or not parts[1].isdigit():
            raise ValueError("bad header (PLAY <nbytes> <rate> <normal|test>)")
        n, rate, mode = int(parts[0]), int(parts[1]), parts[2]
        if rate != PLAY_RATE:
            raise ValueError("bad header (rate %d; speaker takes %d only)" % (rate, PLAY_RATE))
        if mode not in ("normal", "test"):
            raise ValueError("bad header (mode %s; normal or test)" % mode[:20])
        if n <= 0 or n % 2:
            raise ValueError("bad header (nbytes %d; 16-bit samples, so even and not 0)" % n)
        if n > PLAY_MAX:
            raise ValueError("too long (%.1f s; at most %d s)" % (n / 2.0 / PLAY_RATE, PLAY_MAX // 2 // PLAY_RATE))
        return ("PLAY", n, mode)
    raise ValueError("unknown request %r (PLAY, CMD or STATUS)" % word[:20])


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


# ---- the boards -----------------------------------------------------------

def log(msg):
    if QUIET:
        return
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
        self.clock_at = -1e9     # when this board's clock was last pushed

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
        self.push_clock(now)
        return True

    def push_clock(self, now):
        """Set the board's clock over BLE the moment it turns up on USB.

        The boards travel between home and work and their clock chips have
        flat backup cells, so one that lost power on the way arrives with no
        time: watch shows NO TIME and speaker logs nothing until a Mac says
        what time it is. The 5-minute push-clock agent would get there; this
        gets there as soon as the board is plugged in. Eight seconds' grace for
        it to boot and advertise, at most once in two minutes per board, and
        never waited on -- tools/push-clock.sh goes through tell-locked, so it
        cannot collide with the agent's own push.
        """
        if now - self.clock_at < CLOCK_EVERY_S:
            return
        self.clock_at = now
        script = os.path.join(HERE, "push-clock.sh")
        if not os.access(script, os.X_OK):
            return
        import subprocess
        try:
            subprocess.Popen(["/bin/sh", "-c", 'sleep 8; exec "$0" "$1"', script, self.name],
                             stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
            log("%s: pushing its clock (it may have lost it on the way)" % self.name)
        except OSError as e:
            log("%s: could not start push-clock: %s" % (self.name, e))

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
        self.torn = False       # a job ended part-way through what it was writing

    def connect(self, now):
        if not super().connect(now):
            return False
        self.heard_at = now
        self.torn = False
        return True

    def write_some(self, data, now):
        """As much of data as the port takes now, without waiting: the count
        written, 0 when it is full, None when the board has gone.

        Straight to the descriptor, which pyserial opens non-blocking, rather
        than through its write(): that waits for the whole of it, up to the
        write timeout, and the relay has watch to serve meanwhile.
        """
        if self.port is None:
            return None
        try:
            return os.write(self.port.fileno(), data)
        except BlockingIOError:
            return 0
        except OSError as e:
            self.lost(e, now)
            return None

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


# ---- jobs for speaker, and the socket they come in on ----------------------

PLAY_END = re.compile(r"^play: (done|refused|timeout)\b")
PLAY_STARTED = re.compile(r"^play: started \((\d+) bytes")


class Job:
    """One thing for speaker: the bytes to write, and whose answer to wait for.

    It is "waiting" in the queue, "writing" while its bytes go, and
    "listening" from the last byte until `until`; then its client gets END.
    """

    def __init__(self, kind, client, data, word, listen_s):
        self.kind, self.client, self.data = kind, client, data
        self.word, self.listen_s = word, listen_s
        self.state = "waiting"
        self.sent = 0
        self.began = 0.0
        self.progress_at = 0.0
        self.until = 0.0
        self.over = False       # speaker has said its last word on it


class Jobs:
    """The jobs, one at a time, in the order they came.

    The bytes go at PACE_BPS, not as fast as the Mac can write them. speaker's
    USB driver has no flow control: its interrupt moves each packet into a
    12 kB ring and drops the packet when the ring is full, so a reader held
    up for a few ticks would lose part of a clip written flat out, and every
    byte after it would be counted one clip too early. At 192 kB/s the ring
    holds 60 ms of it, six ticks of speaker's 100 Hz scheduler, and a full
    30 s clip still goes in five seconds.

    A PLAY waits while speaker is still playing the last one -- its "play:
    started" says how many bytes, which says how long -- so a queued second
    utterance follows the first instead of being refused as busy. "play:
    done", or refused or timeout, lifts that at once; the estimate, plus two
    seconds, lifts it anyway should the line be lost.
    """

    def __init__(self):
        self.queue = deque()
        self.active = None
        self.play_until = 0.0

    def count(self):
        return len(self.queue) + (self.active is not None)

    def add(self, job):
        self.queue.append(job)

    @staticmethod
    def allowance(j, now):
        """Bytes the pace lets go now."""
        return int(PACE_BURST + (now - j.began) * PACE_BPS) - j.sent

    def wants_write(self, now):
        j = self.active
        return j is not None and j.state == "writing" and self.allowance(j, now) > 0

    def start(self, now, speaker):
        while self.active is None and self.queue:
            j = self.queue[0]
            if j.client.gone:           # hung up while it waited: never sent
                self.queue.popleft()
                continue
            if j.kind == "PLAY" and now < self.play_until:
                return
            self.queue.popleft()
            if speaker.port is None:
                j.client.reply("ERR speaker not connected")
                j.client.end()
                continue
            head = j.data.split(b"\n", 1)[0].decode("ascii")
            if j.kind == "PLAY":
                n, mode = int(head.split()[1]), head.split()[3]
                log("job: play %.1f s (%s)" % (n / 2.0 / PLAY_RATE, mode))
            else:
                log("job: %s" % head)
            if speaker.torn:            # finish off whatever the last job left half-written
                j.data = b"\n" + j.data
                speaker.torn = False
            j.state, j.began, j.progress_at = "writing", now, now
            self.active = j

    def step(self, now, speaker, writable):
        self.start(now, speaker)
        j = self.active
        if j is None:
            return
        if j.state == "writing":
            room = min(CHUNK, self.allowance(j, now))
            n = speaker.write_some(j.data[j.sent:j.sent + room], now) if writable and room > 0 else 0
            if n is None or speaker.port is None:
                j.client.reply("ERR speaker not connected (gone after %d of %d bytes)" % (j.sent, len(j.data)))
                self.finish(j)
                return
            if n:
                j.sent += n
                j.progress_at = now
            if j.sent >= len(j.data):
                j.state = "listening"
                j.until = now + (0.0 if j.over else j.listen_s)
                if j.kind == "CMD":
                    j.client.reply("OK sent")
            elif now - j.progress_at > WRITE_STALL_S:
                speaker.torn = True
                j.client.reply("ERR speaker stopped taking bytes (%d of %d sent)" % (j.sent, len(j.data)))
                log("speaker: took no bytes for %.0f s; job dropped" % WRITE_STALL_S)
                self.finish(j)
                return
        if j.state == "listening" and now >= j.until:
            self.finish(j)

    def finish(self, j):
        j.client.end()
        if self.active is j:
            self.active = None

    def heard(self, msg, now):
        """A message from speaker: the answer to the job, perhaps."""
        word = first_word(msg)
        if word == "play":
            m = PLAY_STARTED.match(msg)
            if m:
                self.play_until = now + int(m.group(1)) / 2.0 / PLAY_RATE + 2.0
            elif PLAY_END.match(msg):
                self.play_until = 0.0
        j = self.active
        if j is None or word != j.word:
            return
        if j.kind == "PLAY":
            j.client.reply(msg)
            if PLAY_END.match(msg):
                j.over = True
                if j.state == "listening":
                    j.until = now
        elif j.state == "listening":
            j.client.reply(msg)
            j.until = min(j.until, now + CMD_GRACE_S)

    def wait(self, now, idle):
        j = self.active
        if j is not None:
            if j.state == "writing":
                stall = j.progress_at + WRITE_STALL_S - now
                if self.allowance(j, now) > 0:     # select wakes when speaker can take them
                    return max(0.0, min(idle, stall))
                ahead = (j.sent + 512 - PACE_BURST) / float(PACE_BPS) + j.began - now
                return max(0.001, min(idle, stall, ahead))
            return max(0.0, min(idle, j.until - now))
        if self.queue and self.queue[0].kind == "PLAY":
            return max(0.0, min(idle, self.play_until - now))
        return idle


class Client:
    """One connection on the socket: the request coming in, the answer going out."""

    def __init__(self, sock, now):
        sock.setblocking(False)
        self.sock = sock
        self.inbuf = bytearray()
        self.out = bytearray()
        self.req = None
        self.queued = False
        self.done = False       # END is in `out`; close once it has gone
        self.gone = False       # hung up, or the socket failed
        self.seen_at = now

    def reply(self, text):
        if not self.done and not self.gone:
            self.out += (text + "\n").encode("utf-8", "replace")

    def end(self):
        self.reply("END")
        self.done = True

    def send(self):
        try:
            n = self.sock.send(self.out)
        except BlockingIOError:
            return
        except OSError:
            self.gone = True
            return
        del self.out[:n]

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


class Server:
    """The listening socket, ~/.tell/speaker.sock, and its clients.

    The directory is the user's own (0700) and so is the socket (0600): the
    speaker in the room is not for other accounts on the Mac to play through.
    A socket file left by a relay that died is replaced; one that a relay
    still answers on is left to it, and this one serves no socket.
    """

    def __init__(self, path=SOCK_PATH):
        self.path = path
        self.sock = None
        self.ino = None
        self.clients = []

    def open(self):
        d = os.path.dirname(self.path)
        try:
            os.makedirs(d, mode=0o700, exist_ok=True)
            os.chmod(d, 0o700)
            if os.path.lexists(self.path):
                if stat.S_ISSOCK(os.lstat(self.path).st_mode) and self.answers():
                    log("socket: another relay answers on %s; not serving it" % self.path)
                    return False
                os.unlink(self.path)
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            old = os.umask(0o177)
            try:
                s.bind(self.path)
            finally:
                os.umask(old)
            os.chmod(self.path, 0o600)
            s.listen(8)
            s.setblocking(False)
        except OSError as e:
            log("socket: cannot serve %s: %s" % (self.path, e))
            return False
        self.sock = s
        self.ino = os.lstat(self.path).st_ino
        log("socket: serving %s" % self.path)
        return True

    def answers(self):
        probe = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        probe.settimeout(0.5)
        try:
            probe.connect(self.path)
            return True
        except OSError:
            return False
        finally:
            probe.close()

    def close(self):
        for c in self.clients:
            c.close()
        self.clients = []
        if self.sock is not None:
            self.sock.close()
            self.sock = None
            try:
                if os.lstat(self.path).st_ino == self.ino:
                    os.unlink(self.path)
            except OSError:
                pass

    def rfds(self):
        if self.sock is None:
            return []
        return [self.sock] + [c.sock for c in self.clients if not c.gone]

    def wfds(self):
        return [c.sock for c in self.clients if c.out and not c.gone]

    def accept(self, now):
        while True:
            try:
                sock, _ = self.sock.accept()
            except (BlockingIOError, InterruptedError):
                return
            except OSError as e:
                log("socket: accept failed: %s" % e)
                return
            self.clients.append(Client(sock, now))

    def reap(self):
        keep = []
        for c in self.clients:
            if c.gone or (c.done and not c.out):
                c.close()
            else:
                keep.append(c)
        self.clients = keep


# ---- the relay --------------------------------------------------------------

class Relay:
    """Both boards, the socket, and one turn of the loop at a time (step())."""

    def __init__(self, speaker, watch, server=None):
        self.speaker, self.watch, self.server = speaker, watch, server
        self.boards = [b for b in (speaker, watch) if b is not None]
        self.pacer = Pacer()
        self.days = DaysOut()
        self.jobs = Jobs()
        self.seen = {}          # "noise", "days", "cal" -> (message, when)
        self.status = []        # the last !status block, [(message, when)]
        self.last_wall = time.time()

    # -- speaker's lines --

    def on_speaker(self, lines, now):
        self.speaker.take(lines, now, self.pacer)
        for line in lines:
            msg = speaker_message(line)
            if msg is None:
                continue
            word = first_word(msg)
            if word == "noise" and parse_noise(line) is not None:
                self.seen["noise"] = (msg, now)
            elif word == "days":
                got = parse_days(msg)
                if got is not None:
                    self.seen["days"] = (msg, now)
                    if got[1]:
                        self.days.set(days_commands(*got))
            elif word in ("cal", "play", "status"):
                if word == "cal":
                    self.seen["cal"] = (msg, now)
                if word == "status":
                    if not self.status or now - self.status[-1][1] > 2.0:
                        self.status = []
                    self.status = (self.status + [(msg, now)])[-8:]
                if not self.speaker.verbose:
                    log("speaker| %s" % msg)
            self.jobs.heard(msg, now)

    # -- the socket --

    def on_client(self, c, now):
        if c.req is None:
            nl = c.inbuf.find(b"\n")
            if nl < 0:
                if len(c.inbuf) > HEADER_MAX:
                    c.reply("ERR bad header (no newline in %d bytes)" % HEADER_MAX)
                    c.end()
                return
            line = bytes(c.inbuf[:nl])
            del c.inbuf[:nl + 1]
            try:
                c.req = parse_request(line)
            except ValueError as e:
                c.req = ("BAD",)
                c.reply("ERR %s" % e)
                c.end()
                return
            if c.req[0] == "STATUS":
                for text in self.status_lines(now):
                    c.reply(text)
                c.end()
                return
            if self.speaker.port is None:
                c.reply("ERR speaker not connected")
                c.end()
                return
            if self.jobs.count() >= JOBS_MAX:
                c.reply("ERR busy (%d jobs ahead)" % self.jobs.count())
                c.end()
                return
            if c.req[0] == "CMD":
                text = c.req[1]
                self.jobs.add(Job("CMD", c, (text + "\n").encode("ascii"), first_word(text[1:]), CMD_LISTEN_S))
                c.queued = True
        if c.req[0] == "PLAY" and not c.queued:
            n, mode = c.req[1], c.req[2]
            if len(c.inbuf) >= n:
                pcm = bytes(c.inbuf[:n])
                data = ("!play %d %d %s\n" % (n, PLAY_RATE, mode)).encode("ascii") + pcm
                self.jobs.add(Job("PLAY", c, data, "play", PLAY_LISTEN_S))
                c.queued = True
                c.reply("OK queued")
        if c.queued or c.done:
            c.inbuf.clear()         # anything after the request is not ours to read

    def status_lines(self, now):
        out = []
        for b, name in ((self.speaker, "speaker"), (self.watch, "watch")):
            if b is None:
                out.append("%s: not opened" % name)
            elif b.port is None:
                out.append("%s: not connected" % name)
            else:
                out.append("%s: connected (%s)" % (name, b.device))
        j = self.jobs.active
        out.append("jobs: %s, %d waiting" % (
            "idle" if j is None else "%s %s (%d of %d bytes)" % (j.kind, j.state, j.sent, len(j.data)),
            len(self.jobs.queue)))
        for key in ("noise", "days", "cal"):
            if key in self.seen:
                msg, at = self.seen[key]
                out.append("%d %s" % (now - at, msg))
        for msg, at in self.status:
            out.append("%d %s" % (now - at, msg))
        return out

    def serve(self, rd, wr, now):
        srv = self.server
        if srv is None or srv.sock is None:
            return
        if srv.sock in rd:
            srv.accept(now)
        for c in list(srv.clients):
            if c.sock in rd and not c.gone:
                try:
                    data = c.sock.recv(65536)
                except BlockingIOError:
                    data = None
                except OSError:
                    data = b""
                if data == b"":
                    c.gone = True
                elif data:
                    c.seen_at = now
                    if not (c.queued or c.done):
                        c.inbuf += data
                        self.on_client(c, now)
            if not (c.queued or c.done or c.gone) and now - c.seen_at > CLIENT_IDLE_S:
                if c.req is None:
                    c.reply("ERR bad header (none in %.0f s)" % CLIENT_IDLE_S)
                else:
                    c.reply("ERR timeout (%d of %d bytes in)" % (len(c.inbuf), c.req[1]))
                c.end()
            if c.out:
                c.send()
        srv.reap()

    # -- one turn --

    def step(self, idle=0.5):
        now = time.monotonic()
        sp, watch = self.speaker, self.watch
        for b in self.boards:
            if b.port is None and now >= b.next_look:
                if b.connect(now) and b is watch:
                    self.days.again(now + DAYS_AGAIN_S)
        self.jobs.step(now, sp, False)

        # Wait for either board to say something, for the waiting reading's
        # second to be up, for a client, or for speaker to take more bytes.
        # watch's own output is read only to be thrown away: left unread it
        # would back up into its console.
        open_ports = [b for b in self.boards if b.port is not None]
        rfds = [b.port for b in open_ports] + (self.server.rfds() if self.server else [])
        wfds = self.server.wfds() if self.server else []
        if self.jobs.wants_write(now) and sp.port is not None:
            wfds.append(sp.port)
        timeout = min(self.pacer.wait(now, idle), self.days.wait(now, idle), self.jobs.wait(now, idle))
        if rfds or wfds:
            rd, wr, _ = select.select(rfds, wfds, [], timeout)
        else:
            time.sleep(timeout)
            rd, wr = [], []
        now = time.monotonic()

        # The Mac asleep, or this process stopped: whatever queued up in
        # the ports meanwhile is old, and none of it may reach the page.
        # The wall clock is the one that runs on through a sleep, and it
        # is asked here, between the wait and the reading, because the
        # first thing the wait returns after a wake is that very backlog.
        wall = time.time()
        if wall - self.last_wall > STALL_S:
            log("nothing for %.0f s (the Mac slept?); dropping what queued up" % (wall - self.last_wall))
            for b in self.boards:
                b.flush()
            self.pacer.forget()
            sp.heard_at = now
            j = self.jobs.active     # its clocks stopped too; the sleep is not speaker's fault
            if j is not None:
                j.progress_at = now
                j.began = now - max(0, j.sent - PACE_BURST) / float(PACE_BPS)
                j.until = max(j.until, now + 1.0) if j.state == "listening" else j.until
            rd = [x for x in rd if not any(x is b.port for b in self.boards)]
        self.last_wall = wall

        for b in open_ports:
            if b.port is not None and b.port in rd:
                lines = b.read(now)
                if b is sp:
                    self.on_speaker(lines, now)
        sp.check(now)
        self.serve(rd, wr, now)
        self.jobs.step(now, sp, sp.port is not None and sp.port in wr)

        for line in (self.pacer.due(now), self.days.due(now)):
            if line is None:
                continue
            if watch is None:
                print(line.rstrip("\n"), flush=True)
            else:
                watch.send(line, now)


def run(args):
    need_pyserial()
    macs = registry()
    speaker = Speaker(args.speaker or macs.get("speaker", SPEAKER_MAC), args.verbose)
    watch = None if args.dry_run else Watch(args.watch or macs.get("watch", WATCH_MAC), args.verbose)
    # A dry run writes nothing to either board, so it takes no jobs either.
    server = None if args.dry_run else Server(args.socket)
    if server is not None and not server.open():
        server = None
    relay = Relay(speaker, watch, server)

    def stop(signum, frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)
    log("relaying speaker %s -> %s" % (speaker.mac.upper(),
                                       "stdout (dry run)" if watch is None else "watch " + watch.mac.upper()))
    try:
        while True:
            relay.step()
    except KeyboardInterrupt:
        pass
    finally:
        for b in relay.boards:
            b.close()
        if server is not None:
            server.close()
    log("stopped")
    return 0


# ---- the self-test ------------------------------------------------------------

LINE = ("noise: LAF 47.1 LAeq3 45.8 dBA (est) LAeq1 46.2 | red 0s | "
        "19:59:24 ring on chime off sd ok")


class _NoSerial:
    """pyserial's two exceptions, for a self-test run where it is not installed."""

    class SerialException(OSError):
        pass

    class SerialTimeoutException(SerialException):
        pass


class FakePort:
    """One end of a socketpair, standing in for a board's port."""

    in_waiting = 0

    def __init__(self, sock):
        sock.setblocking(False)
        self.sock = sock

    def fileno(self):
        return self.sock.fileno()

    def read(self, n):
        try:
            return self.sock.recv(65536)
        except BlockingIOError:
            return b""

    def write(self, data):
        self.sock.sendall(data)
        return len(data)

    def reset_input_buffer(self):
        pass

    def reset_output_buffer(self):
        pass

    def close(self):
        self.sock.close()


def socket_test(check):
    """The whole loop -- speaker's lines to watch, and jobs from a client on a
    real Unix socket -- with socketpairs in the boards' places."""
    global serial
    import tempfile
    if serial is None:
        serial = _NoSerial
    tmp = tempfile.mkdtemp(prefix="nr")
    if len(tmp) > 80:
        tmp = tempfile.mkdtemp(prefix="nr", dir="/tmp")
    path = os.path.join(tmp, "tell", "speaker.sock")

    sp_a, sp_b = socket.socketpair()
    w_a, w_b = socket.socketpair()
    sp_b.setblocking(False)
    w_b.setblocking(False)
    speaker, watch = Speaker(SPEAKER_MAC, False), Watch(WATCH_MAC, False)
    for b, s in ((speaker, sp_a), (watch, w_a)):
        b.port, b.device, b.next_look = FakePort(s), "fake", float("inf")
        b.lines.skip = False
    srv = Server(path)
    check("socket: opens", srv.open(), True)
    check("socket: directory 0700", stat.S_IMODE(os.stat(os.path.dirname(path)).st_mode), 0o700)
    check("socket: 0600", stat.S_IMODE(os.lstat(path).st_mode), 0o600)
    check("socket: a second relay leaves it be", Server(path).open(), False)
    relay = Relay(speaker, watch, srv)
    got = {"speaker": bytearray(), "watch": bytearray()}

    def pump(done, secs=3.0):
        end = time.monotonic() + secs
        while time.monotonic() < end:
            relay.step(idle=0.01)
            for s, k in ((sp_b, "speaker"), (w_b, "watch")):
                try:
                    while True:
                        d = s.recv(65536)
                        if not d:
                            break
                        got[k] += d
                except BlockingIOError:
                    pass
            if done():
                return True
        return False

    def ask(request, when=None, says=b"", secs=3.0):
        """A request from a client; speaker says `says` once when() is true.
        The reply's lines, END and all."""
        c = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        c.connect(path)
        c.setblocking(False)
        out, reply, said = bytearray(request), bytearray(), [False]

        def done():
            if out:
                try:
                    del out[:c.send(out)]
                except BlockingIOError:
                    pass
            try:
                reply.extend(c.recv(65536))
            except BlockingIOError:
                pass
            if when is not None and not said[0] and when():
                sp_b.sendall(says)
                said[0] = True
            return reply.endswith(b"END\n")

        pump(done, secs)
        c.close()
        return reply.decode("utf-8", "replace").splitlines()

    r = ask(b"STATUS\n")
    check("STATUS: speaker", r[:2], ["speaker: connected (fake)", "watch: connected (fake)"])
    check("STATUS: ends", r[-1:], ["END"])

    # speaker's noise line and a full days set, arriving together.
    start = dt.date(2026, 8, 22)
    entries = []
    for i in range(35):
        d = (start + dt.timedelta(days=i)).isoformat()
        entries.append(d + ("=--" if i % 9 == 3 else "=--/--" if i == 5 else
                            "=%.1f/%.1f" % (40 + i / 4.0, 30 + i / 8.0)) + ("*" if i == 34 else ""))
    days_msg = "days: cal " + " ".join(entries)
    sp_b.sendall(("I (1) speaker: " + LINE + " today 46.9\nI (2) speaker: " + days_msg + "\n").encode())
    pump(lambda: got["watch"].count(b"!noisedays") >= 9)
    wl = got["watch"].decode().splitlines()
    dl = [x for x in wl if x.startswith("!noisedays ")]
    check("days reach watch", len(dl), 9)
    check("days lines fit", max(len(x) for x in wl) <= WATCH_LINE, True)
    check("days lines hold at most four", max(len(x.split()) - 3 for x in dl), 4)
    check("days as they came, but for the empty day",
          [e for x in dl for e in x.split()[3:]], [e.replace("=--/--", "=--") for e in entries])
    check("the noise line is not held behind them", wl.index("!noise 47.1 45.8 46.9 est") < wl.index(dl[-1]), True)
    r = ask(b"STATUS\n")
    check("STATUS: the noise line", any(x.endswith(LINE + " today 46.9") and x.split()[1] == "noise:" for x in r), True)
    check("STATUS: the days line", [x.split(" ", 1)[1] for x in r if x.split()[1:2] == ["days:"]], [days_msg])

    # A command, and its answer.
    got["speaker"].clear()
    r = ask(b"CMD !cal 55\n", lambda: b"!cal 55\n" in got["speaker"],
            b"I (3) speaker: noise: LAF 47.1 LAeq3 45.8 dBA (cal)\n"
            b"I (3) speaker: cal: the room is 55.0 dBA: offset 1.00 dB (was 0.00)\n")
    check("CMD: sent", bytes(got["speaker"]), b"!cal 55\n")
    check("CMD: answered", r, ["OK sent", "cal: the room is 55.0 dBA: offset 1.00 dB (was 0.00)", "END"])
    got["speaker"].clear()
    r = ask(b"CMD !chime on\n", lambda: b"!chime on\n" in got["speaker"],
            b"I (4) speaker: chime on (never 21:00-06:00, at most once in 10 min)\n")
    check("CMD: an answer with no colon", r[1:], ["chime on (never 21:00-06:00, at most once in 10 min)", "END"])
    got["speaker"].clear()
    r = ask(b"CMD !status\n")
    check("CMD: !status refused", (r[0][:4], r[-1]), ("ERR ", "END"))
    r = ask(b"CMD !cal\n")
    check("CMD: !cal without a level refused", (r[0][:4], r[-1]), ("ERR ", "END"))
    pump(lambda: False, 0.1)
    check("CMD: nothing refused reaches speaker", bytes(got["speaker"]), b"")
    r = ask(b"CMD !ring on\n", secs=4.0)
    check("CMD: no answer is still an END", r, ["OK sent", "END"])

    # Speech: the header, then every byte, then speaker's word on it.
    got["speaker"].clear()
    pcm = bytes(range(256)) * 400            # 102400 bytes, more than a socket buffer
    head = b"!play 102400 16000 test\n"
    t0 = time.monotonic()
    r = ask(b"PLAY 102400 16000 test\n" + pcm, lambda: len(got["speaker"]) >= len(head) + len(pcm),
            b"I (5) speaker: play: started (102400 bytes, test)\nI (6) speaker: play: done\n")
    took = time.monotonic() - t0
    check("PLAY: every byte, in order", bytes(got["speaker"]) == head + pcm, True)
    check("PLAY: paced", took >= (len(head) + len(pcm) - PACE_BURST) / float(PACE_BPS) * 0.95, True)
    check("PLAY: answered", r, ["OK queued", "play: started (102400 bytes, test)", "play: done", "END"])
    got["speaker"].clear()
    r = ask(b"PLAY 4 16000 normal\n\x01\x00\x02\x00", lambda: got["speaker"].endswith(b"\x02\x00"),
            b"W (7) speaker: play: refused (quiet hours)\n")
    check("PLAY: refused, said so", r, ["OK queued", "play: refused (quiet hours)", "END"])
    for bad in (b"PLAY 32000 44100 test\n", b"PLAY 960002 16000 test\n", b"PLAY 31 16000 test\n", b"HELLO\n"):
        r = ask(bad)
        check("PLAY: %r refused" % bad, (r[0][:4], len(r)), ("ERR ", 2))

    # speaker unplugged.
    speaker.lost("unplugged", time.monotonic())
    speaker.next_look = float("inf")
    got["speaker"].clear()
    check("absent: PLAY", ask(b"PLAY 4 16000 test\n\x00\x00\x00\x00"), ["ERR speaker not connected", "END"])
    check("absent: CMD", ask(b"CMD !ring off\n"), ["ERR speaker not connected", "END"])
    check("absent: STATUS", ask(b"STATUS\n")[0], "speaker: not connected")

    srv.close()
    check("socket: removed on the way out", os.path.lexists(path), False)
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(path)                    # the file a relay killed outright leaves behind
    s.close()
    srv2 = Server(path)
    check("socket: a stale one is replaced", srv2.open(), True)
    srv2.close()
    for s in (sp_b, w_b):
        s.close()
    for b in (speaker, watch):
        b.close()
    try:
        os.rmdir(os.path.dirname(path))
        os.rmdir(tmp)
    except OSError:
        pass


def self_test():
    global QUIET
    QUIET = True
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

    # ---- speaker's messages, and the days ----
    check("message", speaker_message("\x1b[0;33mW (9) speaker: cal: 5.0 dBA not taken\x1b[0m"),
          "cal: 5.0 dBA not taken")
    check("another tag", speaker_message("I (9) wifi: days: cal"), None)
    check("no prefix", speaker_message("play: done\r"), "play: done")
    check("word with colon", first_word("cal: the room is 55.0 dBA"), "cal")
    check("word without", first_word("chime on (never 21:00-06:00)"), "chime")
    check("no word", first_word(""), "")

    check("days", parse_days("days: cal 2026-09-23=49.8/36.0 2026-09-24=-- 2026-09-25=52.4/38.1*"),
          (True, [Day("2026-09-23", 49.8, 36.0, False), Day("2026-09-24", None, None, False),
                  Day("2026-09-25", 52.4, 38.1, True)]))
    check("days, est and gaps", parse_days("days: est 2026-09-24=--/-- 2026-09-25=--.-/40.0*"),
          (False, [Day("2026-09-24", None, None, False), Day("2026-09-25", None, 40.0, True)]))
    check("days, bad entries left out",
          parse_days("days: cal 2026-02-30=40.0/30.0 2026-09-2=40/30 junk 2026-09-25=41.0/30.5*"),
          (True, [Day("2026-09-25", 41.0, 30.5, True)]))
    check("days, sorted, later wins",
          parse_days("days: cal 2026-09-25=41.0/30.0* 2026-09-24=40.0/30.0 2026-09-25=42.0/31.0*")[1],
          [Day("2026-09-24", 40.0, 30.0, False), Day("2026-09-25", 42.0, 31.0, True)])
    check("days, no mode", parse_days("days: 2026-09-25=41.0/30.0*"), None)
    check("days, not days", parse_days("noise: LAF 47.1"), None)
    check("entry, both", day_entry(Day("2026-09-25", 52.4, 38.1, True)), "2026-09-25=52.4/38.1*")
    check("entry, none", day_entry(Day("2026-09-24", None, None, False)), "2026-09-24=--")
    check("entry, half", day_entry(Day("2026-09-24", None, 40.0, False)), "2026-09-24=--/40.0")

    # The widest set there can be: 35 days of three-figure levels.
    start = dt.date(2026, 8, 22)
    wide = [Day((start + dt.timedelta(days=i)).isoformat(), 100.0 + i / 10.0, 100.0, i == 34)
            for i in range(35)]
    cmds = days_commands(True, wide)
    check("wide: nine lines", len(cmds), 9)
    check("wide: every line fits watch", max(len(c.rstrip("\n")) for c in cmds) <= WATCH_LINE, True)
    check("wide: at most four a line", max(len(c.split()) - 3 for c in cmds), 4)
    check("wide: numbered", [c.split()[1] for c in cmds], ["%d/9" % k for k in range(1, 10)])
    check("wide: all there, in order", [e for c in cmds for e in c.split()[3:]],
          [day_entry(d) for d in wide])
    worst = days_commands(True, [Day("2026-09-2%d" % i, 150.0, 150.0, True) for i in range(4)])
    check("worst line there can be", len(worst[0].rstrip("\n")), 114)
    many = " ".join(day_entry(Day((start + dt.timedelta(days=i)).isoformat(), 50.0, 40.0, False))
                    for i in range(40))
    check("forty days keep the newest 35", [d.date for d in parse_days("days: est " + many)[1]][0],
          (start + dt.timedelta(days=5)).isoformat())
    check("one day, one line", days_commands(False, [Day("2026-09-25", 45.0, 33.3, True)]),
          ["!noisedays 1/1 est 2026-09-25=45.0/33.3*\n"])

    do = DaysOut()
    do.set(["a", "b", "c"])
    check("days: first at once", do.due(10.0), "a")
    check("days: spaced", do.due(10.01), None)
    check("days: wait", round(do.wait(10.01, 0.5), 3), 0.04)
    check("days: then the next", do.due(10.05), "b")
    do.set(["x", "y"])
    check("days: a new set replaces the rest", [do.due(11.0), do.due(11.1), do.due(11.2)], ["x", "y", None])
    do.again(20.0)
    check("days: again, not before", do.due(19.9), None)
    check("days: again", do.due(20.0), "x")

    # ---- what the socket takes ----
    for text, want in (("!cal 55", "!cal 55"), ("  !cal   55.5 ", "!cal 55.5"), ("!chime on", "!chime on"),
                       ("!ring off", "!ring off"), ("!night 22-07", "!night 22-07"), ("!night 0-0", "!night 0-0")):
        check("command %r" % text, check_command(text), want)
    for text in ("!cal", "!cal abc", "!cal 5", "!cal 131", "!cal 55 60", "!chime maybe", "!ring",
                 "!night 24-07", "!night 22", "!status", "!clock 1758830000", "!play 2 16000 test",
                 "!cal 55\n!ring on", "!cal 55\r", "", "hello"):
        try:
            check_command(text)
            check("refused %r" % text, "carried", "refused")
        except ValueError:
            check("refused %r" % text, "refused", "refused")
    check("PLAY", parse_request(b"PLAY 32000 16000 normal"), ("PLAY", 32000, "normal"))
    check("PLAY 30 s", parse_request(b"PLAY 960000 16000 test\r"), ("PLAY", 960000, "test"))
    check("STATUS", parse_request(b"STATUS"), ("STATUS",))
    check("CMD", parse_request(b"CMD !ring on"), ("CMD", "!ring on"))
    for bad, why in ((b"PLAY 32000 44100 normal", "bad header"), (b"PLAY 960002 16000 test", "too long"),
                     (b"PLAY 31 16000 test", "bad header"), (b"PLAY 0 16000 test", "bad header"),
                     (b"PLAY -2 16000 test", "bad header"), (b"PLAY 32000 16000 loud", "bad header"),
                     (b"PLAY 32000", "bad header"), (b"CMD !status", "!status"), (b"\xffPLAY", "bad header"),
                     (b"HELLO", "unknown")):
        try:
            parse_request(bad)
            check("refused %r" % bad, "taken", why)
        except ValueError as e:
            check("refused %r" % bad, why if str(e).startswith(why) else str(e), why)

    # ---- the relay itself, on fake boards and a real socket ----
    try:
        socket_test(check)
    except Exception as e:          # a hang or a crash is a failure, not a traceback
        import traceback
        traceback.print_exc()
        check("socket test ran", repr(e), None)

    for f in fails:
        print("FAIL " + f)
    print("self-test: %d of %d checks passed" % (count[0] - len(fails), count[0]))
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser(description="Relay speaker's noise level and days to watch over USB, "
                                             "and take jobs for speaker on a socket.")
    ap.add_argument("--dry-run", action="store_true",
                    help="read speaker and print what would be sent; watch is not opened, "
                         "and no socket is served")
    ap.add_argument("--verbose", action="store_true", help="echo both boards' serial output")
    ap.add_argument("--self-test", action="store_true",
                    help="check the parsers, pacing and socket on fakes, then exit")
    ap.add_argument("--speaker", metavar="MAC", help="speaker's MAC, instead of boards.tsv's")
    ap.add_argument("--watch", metavar="MAC", help="watch's MAC, instead of boards.tsv's")
    ap.add_argument("--socket", metavar="PATH", default=SOCK_PATH,
                    help="where to take jobs for speaker (default ~/.tell/speaker.sock)")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    try:
        return run(args)
    except BrokenPipeError:
        return 0        # --dry-run into head, say


if __name__ == "__main__":
    sys.exit(main())
