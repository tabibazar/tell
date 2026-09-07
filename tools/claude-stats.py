#!/usr/bin/env python3
"""Summarise local Claude Code usage as a fixed-size block of text.

    tools/claude-stats.py | tell --device big
    tools/claude-stats.py --cols 20 --rows 5 | tell --device small
    tools/claude-stats.py --self-test

Reads the session transcripts under ~/.claude/projects rather than
~/.claude/stats-cache.json, which lags by months and only knows about a
fraction of the sessions on disk.

Output is ASCII only: the screen font covers 32..126, so box-drawing
characters would render as '?'.
"""
import argparse
import collections
import datetime as dt
import glob
import json
import os
import sys

TRANSCRIPTS = os.path.expanduser("~/.claude/projects/**/*.jsonl")


def short_model(name):
    """claude-opus-4-5-20251101 -> opus-4.5"""
    n = name.replace("claude-", "")
    for suffix in ("-20", "_20"):
        i = n.find(suffix)
        if i > 0:
            n = n[:i]
    parts = n.split("-")
    if len(parts) >= 3 and parts[1].isdigit() and parts[2].isdigit():
        return "%s-%s.%s" % (parts[0], parts[1], parts[2])
    return n


def human(n):
    """Compact magnitude, so a billion fits in a narrow column."""
    for limit, suffix in ((1e12, "T"), (1e9, "B"), (1e6, "M"), (1e3, "K")):
        if n >= limit:
            v = n / limit
            return "%.1f%s" % (v, suffix) if v < 10 else "%.0f%s" % (v, suffix)
    return str(int(n))


def collect(pattern=TRANSCRIPTS):
    """Aggregate token usage from every transcript."""
    models = collections.defaultdict(
        lambda: {"in": 0, "out": 0, "cread": 0, "ccreate": 0, "calls": 0})
    daily = collections.Counter()
    sessions = set()
    files = 0
    calls = 0
    first_day = last_day = None

    for path in glob.glob(pattern, recursive=True):
        files += 1
        for line in open(path, errors="ignore"):
            if '"usage"' not in line:
                continue
            try:
                d = json.loads(line)
            except ValueError:
                continue
            msg = d.get("message")
            if not isinstance(msg, dict) or not isinstance(msg.get("usage"), dict):
                continue

            u = msg["usage"]
            model = short_model(msg.get("model") or "unknown")
            if model == "<synthetic>":
                continue

            m = models[model]
            m["in"] += u.get("input_tokens") or 0
            m["out"] += u.get("output_tokens") or 0
            m["cread"] += u.get("cache_read_input_tokens") or 0
            m["ccreate"] += u.get("cache_creation_input_tokens") or 0
            m["calls"] += 1
            calls += 1

            if d.get("sessionId"):
                sessions.add(d["sessionId"])

            ts = d.get("timestamp")
            if isinstance(ts, str) and len(ts) >= 10:
                day = ts[:10]
                total = ((u.get("input_tokens") or 0) + (u.get("output_tokens") or 0)
                         + (u.get("cache_read_input_tokens") or 0)
                         + (u.get("cache_creation_input_tokens") or 0))
                daily[day] += total
                if first_day is None or day < first_day:
                    first_day = day
                if last_day is None or day > last_day:
                    last_day = day

    return {"models": dict(models), "daily": daily, "sessions": len(sessions),
            "files": files, "calls": calls,
            "first_day": first_day, "last_day": last_day}


def bar(value, peak, width):
    """Proportional bar. A non-zero value always shows something."""
    if peak <= 0 or width <= 0:
        return ""
    filled = int(round(value / peak * width))
    if value > 0 and filled == 0:
        filled = 1
    return "#" * filled


