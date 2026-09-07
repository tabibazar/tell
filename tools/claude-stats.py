#!/usr/bin/env python3
"""Summarise local Claude Code usage as a fixed-size block of text.

    tools/claude-stats.py | tell --device big
    tools/claude-stats.py --cols 20 --rows 5 | tell --device small
    tools/claude-stats.py --self-test
    tools/claude-stats.py --format data --section year   # the heatmap page
    tools/claude-stats.py --format data --section cost   # API-equivalent cost
    tools/claude-stats.py --format data --section rhythm # weekday x hour heatmap
    tools/claude-stats.py --format data --section now    # today, live

Reads the session transcripts under ~/.claude/projects for recent, exact
figures, and merges ~/.claude/stats-cache.json for the months before that:
transcripts are pruned after a while, but the cache keeps a per-day summary
back to the first session. Where both know a day, the transcripts win.

Output is ASCII only: the screen font covers 32..126, so box-drawing
characters would render as '?'.
"""
import argparse
import collections
import datetime as dt
import glob
import json
import math
import os
import sys

TRANSCRIPTS = os.path.expanduser("~/.claude/projects/**/*.jsonl")
STATS_CACHE = os.path.expanduser("~/.claude/stats-cache.json")

# API list prices in dollars per million tokens: input, output, cache read,
# cache write for the 5-minute TTL. Cache writes for the 1-hour TTL are twice
# the input price. Keyed by the short model name; unknown models fall back to
# their family's rate, and failing that to Opus, so a new model is never
# priced at zero.
PRICES = {
    "fable-5.1":  (10.0, 50.0, 0.25, 12.5),
    "fable-5":    (10.0, 50.0, 1.0, 12.5),
    "opus-5":     (5.0, 25.0, 0.5, 6.25),
    "opus-4.8":   (5.0, 25.0, 0.5, 6.25),
    "opus-4.7":   (5.0, 25.0, 0.5, 6.25),
    "opus-4.6":   (5.0, 25.0, 0.5, 6.25),
    "opus-4.5":   (5.0, 25.0, 0.5, 6.25),
    "sonnet-5":   (2.0, 10.0, 0.2, 2.5),
    "sonnet-4.6": (3.0, 15.0, 0.3, 3.75),
    "sonnet-4.5": (3.0, 15.0, 0.3, 3.75),
    "haiku-4.5":  (1.0, 5.0, 0.1, 1.25),
}
FAMILY_PRICES = {"fable": PRICES["fable-5.1"], "opus": PRICES["opus-5"],
                 "sonnet": PRICES["sonnet-5"], "haiku": PRICES["haiku-4.5"]}


def price_of(model):
    if model in PRICES:
        return PRICES[model]
    return FAMILY_PRICES.get(model.split("-")[0], PRICES["opus-5"])


def usage_cost(model, u):
    """Dollars one message would have cost on the API. `u` is a usage dict
    from a transcript or a modelUsage entry from the stats cache; both
    spellings of the field names are accepted."""
    pin, pout, pread, pwrite5 = price_of(model)
    tin = u.get("input_tokens", u.get("inputTokens")) or 0
    tout = u.get("output_tokens", u.get("outputTokens")) or 0
    tread = u.get("cache_read_input_tokens", u.get("cacheReadInputTokens")) or 0
    twrite = u.get("cache_creation_input_tokens", u.get("cacheCreationInputTokens")) or 0
    # Transcripts say which TTL each cache write used; the cache summary does
    # not, so it is priced at the 5-minute rate, which is the cheaper case.
    split = u.get("cache_creation") or {}
    w1h = split.get("ephemeral_1h_input_tokens") or 0
    w5m = twrite - w1h if w1h <= twrite else twrite
    return (tin * pin + tout * pout + tread * pread
            + w5m * pwrite5 + w1h * pin * 2.0) / 1e6


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


EPOCH_ORDINAL = dt.date(1970, 1, 1).toordinal()


