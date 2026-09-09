#!/usr/bin/env python3
"""Print the !story payload: a two-or-three-sentence recap of today, written
by Claude from the day's prompts.

    tools/story.py | tell --device big
    tools/story.py --dry-run          # show the prompts and the request, no call

The recap comes from the `claude` command in headless mode, so it runs on the
same subscription as the sessions it describes. It is regenerated only when
the number of prompts today has changed; otherwise the cached text is reused,
so a half-hourly agent costs nothing on a quiet afternoon.

Output is ASCII only: the screen font covers 32..126.
"""
import argparse
import datetime as dt
import glob
import json
import os
import re
import shutil
import subprocess
import sys

TRANSCRIPTS = os.path.expanduser("~/.claude/projects/**/*.jsonl")
CACHE_DIR = os.path.expanduser("~/.cache/tell")
MAX_CHARS = 480            # the page shows about eight lines of sixty
MAX_PROMPTS = 80           # more than this and the middle is sampled out
MODEL = os.environ.get("CLAUDE_STORY_MODEL", "sonnet")
EPOCH_ORDINAL = dt.date(1970, 1, 1).toordinal()



def host_name():
    """Same rule as claude-stats.py, so the board files this under the same
    machine."""
    name = os.environ.get("CLAUDE_SCREEN_HOST")
    if not name:
        try:
            name = subprocess.run(["scutil", "--get", "LocalHostName"],
                                  capture_output=True, text=True, timeout=5).stdout.strip()
        except Exception:                       # noqa: BLE001
            name = ""
    name = (name or "mac").split(".")[0]
    name = re.sub(r"[-_]?MacBook[-_]?(Pro|Air)?", "", name, flags=re.I)
    return (re.sub(r"[^A-Za-z0-9-]", "", name).strip("-") or "mac")[:15]


def project_name(path):
    folder = os.path.basename(os.path.dirname(path))
    return (folder.rsplit("-", 1)[-1] or folder)[:15] or "?"


def clean(text):
    """A prompt as a person typed it: injected tags and reminders removed,
    whitespace collapsed."""
    text = re.sub(r"<[^>]{1,80}>", " ", text)
    return re.sub(r"\s+", " ", text).strip()


def todays_prompts(today):
    """(time, project, text) for every human prompt today, in time order."""
    out = []
    for path in glob.glob(TRANSCRIPTS, recursive=True):
        project = project_name(path)
        for line in open(path, errors="ignore"):
            if '"type":"user"' not in line or '"tool_result"' in line:
                continue
            try:
                d = json.loads(line)
            except ValueError:
                continue
            msg = d.get("message")
            if not isinstance(msg, dict) or msg.get("role") != "user":
                continue
            content = msg.get("content")
            if isinstance(content, list):
                content = " ".join(b.get("text", "") for b in content if isinstance(b, dict))
            if not isinstance(content, str) or "[Request interrupted" in content:
                continue
            # Skill bodies and other harness material also arrive as user
            # messages; they are long and start in recognisable ways. Images
            # are references to files, not words the person typed.
            if len(content) > 1500 or content.lstrip().startswith(("Base directory for this skill",
                                                                    "<command-name>", "<local-command")):
                continue
            text = clean(content)
            if not text or text.startswith("[Image: source:"):
                continue
            ts = d.get("timestamp") or ""
            try:
                when = dt.datetime.fromisoformat(ts.replace("Z", "+00:00")).astimezone()
            except ValueError:
                continue
            if when.date() != today:
                continue
            out.append((when, project, text[:300]))
    out.sort()
    return out


def sample(prompts):
    """Keep the first and last of a long day; the middle is where the
    repetition lives."""
    if len(prompts) <= MAX_PROMPTS:
        return prompts
    half = MAX_PROMPTS // 2
    return prompts[:half] + prompts[-half:]