def render(stats, cols=64, rows=20):
    """Lay the summary out for a screen of exactly cols x rows."""
    models = stats["models"]
    total_out = sum(m["out"] for m in models.values())
    total_all = sum(m["in"] + m["out"] + m["cread"] + m["ccreate"]
                    for m in models.values())

    lines = []
    # Fall back to shorter titles on a narrow screen, and to a plain rule when
    # even those do not fit, so the header is never wider than the panel.
    title = ""
    for candidate in (" CLAUDE CODE USAGE ", " CLAUDE USAGE ", " USAGE "):
        if len(candidate) + 2 <= cols:
            title = candidate
            break
    pad = max(0, cols - 2 - len(title))
    lines.append(("+" + "-" * (pad // 2) + title
                  + "-" * (pad - pad // 2) + "+")[:cols])

    lines.append(" sessions %s   calls %s   tokens %s"
                 % (stats["sessions"], human(stats["calls"]), human(total_all)))

    if stats.get("first_day") and stats.get("last_day"):
        span = len(stats["daily"])
        lines.append(" transcripts %s..%s  %d days"
                     % (stats["first_day"], stats["last_day"], span))
    lines.append("")

    ranked = sorted(models.items(), key=lambda kv: -(kv[1]["cread"] + kv[1]["out"]))
    if ranked:
        lines.append(" BY MODEL          out     cache-read")
        peak = max(m["cread"] + m["out"] for _, m in ranked) or 1
        for name, m in ranked[:4]:
            lines.append(" %-13s %7s %9s  %s"
                         % (name[:13], human(m["out"]), human(m["cread"]),
                            bar(m["cread"] + m["out"], peak, 12)))
        lines.append("")

    recent = sorted(stats["daily"].items())[-7:]
    if recent:
        lines.append(" LAST %d DAYS" % len(recent))
        peak = max(v for _, v in recent) or 1
        for day, total in recent:
            label = day[5:]
            lines.append(" %-6s %7s  %s" % (label, human(total),
                                            bar(total, peak, cols - 20)))
        lines.append("")

    summary = (" written %s   read %s"
               % (human(total_out), human(sum(m["cread"] for m in models.values()))))

    # The frame and the summary are fixed furniture; only the middle is
    # trimmed, so the totals are never the thing that falls off the bottom.
    head, middle = lines[0], lines[1:]
    room = rows - 3                      # head, summary, closing rule
    middle = [l[:cols] for l in middle][:room]
    while len(middle) < room:
        middle.append("")
    return [head] + middle + [summary[:cols], "+" + "-" * (cols - 2) + "+"]


def host_name():
    """A short label for this machine, so the board keeps each Mac's data
    apart. socket.gethostname() is a MAC address on a stock Mac, so prefer
    the Bonjour name; override with CLAUDE_SCREEN_HOST."""
    import re
    import socket
    import subprocess

    name = os.environ.get("CLAUDE_SCREEN_HOST")
    if not name:
        try:
            name = subprocess.run(["scutil", "--get", "LocalHostName"],
                                  capture_output=True, text=True,
                                  timeout=5).stdout.strip()
        except Exception:                       # noqa: BLE001
            name = ""
    if not name:
        name = socket.gethostname()

    # Trim the usual "-MacBook-Pro" tail; the panel is only 64 columns.
    name = name.split(".")[0]
    name = re.sub(r"[-_]?MacBook[-_]?(Pro|Air)?", "", name, flags=re.I)
    name = re.sub(r"[^A-Za-z0-9-]", "", name).strip("-")
    return (name or "mac")[:15]


def render_data(stats, section):
    """Marker-prefixed lines for the firmware to parse."""
    lines = []
    if section == "stats":
        lines.append("!stats")
        lines.append("host %s" % host_name())
        ranked = sorted(stats["models"].items(),
                        key=lambda kv: -(kv[1]["cread"] + kv[1]["out"]))
        for name, m in ranked[:8]:
            lines.append("m %s %d %d" % (name[:15], m["out"], m["cread"]))
    else:
        lines.append("!daily")
        lines.append("host %s" % host_name())
        for day, total in sorted(stats["daily"].items())[-14:]:
            lines.append("d %s %d" % (day[5:], total))
    return lines


def self_test():
    """The invariant that matters on a fixed screen: nothing overflows."""
    failures = 0
    fake = {
        "models": {"opus-5": {"in": 1, "out": 2_000_000, "cread": 4_000_000_000,
                              "ccreate": 5, "calls": 10},
                   "haiku-4.5": {"in": 1, "out": 2, "cread": 3, "ccreate": 4,
                                 "calls": 1}},
        "daily": collections.Counter({"2026-09-0%d" % i: i * 1000 for i in range(1, 8)}),
        "sessions": 43, "files": 847, "calls": 26607,
        "first_day": "2026-08-04", "last_day": "2026-09-06",
    }
    for cols, rows in ((64, 20), (20, 5), (33, 10), (80, 24)):
        out = render(fake, cols, rows)
        if len(out) != rows:
            print("FAIL %dx%d: got %d rows" % (cols, rows, len(out)))
            failures += 1
        for i, line in enumerate(out):
            if len(line) > cols:
                print("FAIL %dx%d: line %d is %d chars" % (cols, rows, i, len(line)))
                failures += 1
            if any(ord(c) < 32 or ord(c) > 126 for c in line):
                print("FAIL %dx%d: line %d has non-ASCII" % (cols, rows, i))
                failures += 1
        print("ok   %dx%d fits" % (cols, rows))

    empty = render({"models": {}, "daily": collections.Counter(), "sessions": 0,
                    "files": 0, "calls": 0, "first_day": None,
                    "last_day": None}, 64, 20)
    if len(empty) != 20:
        print("FAIL empty input")
        failures += 1
    else:
        print("ok   empty input still fills the screen")

    print("all tests passed" if not failures else "%d test(s) failed" % failures)
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cols", type=int, default=64)
    ap.add_argument("--rows", type=int, default=20)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--format", choices=("ascii", "data"), default="ascii")
    ap.add_argument("--section", choices=("stats", "daily"), default="stats")
    a = ap.parse_args()

    if a.self_test:
        sys.exit(self_test())

    stats = collect()
    if a.format == "data":
        print("\n".join(render_data(stats, a.section)))
    else:
        print("\n".join(render(stats, a.cols, a.rows)))


if __name__ == "__main__":
    main()