def day_number(day):
    """A date as days since 1970-01-01, which is how the board is told about
    dates: it has no calendar, so it counts and converts back."""
    return day.toordinal() - EPOCH_ORDINAL


def local_stamp(ts):
    """A transcript timestamp ('2026-09-07T01:23:45.678Z', UTC) as a local
    datetime, or None. Local, because a session at 9pm in Toronto is that
    evening, not the next UTC day."""
    try:
        return dt.datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone()
    except (ValueError, TypeError):
        return None


def load_cache(path=STATS_CACHE):
    """Claude Code's own stats summary, or None if unreadable."""
    try:
        with open(path) as f:
            d = json.load(f)
        return d if isinstance(d, dict) else None
    except (OSError, ValueError):
        return None


def new_model():
    return {"in": 0, "out": 0, "cread": 0, "ccreate": 0, "calls": 0, "cost": 0.0}


def project_name(path):
    """The repository a transcript belongs to, from its folder: the folder is
    the working directory with slashes turned to dashes, so the last piece is
    the directory's own name."""
    folder = os.path.basename(os.path.dirname(path))
    return (folder.rsplit("-", 1)[-1] or folder)[:15] or "?"


def collect(pattern=TRANSCRIPTS, cache=None):
    """Aggregate token usage from every transcript, then fill in the days the
    transcripts no longer cover from the stats cache (a dict, a path, or the
    default file; False to skip it)."""
    models = collections.defaultdict(new_model)
    daily = collections.Counter()
    daily_cost = collections.Counter()
    model_daily = collections.defaultdict(collections.Counter)
    sessions = {}                       # id -> [first, last] epoch seconds
    rhythm = collections.Counter()      # (weekday 0=Mon, hour) -> messages
    rhythm_days = set()
    msgs_by_day = collections.Counter()
    sessions_by_day = collections.defaultdict(set)
    latest = None                       # (epoch, model, project, session id)
    files = 0
    calls = 0
    first_day = last_day = None
    stamp_cache = {}

    for path in glob.glob(pattern, recursive=True):
        files += 1
        project = project_name(path)
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
            cost = usage_cost(model, u)
            m["cost"] += cost
            calls += 1

            ts = d.get("timestamp")
            when = None
            if isinstance(ts, str) and len(ts) >= 10:
                # Convert once per distinct second; transcripts repeat them.
                key = ts[:19]
                when = stamp_cache.get(key)
                if when is None:
                    when = local_stamp(ts)
                    stamp_cache[key] = when

            if d.get("sessionId"):
                span = sessions.setdefault(d["sessionId"], [None, None])
                if when is not None:
                    t = when.timestamp()
                    if span[0] is None or t < span[0]:
                        span[0] = t
                    if span[1] is None or t > span[1]:
                        span[1] = t

            if when is not None:
                day = when.date().isoformat()
                total = ((u.get("input_tokens") or 0) + (u.get("output_tokens") or 0)
                         + (u.get("cache_read_input_tokens") or 0)
                         + (u.get("cache_creation_input_tokens") or 0))
                daily[day] += total
                daily_cost[day] += cost
                model_daily[model][day] += total
                rhythm[(when.weekday(), when.hour)] += 1
                rhythm_days.add(day)
                msgs_by_day[day] += 1
                if d.get("sessionId"):
                    sessions_by_day[day].add(d["sessionId"])
                t = when.timestamp()
                if latest is None or t > latest[0]:
                    latest = (t, model, project, d.get("sessionId"))
                if first_day is None or day < first_day:
                    first_day = day
                if last_day is None or day > last_day:
                    last_day = day

    longest = 0
    for first, last in sessions.values():
        if first is not None and last is not None and last - first > longest:
            longest = last - first

    stats = {"models": dict(models), "daily": daily, "daily_cost": daily_cost,
             "model_daily": model_daily, "sessions": len(sessions),
             "files": files, "calls": calls, "longest_session": int(longest),
             "first_day": first_day, "last_day": last_day,
             "estimated_days": 0,
             "rhythm": rhythm, "rhythm_days": len(rhythm_days),
             "msgs_by_day": msgs_by_day, "sessions_by_day": sessions_by_day,
             "session_spans": sessions, "latest": latest}
    if cache is None:
        cache = load_cache()
    elif isinstance(cache, str):
        cache = load_cache(cache)
    if cache:
        merge_cache(stats, cache, sessions)
    return stats


