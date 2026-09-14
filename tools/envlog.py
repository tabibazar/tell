#!/usr/bin/env python3
"""Read wave's environment log out of flash and make sense of it.

    tools/envlog.py                       dump from the board and summarise
    tools/envlog.py --csv readings.csv    also write every reading out
    tools/envlog.py --bin envlog.bin      decode a dump taken earlier
    tools/envlog.py --self-test           check the decoder against known bytes

The log is a ring of 4 KB sectors written by main/envstore.c. Each sector
opens with a magic and a sequence number; sequence numbers only ever rise, so
the oldest live sector is the one with the lowest and the ring's order follows
from that. Records are twelve bytes and carry their own timestamp, which is
what makes a power cut show up as a gap rather than as history sliding
sideways.
"""

import argparse
import datetime as dt
import os
import struct
import subprocess
import sys
import tempfile

MAGIC = 0x31564E45          # "ENV1", little-endian
SECTOR = 4096
HDR = 8
RECORD = 12
PER_SECTOR = (SECTOR - HDR) // RECORD
NO_MINUTE = 0xFFFFFFFF

PART_OFFSET = 0x410000
PART_SIZE = 0x80000


def decode(blob):
    """Every reading in the blob, oldest first."""
    sectors = []
    for i in range(len(blob) // SECTOR):
        base = i * SECTOR
        magic, seq = struct.unpack_from("<II", blob, base)
        if magic != MAGIC:
            continue
        sectors.append((seq, base))
    sectors.sort()

    out = []
    for _seq, base in sectors:
        for k in range(PER_SECTOR):
            off = base + HDR + k * RECORD
            minute, temp, rh, hpa, _res = struct.unpack_from("<IhHHH", blob, off)
            if minute == NO_MINUTE:
                break          # the rest of this sector was never written
            out.append((minute, temp / 100.0, rh / 100.0, hpa / 10.0))
    return out


def when(minute):
    """A record's timestamp. The board counts minutes since 1970 in its own
    local time, so this is naive rather than UTC -- saying otherwise would
    shift every reading by the offset."""
    return dt.datetime(1970, 1, 1) + dt.timedelta(minutes=minute)


def summarise(rows):
    if not rows:
        print("the log is empty")
        return

    # Ring order is time order, normally. When it is not, say so rather than
    # quietly reporting a span and a set of gaps computed from readings that
    # are out of sequence -- which is exactly how a firmware bug that wrote
    # one corrupt timestamp an hour first showed up as nonsense here.
    out_of_order = [j for j in range(1, len(rows)) if rows[j][0] < rows[j - 1][0]]
    if out_of_order:
        print(f"WARNING: {len(out_of_order)} reading(s) are older than the one"
              f" written before them.")
        print("         Their timestamps are wrong; summarising in time order.")
        for j in out_of_order[:5]:
            print(f"           {when(rows[j][0]):%Y-%m-%d %H:%M}"
                  f" written after {when(rows[j - 1][0]):%Y-%m-%d %H:%M}")
        if len(out_of_order) > 5:
            print(f"           ... and {len(out_of_order) - 5} more")
        print()
        rows = sorted(rows)

    first, last = rows[0], rows[-1]
    span = (last[0] - first[0]) / 60.0
    print(f"{len(rows)} readings")
    print(f"from  {when(first[0]):%Y-%m-%d %H:%M}")
    print(f"to    {when(last[0]):%Y-%m-%d %H:%M}   ({span:.1f} hours)")
    print()
    for name, i, unit, dp in (("temperature", 1, "C", 1),
                              ("humidity", 2, "%", 0),
                              ("pressure", 3, "hPa", 1)):
        vals = [r[i] for r in rows]
        lo, hi = min(vals), max(vals)
        avg = sum(vals) / len(vals)
        print(f"{name:12s} {lo:.{dp}f} to {hi:.{dp}f} {unit}"
              f"   mean {avg:.{dp}f}   now {rows[-1][i]:.{dp}f}")

    # Gaps. One a minute is the intent, so anything longer is the board
    # having been off -- which is the thing worth seeing in a log that is
    # meant to survive a power cut.
    gaps = []
    for a, b in zip(rows, rows[1:]):
        missing = b[0] - a[0]
        if missing > 1:
            gaps.append((a[0], b[0], missing))
    print()
    if not gaps:
        print("no gaps: every minute accounted for")
    else:
        print(f"{len(gaps)} gap(s), where the board was off or the sensor quiet:")
        for start, end, missing in gaps[-10:]:
            mins = missing - 1
            print(f"  {when(start):%Y-%m-%d %H:%M} -> {when(end):%H:%M}"
                  f"   {mins} minute(s) missing")
        if len(gaps) > 10:
            print(f"  ... and {len(gaps) - 10} earlier")


def dump_from_board(port):
    esptool = os.path.expanduser(
        "~/esp/esp-idf/components/esptool_py/esptool/esptool.py")
    python = os.path.expanduser(
        "~/.espressif/python_env/idf5.5_py3.13_env/bin/python")
    tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
    tmp.close()
    cmd = [python, esptool, "--port", port, "read_flash",
           hex(PART_OFFSET), hex(PART_SIZE), tmp.name]
    print(f"reading {PART_SIZE // 1024} KB from {port} ...", file=sys.stderr)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(r.stderr.strip() or "esptool failed")
    with open(tmp.name, "rb") as f:
        blob = f.read()
    os.unlink(tmp.name)
    return blob


def find_port():
    import glob
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        sys.exit("no board on USB: plug wave in")
    return ports[0]


def self_test():
    """Build a partition the way the firmware does and read it back, including
    a wrap, so the decoder is checked before it is trusted with real data."""
    blob = bytearray(b"\xff" * (SECTOR * 4))
    written = []
    # Three sectors, the third holding a partial run: sequence numbers rising,
    # and deliberately not starting at sector zero so the ordering is tested
    # rather than the layout accidentally agreeing.
    order = [(2, 7), (3, 8), (0, 9)]
    minute = 29000000
    for sector, seq in order:
        struct.pack_into("<II", blob, sector * SECTOR, MAGIC, seq)
        n = PER_SECTOR if sector != 0 else 5
        for k in range(n):
            off = sector * SECTOR + HDR + k * RECORD
            struct.pack_into("<IhHHH", blob, off, minute, 2431, 4512, 9847, 0)
            written.append(minute)
            minute += 1
    rows = decode(bytes(blob))
    ok = True
    if len(rows) != len(written):
        print(f"FAIL count: got {len(rows)}, want {len(written)}"); ok = False
    if [r[0] for r in rows] != written:
        print("FAIL order: readings did not come back oldest first"); ok = False
    if rows and (abs(rows[0][1] - 24.31) > 1e-9
                 or abs(rows[0][2] - 45.12) > 1e-9
                 or abs(rows[0][3] - 984.7) > 1e-9):
        print(f"FAIL scaling: got {rows[0][1:]}"); ok = False
    # A negative temperature must survive as negative.
    struct.pack_into("<IhHHH", blob, 2 * SECTOR + HDR, 29000000, -1250, 4512, 9847, 0)
    if decode(bytes(blob))[0][1] != -12.5:
        print("FAIL sign: a freezing reading came back positive"); ok = False
    print("self-test passed" if ok else "self-test FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", help="decode a dump instead of reading the board")
    ap.add_argument("--port", help="serial port (default: the first usbmodem)")
    ap.add_argument("--csv", help="also write every reading here")
    ap.add_argument("--self-test", action="store_true")
    a = ap.parse_args()

    if a.self_test:
        sys.exit(self_test())

    if a.bin:
        with open(a.bin, "rb") as f:
            blob = f.read()
    else:
        blob = dump_from_board(a.port or find_port())

    rows = decode(blob)
    summarise(rows)

    if a.csv:
        with open(a.csv, "w") as f:
            f.write("when,temperature_c,humidity_pct,pressure_hpa\n")
            for minute, t, h, p in rows:
                f.write(f"{when(minute):%Y-%m-%d %H:%M},{t:.2f},{h:.2f},{p:.1f}\n")
        print(f"\n{len(rows)} readings written to {a.csv}")


if __name__ == "__main__":
    main()