def build_request(today, prompts):
    lines = ["Below are the prompts one person typed to Claude Code today, %s, "
             "with the time and the project each was in." % today.strftime("%A %d %B %Y"),
             "Write a recap of what they worked on, addressed to them as 'you', in two "
             "or three plain sentences and at most %d characters." % MAX_CHARS,
             "Plain ASCII only: no markdown, no bullet points, no quotation marks, "
             "no em dashes. Mention projects by name. Do not list every prompt; "
             "say what was built, fixed or decided. Output only the recap.", ""]
    for when, project, text in sample(prompts):
        lines.append("%s [%s] %s" % (when.strftime("%H:%M"), project, text))
    return "\n".join(lines)


def ask_claude(request):
    """The recap, or None if the command is missing or fails."""
    exe = os.environ.get("CLAUDE_BIN") or shutil.which("claude") or "/opt/homebrew/bin/claude"
    env = dict(os.environ)
    env.pop("CLAUDECODE", None)         # allowed to run from inside a session too
    try:
        r = subprocess.run([exe, "-p", "Write the recap described in the input.",
                            "--model", MODEL],
                           input=request, capture_output=True, text=True,
                           timeout=180, env=env)
    except (OSError, subprocess.TimeoutExpired) as e:
        print("story: claude failed: %s" % e, file=sys.stderr)
        return None
    if r.returncode != 0:
        print("story: claude exited %d: %s" % (r.returncode, r.stderr.strip()[:200]),
              file=sys.stderr)
        return None
    return r.stdout


def tidy(text):
    """ASCII, single spaces, cut at a sentence or word boundary to fit."""
    text = text.replace("—", "-").replace("–", "-")
    text = text.replace("‘", "'").replace("’", "'")
    text = text.replace("“", '"').replace("”", '"')
    text = "".join(ch if 32 <= ord(ch) <= 126 else " " for ch in text)
    text = re.sub(r"\s+", " ", text).strip().strip('"')
    if len(text) > MAX_CHARS:
        cut = text.rfind(". ", 0, MAX_CHARS)
        text = text[:cut + 1] if cut > MAX_CHARS // 2 else text[:MAX_CHARS].rsplit(" ", 1)[0]
    return text


def cached(today):
    try:
        with open(os.path.join(CACHE_DIR, "story-%s.json" % today.isoformat())) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def remember(today, count, text):
    os.makedirs(CACHE_DIR, exist_ok=True)
    with open(os.path.join(CACHE_DIR, "story-%s.json" % today.isoformat()), "w") as f:
        json.dump({"count": count, "text": text}, f)


def payload(today, count, text):
    lines = ["!story", "host %s" % host_name(),
             "date %d" % (today.toordinal() - EPOCH_ORDINAL), "prompts %d" % count]
    # Long lines are fine for BLE, but keep each under the parser's comfort.
    words, cur = text.split(), ""
    for w in words:
        if len(cur) + len(w) + 1 > 100:
            lines.append("text " + cur)
            cur = w
        else:
            cur = (cur + " " + w).strip()
    if cur:
        lines.append("text " + cur)
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true", help="show the request, do not call claude")
    ap.add_argument("--force", action="store_true", help="regenerate even if nothing changed")
    a = ap.parse_args()
    today = dt.date.today()
    prompts = todays_prompts(today)

    if a.dry_run:
        print(build_request(today, prompts))
        return
    if not prompts:
        print(payload(today, 0, "Nothing yet today. The first prompt starts the story."))
        return

    c = cached(today)
    if c and c.get("count") == len(prompts) and c.get("text") and not a.force:
        print(payload(today, len(prompts), c["text"]))
        return

    text = ask_claude(build_request(today, prompts))
    if not text or not tidy(text):
        # Keep whatever we had rather than blanking the page.
        if c and c.get("text"):
            print(payload(today, c.get("count", 0), c["text"]))
        else:
            print(payload(today, len(prompts), "%d prompts today; the recap could not be "
                          "written this time." % len(prompts)))
        return
    text = tidy(text)
    remember(today, len(prompts), text)
    print(payload(today, len(prompts), text))


if __name__ == "__main__":
    main()