def merge_cache(stats, cache, sessions):
    """Adds what the stats cache knows and the transcripts no longer do.

    The cache is complete up to its lastComputedDate, so for days up to and
    including that date it is the source of truth and the transcripts' figures
    for those days are dropped; after it only the transcripts know anything.
    That keeps totals from double counting whichever way the two overlap."""
    cutoff = cache.get("lastComputedDate")
    if not isinstance(cutoff, str) or len(cutoff) != 10:
        return
    models = stats["models"]
    daily, daily_cost = stats["daily"], stats["daily_cost"]
    model_daily = stats["model_daily"]

    # Per-model, all time: the cache's totals, plus transcript messages after
    # the cutoff. Recompute the transcript share from its daily series.
    tx_after = collections.defaultdict(new_model)
    for name, m in models.items():
        after_tokens = sum(t for d, t in model_daily[name].items() if d > cutoff)
        all_tokens = sum(model_daily[name].values()) or 1
        frac = after_tokens / all_tokens
        for k in ("in", "out", "cread", "ccreate", "cost"):
            tx_after[name][k] = m[k] * frac
        tx_after[name]["calls"] = m["calls"]        # calls: transcripts only
    merged = collections.defaultdict(new_model)
    for full, u in (cache.get("modelUsage") or {}).items():
        name = short_model(full)
        if name == "<synthetic>" or not isinstance(u, dict):
            continue
        mm = merged[name]
        mm["in"] += u.get("inputTokens") or 0
        mm["out"] += u.get("outputTokens") or 0
        mm["cread"] += u.get("cacheReadInputTokens") or 0
        mm["ccreate"] += u.get("cacheCreationInputTokens") or 0
        mm["cost"] += usage_cost(name, u)
    for name, m in tx_after.items():
        mm = merged[name]
        for k in ("in", "out", "cread", "ccreate", "cost"):
            mm[k] += m[k]
        mm["calls"] += m["calls"]
    for name, m in merged.items():
        for k in ("in", "out", "cread", "ccreate", "calls"):
            m[k] = int(round(m[k]))
    stats["models"] = dict(merged)

    # Per day: drop the transcripts' days up to the cutoff, take the cache's.
    for series in (daily, daily_cost):
        for d in [d for d in series if d <= cutoff]:
            del series[d]
    for name in list(model_daily):
        for d in [d for d in model_daily[name] if d <= cutoff]:
            del model_daily[name][d]

    # Blended cost per token per model, to price the cache's daily tokens.
    rate = {}
    for name, m in stats["models"].items():
        tokens = m["in"] + m["out"] + m["cread"] + m["ccreate"]
        rate[name] = m["cost"] / tokens if tokens else 0.0
    known_days = set()
    for row in cache.get("dailyModelTokens") or []:
        d = row.get("date")
        by_model = row.get("tokensByModel")
        if not isinstance(d, str) or d > cutoff or not isinstance(by_model, dict):
            continue
        known_days.add(d)
        for full, t in by_model.items():
            name = short_model(full)
            t = int(t or 0)
            daily[d] += t
            model_daily[name][d] += t
            daily_cost[d] += t * rate.get(name, 0.0)

    # Older still, the cache knows only that there were messages. Estimate
    # tokens from the average message on the days where both are known, so
    # the heatmap shows the activity rather than a blank; the estimate is
    # marked in the output so nobody mistakes it for a measurement.
    activity = [r for r in cache.get("dailyActivity") or []
                if isinstance(r.get("date"), str) and r["date"] <= cutoff]
    known_msgs = sum(r.get("messageCount") or 0 for r in activity
                     if r["date"] in known_days)
    known_tokens = sum(daily[d] for d in known_days)
    known_cost = sum(daily_cost[d] for d in known_days)
    per_msg_tokens = known_tokens / known_msgs if known_msgs else 0
    per_msg_cost = known_cost / known_msgs if known_msgs else 0.0
    for r in activity:
        d = r["date"]
        if d in known_days or d in daily:
            continue
        n = r.get("messageCount") or 0
        if n <= 0:
            continue
        daily[d] += int(n * per_msg_tokens)
        daily_cost[d] += n * per_msg_cost
        stats["estimated_days"] += 1

    # Headline figures.
    after = sum(1 for first, _ in sessions.values()
                if first is not None
                and dt.datetime.fromtimestamp(first).date().isoformat() > cutoff)
    stats["sessions"] = int(cache.get("totalSessions") or 0) + after
    longest = cache.get("longestSession") or {}
    if isinstance(longest, dict):
        secs = int((longest.get("duration") or 0) / 1000)
        stats["longest_session"] = max(stats["longest_session"], secs)
    first = cache.get("firstSessionDate")
    when = local_stamp(first) if isinstance(first, str) else None
    if when is not None:
        d = when.date().isoformat()
        if stats["first_day"] is None or d < stats["first_day"]:
            stats["first_day"] = d
    if daily:
        stats["first_day"] = min(stats["first_day"] or "9999", min(daily))
        stats["last_day"] = max(stats["last_day"] or "0000", max(daily))


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


