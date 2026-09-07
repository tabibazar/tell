#!/usr/bin/env python3
"""Summarise local Claude Code usage as a fixed-size block of text.

    tools/claude-stats.py | tell --device big
    tools/claude-stats.py --cols 20 --rows 5 | tell --device small
    tools/claude-stats.py --self-test
    tools/claude-stats.py --format data --section year   # the heatmap page
    tools/claude-stats.py --format data --section cost   # API-equivalent cost
    tools/claude-stats.py --format data --section rhythm # weekday x hour heatmap
    tools/claude-stats.py --format data --section now    # today, live
    tools/claude-stats.py --format data --section projects  # tokens by repository
    tools/claude-stats.py --format data --section cache  # prompt-cache hit rate
    tools/claude-stats.py --format data --section tools  # which tools Claude calls
    tools/claude-stats.py --format data --section thinking  # thinking vs visible output
    tools/claude-stats.py --format data --section week   # last 7 days vs the 7 before
    tools/claude-stats.py --format data --section records  # personal bests

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


def short_tool(name):
    """mcp__claude_ai_Datadog__search_datadog_logs -> mcp:Datadog. Built-in
    tools keep their names; they are short already."""
    if name.startswith("mcp__"):
        parts = name.split("__")
        server = parts[1] if len(parts) > 1 else "?"
        for prefix in ("claude_ai_", "plugin_"):
            if server.startswith(prefix):
                server = server[len(prefix):]
        return ("mcp:" + server.split("_")[0])[:15]
    return name[:15]


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
    projects = collections.defaultdict(
        lambda: {"tokens": 0, "cost": 0.0, "msgs": 0, "sessions": set()})
    daily_cache = collections.defaultdict(lambda: [0, 0, 0])   # in, cread, cwrite
    tools = collections.Counter()
    daily_tools = collections.Counter()
    thinking = collections.defaultdict(lambda: [0, 0])         # model -> [thinking, output]
    daily_thinking = collections.defaultdict(lambda: [0, 0])   # day -> [thinking, output]
    session_tools = collections.Counter()                      # session id -> tool calls
    session_day = {}                                           # session id -> first day
    biggest_response = (0, None)                               # output tokens, day
    first_minute = last_minute = None                          # (minute of day, day)
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

            n_tools = 0
            for block in msg.get("content") or []:
                if isinstance(block, dict) and block.get("type") == "tool_use":
                    tools[short_tool(block.get("name") or "?")] += 1
                    n_tools += 1
            # Only messages that report the split count towards the thinking
            # share; older models report no details, and zero would be a lie.
            details = u.get("output_tokens_details")
            think = details.get("thinking_tokens") if isinstance(details, dict) else None

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
                daily_tools[day] += n_tools
                sid = d.get("sessionId")
                if sid:
                    session_tools[sid] += n_tools
                    if sid not in session_day or day < session_day[sid]:
                        session_day[sid] = day
                out_tokens = u.get("output_tokens") or 0
                if out_tokens > biggest_response[0]:
                    biggest_response = (out_tokens, day)
                minute = when.hour * 60 + when.minute
                if first_minute is None or minute < first_minute[0]:
                    first_minute = (minute, day)
                if last_minute is None or minute > last_minute[0]:
                    last_minute = (minute, day)
                if think is not None:
                    thinking[model][0] += think or 0
                    thinking[model][1] += u.get("output_tokens") or 0
                    daily_thinking[day][0] += think or 0
                    daily_thinking[day][1] += u.get("output_tokens") or 0
                dc = daily_cache[day]
                dc[0] += u.get("input_tokens") or 0
                dc[1] += u.get("cache_read_input_tokens") or 0
                dc[2] += u.get("cache_creation_input_tokens") or 0
                pj = projects[project]
                pj["tokens"] += total
                pj["cost"] += cost
                pj["msgs"] += 1
                if d.get("sessionId"):
                    pj["sessions"].add(d["sessionId"])
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
             "session_spans": sessions, "latest": latest,
             "projects": projects, "daily_cache": daily_cache,
             "tools": tools, "daily_tools": daily_tools,
             "tx_sessions": len(sessions), "tx_calls": calls,
             "thinking": thinking, "daily_thinking": daily_thinking,
             "session_tools": session_tools, "session_day": session_day,
             "biggest_response": biggest_response,
             "earliest_minute": first_minute, "latest_minute": last_minute}
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

    daily_tools = stats.get("daily_tools")
    msgs_by_day = stats.get("msgs_by_day")
    sessions_by_day = stats.get("sessions_by_day")
    cache_sessions_by_day = {}
    if daily_tools is not None:
        for series in (daily_tools, msgs_by_day):
            for d in [d for d in series if d <= cutoff]:
                del series[d]
        for d in [d for d in sessions_by_day if d <= cutoff]:
            del sessions_by_day[d]
        for r in cache.get("dailyActivity") or []:
            d = r.get("date")
            if not isinstance(d, str) or d > cutoff:
                continue
            if r.get("toolCallCount"):
                daily_tools[d] += int(r["toolCallCount"])
            if r.get("messageCount"):
                msgs_by_day[d] += int(r["messageCount"])
            if r.get("sessionCount"):
                cache_sessions_by_day[d] = int(r["sessionCount"])
    stats["cache_sessions_by_day"] = cache_sessions_by_day

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


PCT_ALPHABET = GRID_ALPHABET      # 62 steps, linear: 0..100% to within 1.6%


def pct_char(pct):
    """A percentage as one character on a linear scale. The log scale used for
    tokens would be wrong here: 97% and 93% must not share a character."""
    if pct is None:
        return "."
    i = int(round(max(0.0, min(100.0, pct)) * (len(PCT_ALPHABET) - 1) / 100.0))
    return PCT_ALPHABET[i]


def render_projects(stats):
    """The !projects payload: the repositories the tokens went to, from the
    transcripts on this machine (the stats cache has no per-project data)."""
    projects = stats.get("projects", {})
    ranked = sorted(projects.items(), key=lambda kv: -kv[1]["tokens"])
    lines = ["!projects", "host %s" % host_name(),
             "days %d" % stats.get("rhythm_days", 0)]
    shown, other = ranked[:7], ranked[7:]
    for name, pj in shown:
        lines.append("p %s %d %d %d %d" % (name[:15], pj["tokens"], cents(pj["cost"]),
                                           len(pj["sessions"]), pj["msgs"]))
    if other:
        lines.append("p other %d %d %d %d" % (
            sum(pj["tokens"] for _, pj in other),
            cents(sum(pj["cost"] for _, pj in other)),
            sum(len(pj["sessions"]) for _, pj in other),
            sum(pj["msgs"] for _, pj in other)))
    return lines


def cache_saving(model, cread):
    """Dollars saved by serving `cread` tokens from cache instead of as input."""
    pin, _, pread, _ = price_of(model)
    return cread * (pin - pread) / 1e6


def render_cache(stats, today=None):
    """The !cache payload: how much of the input came from cache, per model
    all time and per day for the last MODEL_DAYS, and what that saved."""
    today = today or dt.date.today()
    models = stats["models"]
    start = today - dt.timedelta(days=MODEL_DAYS - 1)
    lines = ["!cache", "host %s" % host_name(),
             "start %d" % day_number(start), "today %d" % day_number(today)]

    saved = 0.0
    ranked = sorted(models.items(),
                    key=lambda kv: -(kv[1]["in"] + kv[1]["cread"] + kv[1]["ccreate"]))
    for name, m in ranked[:8]:
        s_ = cache_saving(name, m["cread"])
        saved += s_
        lines.append("m %s %d %d %d %d" % (name[:15], m["in"], m["cread"],
                                           m["ccreate"], cents(s_)))
    saved += sum(cache_saving(n, m["cread"]) for n, m in ranked[8:])
    lines.append("saved %d" % cents(saved))
    lines.append("cost %d" % cents(sum(m["cost"] for m in models.values())))

    daily_cache = stats.get("daily_cache", {})
    chars = []
    day = start
    while day <= today:
        dc = daily_cache.get(day.isoformat())
        total = sum(dc) if dc else 0
        chars.append(pct_char(100.0 * dc[1] / total if total else None))
        day += dt.timedelta(days=1)
    lines.append("grid %s" % "".join(chars))
    return lines


def render_tools(stats, today=None):
    """The !tools payload: which tools Claude called, how often, and a daily
    count of calls (the stats cache fills the pruned months)."""
    today = today or dt.date.today()
    tools = stats.get("tools", {})
    ranked = sorted(tools.items(), key=lambda kv: -kv[1])
    start = today - dt.timedelta(days=MODEL_DAYS - 1)
    lines = ["!tools", "host %s" % host_name(),
             "days %d" % stats.get("rhythm_days", 0),
             "calls %d" % sum(tools.values()),
             "msgs %d" % stats.get("tx_calls", 0),
             "sessions %d" % stats.get("tx_sessions", 0),
             "start %d" % day_number(start), "today %d" % day_number(today)]
    shown, other = ranked[:7], ranked[7:]
    for name, n in shown:
        lines.append("t %s %d" % (name, n))
    if other:
        lines.append("t other %d" % sum(n for _, n in other))
    daily = {d: n * 1000 for d, n in stats.get("daily_tools", {}).items()}
    lines.append("grid %s" % day_grid(daily, start, today))
    return lines


def render_thinking(stats, today=None):
    """The !thinking payload: thinking versus visible output per model, what
    the thinking cost at output prices, and the daily share."""
    today = today or dt.date.today()
    thinking = stats.get("thinking", {})
    start = today - dt.timedelta(days=MODEL_DAYS - 1)
    lines = ["!thinking", "host %s" % host_name(),
             "days %d" % stats.get("rhythm_days", 0),
             "start %d" % day_number(start), "today %d" % day_number(today)]
    ranked = sorted(thinking.items(), key=lambda kv: -kv[1][1])
    for name, (think, out) in ranked[:8]:
        pout = price_of(name)[1]
        lines.append("m %s %d %d %d %d" % (name[:15], think, max(0, out - think),
                                           cents(think * pout / 1e6),
                                           cents(out * pout / 1e6)))
    chars = []
    day = start
    while day <= today:
        row = stats.get("daily_thinking", {}).get(day.isoformat())
        chars.append(pct_char(100.0 * row[0] / row[1] if row and row[1] else None))
        day += dt.timedelta(days=1)
    lines.append("grid %s" % "".join(chars))
    return lines


def sessions_on(stats, day):
    """Sessions active on a day: the transcripts' set, or the cache's count."""
    live = stats.get("sessions_by_day", {}).get(day)
    if live:
        return len(live)
    return stats.get("cache_sessions_by_day", {}).get(day, 0)


def render_week(stats, today=None):
    """The !week payload: the last seven days against the seven before."""
    today = today or dt.date.today()
    days = [(today - dt.timedelta(days=i)).isoformat() for i in range(14)]
    this, last = days[:7], days[7:]

    def total(series, window):
        return sum(series.get(d, 0) for d in window)

    daily, daily_cost = stats["daily"], stats.get("daily_cost", {})
    msgs, tools_by_day = stats.get("msgs_by_day", {}), stats.get("daily_tools", {})
    lines = ["!week", "host %s" % host_name(), "today %d" % day_number(today),
             "w tokens %d %d" % (total(daily, this), total(daily, last)),
             "w cost %d %d" % (cents(total(daily_cost, this)), cents(total(daily_cost, last))),
             "w msgs %d %d" % (total(msgs, this), total(msgs, last)),
             "w sessions %d %d" % (sum(sessions_on(stats, d) for d in this),
                                   sum(sessions_on(stats, d) for d in last)),
             "w tools %d %d" % (total(tools_by_day, this), total(tools_by_day, last)),
             "w days %d %d" % (sum(1 for d in this if daily.get(d, 0) > 0),
                               sum(1 for d in last if daily.get(d, 0) > 0))]
    # Fourteen days of tokens, oldest first, for the two rows of bars.
    lines.append("grid %s" % "".join(grid_char(daily.get(d, 0)) for d in reversed(days)))
    return lines


def longest_streak(daily):
    """The longest run of consecutive active days, and the day it ended."""
    best, best_end, run, prev = 0, None, 0, None
    for d in sorted(d for d, t in daily.items() if t > 0):
        day = dt.date.fromisoformat(d)
        run = run + 1 if prev is not None and (day - prev).days == 1 else 1
        prev = day
        if run > best:
            best, best_end = run, d
    return best, best_end


def render_records(stats, cache=None):
    """The !records payload: personal bests, each with the day it happened."""
    daily, daily_cost = stats["daily"], stats.get("daily_cost", {})
    msgs = stats.get("msgs_by_day", {})
    lines = ["!records", "host %s" % host_name()]

    def record(key, value, day):
        if value and day:
            lines.append("r %s %d %d" % (key, value, day_number(dt.date.fromisoformat(day))))

    if daily:
        d = max(daily, key=daily.get)
        record("bigday", daily[d], d)
    if daily_cost:
        d = max(daily_cost, key=daily_cost.get)
        record("costday", cents(daily_cost[d]), d)
    if msgs:
        d = max(msgs, key=msgs.get)
        record("msgs", msgs[d], d)
    streak, end = longest_streak(daily)
    record("streak", streak, end)

    # Longest session: the transcripts' spans, or the cache's if longer.
    best_secs, best_day = 0, None
    for sid, (first, last) in stats.get("session_spans", {}).items():
        if first is not None and last is not None and last - first > best_secs:
            best_secs = int(last - first)
            best_day = dt.datetime.fromtimestamp(first).date().isoformat()
    cache = cache if cache is not None else load_cache()
    longest = (cache or {}).get("longestSession") or {}
    if isinstance(longest, dict) and (longest.get("duration") or 0) / 1000 > best_secs:
        when = local_stamp(longest.get("timestamp") or "")
        if when is not None:
            best_secs = int(longest["duration"] / 1000)
            best_day = when.date().isoformat()
    record("session", best_secs, best_day)

    session_tools = stats.get("session_tools", {})
    if session_tools:
        sid = max(session_tools, key=session_tools.get)
        record("toolsess", session_tools[sid], stats.get("session_day", {}).get(sid))
    tokens, day = stats.get("biggest_response") or (0, None)
    record("response", tokens, day)
    early, late = stats.get("earliest_minute"), stats.get("latest_minute")
    if early:
        lines.append("r early %d %d" % (early[0], day_number(dt.date.fromisoformat(early[1]))))
    if late:
        lines.append("r late %d %d" % (late[0], day_number(dt.date.fromisoformat(late[1]))))
    if stats.get("first_day"):
        lines.append("since %d" % day_number(dt.date.fromisoformat(stats["first_day"])))
    return lines


def render_data(stats, section, today=None):
    """Marker-prefixed lines for the firmware to parse."""
    if section == "week":
        return render_week(stats, today)
    if section == "records":
        return render_records(stats)
    if section == "tools":
        return render_tools(stats, today)
    if section == "thinking":
        return render_thinking(stats, today)
    if section == "projects":
        return render_projects(stats)
    if section == "cache":
        return render_cache(stats, today)
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

    if pct_char(0) != "0" or pct_char(100) != "z" or pct_char(None) != "." \
            or pct_char(50) != PCT_ALPHABET[round(61 * 0.5)]:
        print("FAIL percent characters: %r %r %r" % (pct_char(0), pct_char(100), pct_char(50)))
        failures += 1
    print("ok   percent characters")

    fake["projects"] = {"p%d" % i: {"tokens": 1000 * (10 - i), "cost": float(i),
                                      "msgs": i, "sessions": set(range(i))}
                        for i in range(10)}
    payload = "\n".join(render_projects(fake))
    rows = [l for l in payload.split("\n") if l.startswith("p ")]
    if len(rows) != 8 or not rows[-1].startswith("p other ") or "p p0 10000 0 0 0" not in payload:
        print("FAIL projects payload:\n%s" % payload)
        failures += 1
    print("ok   projects payload (%d bytes)" % len(payload))

    fake["daily_cache"] = {"2026-09-07": [10, 90, 0], "2026-09-06": [50, 50, 0]}
    payload = "\n".join(render_cache(fake, dt.date(2026, 9, 7)))
    grid = [l for l in payload.split("\n") if l.startswith("grid ")][0][5:]
    # opus-5: 4e9 cached tokens at $5 - $0.50 per million = $18,000 saved.
    if len(grid) != MODEL_DAYS or grid[-1] != pct_char(90) or grid[-2] != pct_char(50) \
            or grid[0] != "." or "m opus-5 1 4000000000 5 1800000" not in payload:
        print("FAIL cache payload:\n%s" % payload)
        failures += 1
    print("ok   cache payload (%d bytes)" % len(payload))

    if short_tool("mcp__claude_ai_Datadog__search_datadog_logs") != "mcp:Datadog" \
            or short_tool("mcp__plugin_slack_slack__slack_send_message") != "mcp:slack" \
            or short_tool("Bash") != "Bash":
        print("FAIL tool names: %r %r" % (short_tool("mcp__claude_ai_Datadog__x"),
                                          short_tool("mcp__plugin_slack_slack__y")))
        failures += 1
    print("ok   tool names")

    fake["tools"] = collections.Counter({"t%d" % i: 100 - i for i in range(9)})
    fake["daily_tools"] = collections.Counter({"2026-09-07": 42})
    fake["tx_calls"], fake["tx_sessions"] = 500, 7
    payload = "\n".join(render_tools(fake, dt.date(2026, 9, 7)))
    rows = [l for l in payload.split("\n") if l.startswith("t ")]
    grid = [l for l in payload.split("\n") if l.startswith("grid ")][0][5:]
    if len(rows) != 8 or not rows[-1].startswith("t other ") or "calls 864" not in payload \
            or len(grid) != MODEL_DAYS or grid_value(grid[-1]) < 38000 or grid[0] != ".":
        print("FAIL tools payload:\n%s" % payload)
        failures += 1
    print("ok   tools payload (%d bytes)" % len(payload))

    fake["thinking"] = {"opus-5": [350, 1000], "haiku-4.5": [0, 10]}
    fake["daily_thinking"] = {"2026-09-07": [35, 100], "2026-09-06": [50, 100]}
    payload = "\n".join(render_thinking(fake, dt.date(2026, 9, 7)))
    grid = [l for l in payload.split("\n") if l.startswith("grid ")][0][5:]
    if "m opus-5 350 650 1 2" not in payload or grid[-1] != pct_char(35) \
            or grid[-2] != pct_char(50) or grid[0] != ".":
        print("FAIL thinking payload:\n%s" % payload)
        failures += 1
    print("ok   thinking payload (%d bytes)" % len(payload))

    fake["daily"] = collections.Counter({"2026-09-0%d" % i: i * 1000 for i in range(1, 8)})
    fake["daily"].update({"2026-08-3%d" % i: 500 for i in range(0, 2)})
    fake["msgs_by_day"] = collections.Counter({"2026-09-07": 12, "2026-09-01": 3, "2026-08-30": 40})
    fake["sessions_by_day"] = {"2026-09-07": {"a", "b"}}
    fake["cache_sessions_by_day"] = {"2026-08-30": 5}
    fake["daily_tools"] = collections.Counter({"2026-09-07": 42, "2026-08-31": 7})
    payload = "\n".join(render_week(fake, dt.date(2026, 9, 7)))
    grid = [l for l in payload.split("\n") if l.startswith("grid ")][0][5:]
    if "w tokens 28000 1000" not in payload or "w msgs 15 40" not in payload \
            or "w sessions 2 5" not in payload or "w tools 42 7" not in payload \
            or "w days 7 2" not in payload or len(grid) != 14 or grid[-1] == "." or grid[0] != ".":
        print("FAIL week payload:\n%s" % payload)
        failures += 1
    print("ok   week payload (%d bytes)" % len(payload))

    fake["session_spans"] = {"a": [dt.datetime(2026, 9, 1, 9).timestamp(),
                                   dt.datetime(2026, 9, 1, 12).timestamp()]}
    fake["session_tools"] = collections.Counter({"a": 300})
    fake["session_day"] = {"a": "2026-09-01"}
    fake["biggest_response"] = (12345, "2026-09-03")
    fake["earliest_minute"] = (6 * 60 + 5, "2026-09-02")
    fake["latest_minute"] = (23 * 60 + 40, "2026-09-04")
    fake["first_day"] = "2026-08-30"
    payload = "\n".join(render_records(fake, cache={}))
    if "r bigday 7000 20703" not in payload or "r streak 9 20703" not in payload \
            or "r session 10800 20697" not in payload or "r toolsess 300 20697" not in payload \
            or "r response 12345 20699" not in payload or "r early 365 20698" not in payload \
            or "r late 1420 20700" not in payload or "r msgs 40 20695" not in payload \
            or "since 20695" not in payload:
        print("FAIL records payload:\n%s" % payload)
        failures += 1
    print("ok   records payload (%d bytes)" % len(payload))

    print("all tests passed" if not failures else "%d test(s) failed" % failures)
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cols", type=int, default=64)
    ap.add_argument("--rows", type=int, default=20)
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--format", choices=("ascii", "data"), default="ascii")
    ap.add_argument("--section",
                    choices=("stats", "daily", "year", "cost", "rhythm", "now",
                             "projects", "cache", "tools", "thinking", "week",
                             "records"),
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
        for section in ("stats", "daily", "year", "cost", "rhythm", "now",
                        "projects", "cache", "tools", "thinking", "week", "records"):
            with open(os.path.join(a.all, section + ".txt"), "w") as f:
                f.write("\n".join(render_data(stats, section)) + "\n")
        return
    if a.format == "data":
        print("\n".join(render_data(stats, a.section)))
    else:
        print("\n".join(render(stats, a.cols, a.rows)))


if __name__ == "__main__":
    main()