GRID_ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
GRID_WEEKS = 53


def grid_char(tokens):
    """One character for a day's tokens: '.' for none, otherwise a half-octave
    log scale, round(2*log2(tokens/1000)), so 62 characters span a thousand
    tokens to two trillion within about 19%. A year of days is then at most
    371 bytes however busy it was, which is what keeps the payload under the
    2048-byte BLE limit."""
    if tokens <= 0:
        return "."
    i = int(round(2 * math.log2(max(tokens, 1000) / 1000.0)))
    return GRID_ALPHABET[max(0, min(len(GRID_ALPHABET) - 1, i))]


def grid_value(ch):
    """The tokens a grid character stands for; the inverse of grid_char."""
    if ch == ".":
        return 0
    return int(round(1000 * 2 ** (GRID_ALPHABET.index(ch) / 2.0)))


def year_grid(daily, today):
    """The grid start (a Sunday about a year back, so today is in the last
    column) and the one-character-per-day string from there to today."""
    since_sunday = (today.weekday() + 1) % 7
    start = today - dt.timedelta(days=(GRID_WEEKS - 1) * 7 + since_sunday)
    chars = []
    day = start
    while day <= today:
        chars.append(grid_char(daily.get(day.isoformat(), 0)))
        day += dt.timedelta(days=1)
    return start, "".join(chars)


def render_year(stats, today=None):
    """The !year payload: the grid, and the headline figures beneath it."""
    today = today or dt.date.today()
    start, grid = year_grid(stats["daily"], today)
    models = stats["models"]
    lines = ["!year", "host %s" % host_name(),
             "start %d" % day_number(start),
             "today %d" % day_number(today)]
    if stats.get("first_day"):
        lines.append("first %d" % day_number(dt.date.fromisoformat(stats["first_day"])))
    lines.append("grid %s" % grid)
    lines.append("sessions %d" % stats["sessions"])
    lines.append("longest %d" % stats.get("longest_session", 0))
    if models:
        # Favourite by total tokens: it is the only measure the merged
        # history has for every model, all the way back.
        fav = max(models.items(),
                  key=lambda kv: kv[1]["in"] + kv[1]["out"] + kv[1]["cread"]
                  + kv[1]["ccreate"])[0]
        lines.append("fav %s" % fav[:15])
    lines.append("tok %d %d %d %d" % (
        sum(m["in"] for m in models.values()),
        sum(m["out"] for m in models.values()),
        sum(m["cread"] for m in models.values()),
        sum(m["ccreate"] for m in models.values())))
    if stats.get("estimated_days"):
        # Days the cache knew only as message counts; the board says so.
        lines.append("estimated %d" % stats["estimated_days"])
    return lines


MODEL_DAYS = 60


def day_grid(counter, start, today):
    """One character per day from start to today for one Counter of days."""
    chars = []
    day = start
    while day <= today:
        chars.append(grid_char(counter.get(day.isoformat(), 0)))
        day += dt.timedelta(days=1)
    return "".join(chars)


def cents(dollars):
    return int(round(dollars * 100))


def render_cost(stats, today=None):
    """The !cost payload: what the usage would have cost on the API."""
    today = today or dt.date.today()
    models = stats["models"]
    daily_cost = stats.get("daily_cost", {})
    start = today - dt.timedelta(days=MODEL_DAYS - 1)

    def window(days):
        since = (today - dt.timedelta(days=days - 1)).isoformat()
        return sum(c for d, c in daily_cost.items() if since <= d <= today.isoformat())

    lines = ["!cost", "host %s" % host_name(),
             "start %d" % day_number(start), "today %d" % day_number(today),
             "total %d" % cents(sum(m["cost"] for m in models.values())),
             "last30 %d" % cents(window(30)),
             "last7 %d" % cents(window(7))]
    ranked = sorted(models.items(), key=lambda kv: -kv[1]["cost"])
    for name, m in ranked[:8]:
        lines.append("c %s %d" % (name[:15], cents(m["cost"])))
    # Daily cost in thousandths of a dollar, so the grid scale's unit of
    # 1000 is one dollar and a cheap day still registers.
    milli = {d: c * 1000 for d, c in daily_cost.items()}
    lines.append("grid %s" % day_grid(milli, start, today))
    plan = os.environ.get("CLAUDE_PLAN_USD")
    if plan:
        try:
            lines.append("plan %d" % cents(float(plan)))
        except ValueError:
            pass
    return lines


def render_rhythm(stats):
    """The !rhythm payload: messages by weekday and hour, Monday first, as
    168 characters. Counts are scaled by a thousand before encoding so the
    grid alphabet's unit of a thousand becomes one message."""
    rhythm = stats.get("rhythm", {})
    chars = []
    for dow in range(7):
        for hour in range(24):
            chars.append(grid_char(rhythm.get((dow, hour), 0) * 1000))
    return ["!rhythm", "host %s" % host_name(),
            "days %d" % stats.get("rhythm_days", 0),
            "msgs %d" % sum(rhythm.values()),
            "grid %s" % "".join(chars)]


def render_now(stats, now=None):
    """The !now payload: today so far, and what happened last."""
    now = now or dt.datetime.now().astimezone()
    today = now.date().isoformat()
    daily_cost = stats.get("daily_cost", {})
    lines = ["!now", "host %s" % host_name(),
             "tokens %d" % stats["daily"].get(today, 0),
             "cost %d" % cents(daily_cost.get(today, 0.0)),
             "msgs %d" % stats.get("msgs_by_day", {}).get(today, 0),
             "sessions %d" % len(stats.get("sessions_by_day", {}).get(today, ()))]

    # A typical day, for the "today versus usual" bar: the 30-day mean of
    # the days that had anything at all.
    since = (now.date() - dt.timedelta(days=30)).isoformat()
    recent = [t for d, t in stats["daily"].items() if since <= d < today and t > 0]
    lines.append("avg %d" % (sum(recent) // len(recent) if recent else 0))

    latest = stats.get("latest")
    if latest:
        t, model, project, sid = latest
        lines.append("last %d" % max(0, int(now.timestamp() - t)))
        lines.append("model %s" % model[:15])
        lines.append("project %s" % project[:15])
        span = stats.get("session_spans", {}).get(sid)
        if span and span[0] is not None and span[1] is not None:
            lines.append("session %d" % int(span[1] - span[0]))
    return lines


def render_data(stats, section, today=None):
    """Marker-prefixed lines for the firmware to parse."""
    if section == "year":
        return render_year(stats, today)
    if section == "cost":
        return render_cost(stats, today)
    if section == "rhythm":
        return render_rhythm(stats)
    if section == "now":
        return render_now(stats)
    today = today or dt.date.today()
    lines = []
    if section == "stats":
        lines.append("!stats")
        lines.append("host %s" % host_name())
        # Each model row ends with its last MODEL_DAYS days, a character per
        # day, which is what the per-model line chart draws. Older firmware
        # ignores the extra fields.
        start = today - dt.timedelta(days=MODEL_DAYS - 1)
        lines.append("start %d" % day_number(start))
        lines.append("today %d" % day_number(today))
        ranked = sorted(stats["models"].items(),
                        key=lambda kv: -(kv[1]["cread"] + kv[1]["out"]))
        model_daily = stats.get("model_daily", {})
        for name, m in ranked[:8]:
            grid = day_grid(model_daily.get(name, {}), start, today)
            lines.append("m %s %d %d %d %d %d %s"
                         % (name[:15], m["out"], m["cread"], m["in"],
                            m["ccreate"], m["calls"], grid))
    else:
        lines.append("!daily")
        lines.append("host %s" % host_name())
        for day, total in sorted(stats["daily"].items())[-14:]:
            # Trailing weekday (0 Monday) so the board can colour by it; it
            # has no calendar of its own.
            try:
                dow = dt.date.fromisoformat(day).weekday()
            except ValueError:
                dow = -1
            lines.append("d %s %d %d" % (day[5:], total, dow))
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

    # The year grid: bounded, ASCII, and decodable to within a fifth.
    for tokens in (1, 999, 1000, 1500, 12345, 2_000_000, 750_000_000, 1_000_000_000_000):
        back = grid_value(grid_char(tokens))
        ref = max(tokens, 1000)
        if not (0.8 * ref <= back <= 1.25 * ref):
            print("FAIL grid: %d -> %r -> %d" % (tokens, grid_char(tokens), back))
            failures += 1
    if grid_char(0) != "." or grid_value(".") != 0:
        print("FAIL grid: zero must be a dot")
        failures += 1
    print("ok   grid characters round-trip")

    for today in (dt.date(2026, 9, 7), dt.date(2026, 9, 6), dt.date(2026, 9, 12),
                  dt.date(2024, 2, 29)):
        start, grid = year_grid(fake["daily"], today)
        if (start.weekday() + 1) % 7 != 0:
            print("FAIL grid for %s starts on a %s" % (today, start.strftime("%A")))
            failures += 1
        if len(grid) != (today - start).days + 1 or len(grid) > GRID_WEEKS * 7:
            print("FAIL grid length %d for %s" % (len(grid), today))
            failures += 1
    print("ok   grid starts on a Sunday and ends today")

    payload = "\n".join(render_year(fake, dt.date(2026, 9, 7)))
    if len(payload.encode()) > 2048:
        print("FAIL year payload is %d bytes" % len(payload))
        failures += 1
    if any(ord(c) < 32 or ord(c) > 126 for c in payload.replace("\n", "")):
        print("FAIL year payload has non-ASCII")
        failures += 1
    if "grid " not in payload or "tok 2 2000002 4000000003 9" not in payload:
        print("FAIL year payload content:\n%s" % payload)
        failures += 1
    print("ok   year payload fits one BLE message (%d bytes)" % len(payload))

    big = dict(fake)
    big["models"] = {"model-%d" % i: {"in": 10**12, "out": 10**12, "cread": 10**13,
                                      "ccreate": 10**12, "calls": 10**6}
                     for i in range(12)}
    big["model_daily"] = {n: collections.Counter({"2026-09-0%d" % i: 10**11 for i in range(1, 8)})
                          for n in big["models"]}
    payload = "\n".join(render_data(big, "stats", dt.date(2026, 9, 7)))
    if len(payload.encode()) > 2048:
        print("FAIL stats payload is %d bytes with eight huge models" % len(payload))
        failures += 1
    rows = [l for l in payload.split("\n") if l.startswith("m ")]
    if len(rows) != 8 or any(len(r.split()[7]) != MODEL_DAYS for r in rows):
        print("FAIL stats rows:\n%s" % payload)
        failures += 1
    print("ok   stats payload with eight models fits one BLE message (%d bytes)"
          % len(payload))

    # Pricing: one Opus 5 message, all four token kinds, 5-minute cache.
    c = usage_cost("opus-5", {"input_tokens": 1_000_000, "output_tokens": 1_000_000,
                               "cache_read_input_tokens": 1_000_000,
                               "cache_creation_input_tokens": 1_000_000})
    if abs(c - (5 + 25 + 0.5 + 6.25)) > 1e-6:
        print("FAIL opus-5 price: %.4f" % c)
        failures += 1
    c = usage_cost("opus-5", {"input_tokens": 0, "output_tokens": 0,
                               "cache_read_input_tokens": 0,
                               "cache_creation_input_tokens": 1_000_000,
                               "cache_creation": {"ephemeral_1h_input_tokens": 1_000_000}})
    if abs(c - 10.0) > 1e-6:
        print("FAIL 1-hour cache write should be twice input: %.4f" % c)
        failures += 1
    if price_of("opus-9") != PRICES["opus-5"] or price_of("mystery") != PRICES["opus-5"]:
        print("FAIL unknown models fall back to a family rate")
        failures += 1
    print("ok   API prices")

    # Cache merge: the cache owns days up to its cutoff, transcripts after.
    tx = {"models": {"opus-5": dict(new_model(), out=100, cost=1.0, calls=2)},
          "daily": collections.Counter({"2026-09-05": 500, "2026-09-07": 700}),
          "daily_cost": collections.Counter({"2026-09-05": 0.5, "2026-09-07": 0.5}),
          "model_daily": {"opus-5": collections.Counter({"2026-09-05": 500,
                                                          "2026-09-07": 700})},
          "sessions": 3, "files": 1, "calls": 2, "longest_session": 100,
          "first_day": "2026-09-05", "last_day": "2026-09-07", "estimated_days": 0}
    cache = {"lastComputedDate": "2026-09-06",
             "modelUsage": {"claude-opus-5": {"inputTokens": 0, "outputTokens": 1000,
                                               "cacheReadInputTokens": 0,
                                               "cacheCreationInputTokens": 0}},
             "dailyModelTokens": [{"date": "2026-09-05", "tokensByModel": {"claude-opus-5": 400}},
                                  {"date": "2026-09-06", "tokensByModel": {"claude-opus-5": 600}}],
             "dailyActivity": [{"date": "2026-04-20", "messageCount": 10},
                               {"date": "2026-09-05", "messageCount": 4},
                               {"date": "2026-09-06", "messageCount": 6}],
             "totalSessions": 50, "longestSession": {"duration": 3_600_000},
             "firstSessionDate": "2026-04-20T14:00:00.000Z"}
    merge_cache(tx, cache, {"a": [dt.datetime(2026, 9, 7, 12).timestamp(), None],
                            "b": [dt.datetime(2026, 9, 1, 12).timestamp(), None]})
    m = tx["models"]["opus-5"]
    want_out = 1000 + round(100 * 700 / 1200)
    if m["out"] != want_out:
        print("FAIL merged output tokens %d, want %d" % (m["out"], want_out))
        failures += 1
    if tx["daily"]["2026-09-05"] != 400 or tx["daily"]["2026-09-06"] != 600 \
            or tx["daily"]["2026-09-07"] != 700:
        print("FAIL merged days: %r" % dict(tx["daily"]))
        failures += 1
    if tx["daily"]["2026-04-20"] != 1000 or tx["estimated_days"] != 1:
        print("FAIL activity-only day estimated: %r" % tx["daily"].get("2026-04-20"))
        failures += 1
    if tx["sessions"] != 51 or tx["longest_session"] != 3600 \
            or tx["first_day"] != "2026-04-20":
        print("FAIL headline merge: %r %r %r" % (tx["sessions"], tx["longest_session"],
                                                 tx["first_day"]))
        failures += 1
    print("ok   stats cache merges under the transcripts")

    fake["daily_cost"] = collections.Counter({"2026-09-0%d" % i: i * 1.5 for i in range(1, 8)})
    for mm in fake["models"].values():
        mm["cost"] = 12.34
    payload = "\n".join(render_cost(fake, dt.date(2026, 9, 7)))
    if len(payload.encode()) > 2048 or "total 2468" not in payload \
            or "last7 4200" not in payload or "grid " not in payload:
        print("FAIL cost payload:\n%s" % payload)
        failures += 1
    print("ok   cost payload (%d bytes)" % len(payload))

    fake["rhythm"] = collections.Counter({(1, 14): 40, (6, 2): 1})
    fake["rhythm_days"] = 62
    payload = "\n".join(render_rhythm(fake))
    grid = [l for l in payload.split("\n") if l.startswith("grid ")][0][5:]
    if len(grid) != 168 or grid[1 * 24 + 14] == "." or grid[6 * 24 + 2] == "." \
            or grid[0] != "." or grid_value(grid[1 * 24 + 14]) < 30000:
        print("FAIL rhythm grid:\n%s" % payload)
        failures += 1
    print("ok   rhythm payload (%d bytes)" % len(payload))

    fake["msgs_by_day"] = collections.Counter({"2026-09-07": 12})
    fake["sessions_by_day"] = {"2026-09-07": {"a", "b"}}
    fake["latest"] = (dt.datetime(2026, 9, 7, 12, 0).timestamp(), "opus-5", "tell", "a")
    fake["session_spans"] = {"a": [dt.datetime(2026, 9, 7, 9, 0).timestamp(),
                                   dt.datetime(2026, 9, 7, 12, 0).timestamp()]}
    payload = "\n".join(render_now(fake, dt.datetime(2026, 9, 7, 12, 5).astimezone()))
    if "tokens 7000" not in payload or "msgs 12" not in payload or "sessions 2" not in payload \
            or "last 300" not in payload or "session 10800" not in payload \
            or "project tell" not in payload:
        print("FAIL now payload:\n%s" % payload)
        failures += 1
    print("ok   now payload (%d bytes)" % len(payload))

    print("all tests passed" if not failures else "%d test(s) failed" % failures)
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cols", type=int, default=64)
    ap.add_argument("--rows", type=int, default=20)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--format", choices=("ascii", "data"), default="ascii")
    ap.add_argument("--section",
                    choices=("stats", "daily", "year", "cost", "rhythm", "now"),
                    default="stats")
    ap.add_argument("--no-cache", action="store_true",
                    help="transcripts only; skip ~/.claude/stats-cache.json")
    ap.add_argument("--all", metavar="DIR",
                    help="write every data section to DIR/<section>.txt in one pass")
    a = ap.parse_args()

    if a.self_test:
        sys.exit(self_test())

    stats = collect(cache=False if a.no_cache else None)
    if a.all:
        os.makedirs(a.all, exist_ok=True)
        for section in ("stats", "daily", "year", "cost", "rhythm", "now"):
            with open(os.path.join(a.all, section + ".txt"), "w") as f:
                f.write("\n".join(render_data(stats, section)) + "\n")
        return
    if a.format == "data":
        print("\n".join(render_data(stats, a.section)))
    else:
        print("\n".join(render(stats, a.cols, a.rows)))


if __name__ == "__main__":
    main()
