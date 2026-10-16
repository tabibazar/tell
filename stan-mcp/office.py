"""The HOA office console at /office: how a community's board and management company use stan-claw.

They sign in with a per-community password (derived from MCP_TOKEN, see office_password), then:
  /office/home      the dashboard: questions answered, actions done, homes using their panel, top topics,
                    unanswered questions, open requests, announcements and how many office calls were saved
  /office/announce  post an announcement every panel shows and reads aloud, and see who has heard it
  /office/alert     the same, urgent, with templates (water shut-off, elevator, road closed, storm)
  /office/requests  every homeowner's requests; a status update is spoken on that home's panel

The board tells us a message was read aloud with the hidden tool message_heard. record() is called after
every tool call and keeps anonymous daily stats (never an email or a name; homes are counted by a keyed hash).

Storage, pk "office#<community>":
  sk "stat#<YYYY-MM-DD>"     the day's stats (kept 90 days)
  sk "ann#<time>#<id>"       an announcement, with {email: message id} for every home it went to
  sk "req#<ref>#<time>"      a status update on a request
  sk "filed#<ref>"           a request filed through stan-mcp (note_request)
and pk "hw#<community>|<email>", sk "heard#<message id>": that home's panel read the message aloud.
"""
import hmac
import html
import os
import re
import secrets
import urllib.parse
from collections import Counter

import datetime as dt

import family
import state
from hoa_data import COMMUNITIES

SESSION_S = 12 * 3600
MINUTES_PER_CALL = 4             # the assumed length of a phone call to the office that a panel answer saves
STATS_DAYS = 90
MSG_MAX = 400
PANEL_SHOWS = 240                # the panel's message buffer (stan-claw CARE_TEXT)

# The board's own polling and housekeeping: not questions, not actions.
IGNORE = set(family.QUIET) | {"message_heard", "sign_in", "list_communities", "notify_family", "add_family_contact"}
# Things a homeowner gets done (payments, requests, bookings, appeals, votes). The controller may add to it.
ACTIONS = set(family.DOES) | {"vote", "cast_vote", "submit_vote", "proxy_vote", "rsvp_event"}
NOT_ANSWERED = ("No ", "Nothing", "Not signed in", "I don't know")
TOPIC_KEYS = ("topic", "query", "item", "question", "details")
STOP = set("""a about above after again all also am an and any are around as ask asked at be been before being
below between both but by can could did do does doing done down during each few for from further get got had has
have having he her here hers him his how i if in into is it its just know let like may me might more most much
must my no nor not now of off on once only or other our ours out over own please same say says she should so some
such tell than thank thanks that the their theirs them then there these they this those through to too under
until up us very want was we were what when where which while who whom why will with would yes you your yours
okay hello hi stan claw""".split())
TOPIC_LABEL = {"my_account": "account balance", "make_payment": "payment", "payment_plan": "payment plan",
               "violations": "violations", "appeal_violation": "appeal", "my_requests": "request status",
               "file_request": "new request", "reserve_amenity": "booking", "guest_pass": "guest pass",
               "my_household": "household record", "update_household": "household record",
               "resale_certificate": "resale certificate", "community_info": "community overview",
               "fines_schedule": "fines", "amenities": "amenities", "events_and_notices": "events and notices",
               "documents": "documents", "board_and_meetings": "board and meetings", "contacts": "office contacts",
               "who_fixes_what": "repairs"}
CLOSED = ("approved", "completed", "complete", "closed", "resolved", "denied", "done", "fixed", "repaired",
          "withdrawn", "cancelled", "canceled")
KINDS = {"ARC": "architectural", "MNT": "maintenance", "CMP": "complaint", "REN": "renovation",
         "RES": "resale certificate"}
URGENT_TAG = "<span class='tag red'>urgent</span> "
TEMPLATES = [
    ("water", "Water shut-off", "The water will be shut off on [day] from [time] to [time] for repairs. Please fill "
                                "a jug of water beforehand. We are sorry for the trouble."),
    ("elevator", "Elevator out of service", "The elevator in [building] is out of service until [day]. If you need "
                                            "help with stairs or groceries, call the office and we will arrange it."),
    ("road", "Road closed", "[Street] is closed on [day] from [time] to [time] for [reason]. Please use [other way] "
                            "and park off the street."),
    ("storm", "Storm warning", "A storm is expected [day] evening. Please bring in loose items from outside, charge "
                               "your phone, and keep a flashlight handy. Call 911 in an emergency."),
]


def office_password(c_key):
    """The console password for a community (never shown on a page; a controller script prints it)."""
    return family.sig("office", c_key)[:10]


def _pk(c):
    return f"office#{c}"


def _today():
    return family.local(family.now()).date()


# --------------------------------------------------------------------------
# Anonymous stats
# --------------------------------------------------------------------------
def _answered(text):
    t = (text or "").lstrip().lstrip('"')
    return bool(t) and not t.startswith(NOT_ANSWERED)


def _private_words(c):
    """Words that could identify a homeowner: every name and email part in the community."""
    out = set()
    for e, p in COMMUNITIES.get(c, {}).get("homeowners", {}).items():
        out |= set(re.findall(r"[a-z]+", (p.get("name") or "").lower()))
        out |= set(re.findall(r"[a-z]+", e.lower()))
    return out


def topic_words(c, args):
    """Up to 8 lower-case content words from the question's free text; never a name or an email."""
    if not isinstance(args, dict):
        return []
    text = " ".join(str(args.get(k) or "") for k in TOPIC_KEYS).lower()
    text = re.sub(r"\S+@\S+", " ", text)
    bad = _private_words(c) | STOP
    words = []
    for w in re.findall(r"[a-z]{3,}", text):
        if w not in bad and w not in words:
            words.append(w)
    return words[:8]


def record(c_key, email_or_none, tool, args, result_text):
    """Called after every tool call. Keeps the day's anonymous counts. Never raises."""
    try:
        if c_key not in COMMUNITIES or not tool or tool in IGNORE:
            return                      # the board's own polling would only add writes
        today = _today().isoformat()
        sk = f"stat#{today}"
        d = state.get(_pk(c_key), sk) or {"day": today, "tools": {}, "topics": {}, "unanswered_topics": {},
                                          "answered": 0, "unanswered": 0, "actions": 0, "homes": {}}
        d["tools"][tool] = d["tools"].get(tool, 0) + 1
        text = result_text if isinstance(result_text, str) else str(result_text or "")
        ok = _answered(text)
        words = topic_words(c_key, args)
        units = words or [TOPIC_LABEL.get(tool, tool.replace("_", " "))]
        if tool in ACTIONS:
            if ok and ("(simulated)" in text or re.search(r"\b[A-Z]{2,4}-2026-\d{3,4}\b", text)) \
                    and "needs a family" not in text:
                d["actions"] += 1
        elif ok:
            d["answered"] += 1
        if ok:
            for u in units:
                d["topics"][u] = d["topics"].get(u, 0) + 1
        else:
            d["unanswered"] += 1
            phrase = " ".join(units)
            d["unanswered_topics"][phrase] = d["unanswered_topics"].get(phrase, 0) + 1
        if email_or_none:
            d["homes"][family.sig("office-home", c_key, email_or_none)[:10]] = 1
        state.put(_pk(c_key), sk, d, keep_days=STATS_DAYS)
    except Exception as x:          # noqa: BLE001 -- stats never cost an answer
        print("office.record failed:", x)


def calls_saved_hours(answered, actions):
    return (answered + actions) * MINUTES_PER_CALL / 60


def numbers(c, days):
    """Totals over the last `days` local days (today included)."""
    first = (_today() - dt.timedelta(days=days - 1)).isoformat()
    out = {"answered": 0, "unanswered": 0, "actions": 0, "homes": set(), "topics": Counter(),
           "unanswered_topics": Counter(), "tools": Counter()}
    for sk, d in state.query(_pk(c), "stat#"):
        if d.get("day", "") < first:
            continue
        for k in ("answered", "unanswered", "actions"):
            out[k] += d.get(k, 0)
        out["homes"] |= set(d.get("homes", {}))
        for k in ("topics", "unanswered_topics", "tools"):
            out[k].update(d.get(k, {}))
    out["homes"] = len(out["homes"])
    out["calls"] = out["answered"] + out["actions"]
    out["hours"] = calls_saved_hours(out["answered"], out["actions"])
    return out


def _by_day(c, days):
    have = {d.get("day"): d for _, d in state.query(_pk(c), "stat#")}
    today = _today()
    return [(today - dt.timedelta(days=i), have.get((today - dt.timedelta(days=i)).isoformat(), {}))
            for i in range(days - 1, -1, -1)]


# --------------------------------------------------------------------------
# Heard: the board tells us a message was shown and read aloud
# --------------------------------------------------------------------------
def message_heard(c_key, email, args):
    try:
        mid = str((args or {}).get("id") or "")[:32]
        if mid and re.fullmatch(r"[A-Za-z0-9_-]+", mid):
            state.put(family.hw_key(c_key, email), f"heard#{mid}", {"id": mid, "at": int(family.now())}, keep_days=90)
    except Exception as x:          # noqa: BLE001 -- the board never sees an error for this
        print("office.message_heard failed:", x)
    return "ok"


HIDDEN_TOOLS = [("message_heard", message_heard)]


def _heard(c, e, mid):
    return bool(mid) and state.get(family.hw_key(c, e), f"heard#{mid}") is not None


# --------------------------------------------------------------------------
# Announcements
# --------------------------------------------------------------------------
def announce(c, title, message, urgent):
    """Posts to every homeowner's panel; returns the stored announcement."""
    title, message = (title or "").strip()[:80], (message or "").strip()[:MSG_MAX]
    text = (title + ("" if title[-1:] in ".!?:" else ".") + " " + message) if title else message
    sender = "Urgent: The Board" if urgent else "The Board"
    msgs = {e: family.post_message(c, e, sender, text) for e in COMMUNITIES[c]["homeowners"]}
    t = family.now()
    a = {"id": secrets.token_hex(4), "title": title, "text": message, "urgent": bool(urgent), "at": int(t), "msgs": msgs}
    state.put(_pk(c), f"ann#{t:.3f}#{a['id']}", a, keep_days=365)
    return a


def announcements(c):
    """Newest first, each with heard (count), total and not_heard [address (lot)]."""
    out = []
    for _, a in reversed(state.query(_pk(c), "ann#")):
        people = COMMUNITIES[c]["homeowners"]
        heard = [e for e, mid in a["msgs"].items() if _heard(c, e, mid)]
        out.append({**a, "heard": len(heard), "total": len(a["msgs"]),
                    "not_heard": [_where(c, e) for e in a["msgs"] if e not in heard and e in people]})
    return out


def _where(c, e):
    p = COMMUNITIES[c]["homeowners"].get(e, {})
    lot = p.get("lot", "")
    return f"{p.get('address', 'a home')}" + (f" ({lot})" if lot and lot not in p.get("address", "") else "")


# --------------------------------------------------------------------------
# Requests
# --------------------------------------------------------------------------
def note_request(c_key, email, ref, kind, details):
    """The controller calls this when a homeowner files a request through stan-mcp."""
    try:
        if c_key in COMMUNITIES and ref:
            state.put(_pk(c_key), f"filed#{ref}", {"ref": ref, "email": email, "kind": kind or _kind(ref),
                                                    "details": str(details or "")[:300], "at": int(family.now())},
                      keep_days=365)
    except Exception as x:          # noqa: BLE001
        print("office.note_request failed:", x)


def _closed(status):
    """'Approved on ...', 'Fixed' close a request; 'not fixed yet', 'incomplete' don't."""
    return bool(re.search(r"(?<!not )\b(" + "|".join(CLOSED) + r")\b", (status or "").lower()))


def _kind(ref):
    return KINDS.get(str(ref).split("-")[0], "request")


def all_requests(c):
    """Every homeowner's requests with the latest status, newest update first, then the rest."""
    people = COMMUNITIES[c]["homeowners"]
    found = {}
    for e, p in people.items():
        for r in p.get("requests", []):
            found[r["id"]] = {"ref": r["id"], "email": e, "kind": r.get("kind", _kind(r["id"])),
                              "details": r.get("details", ""), "status": r.get("status", ""), "at": 0}
        for _, act in state.query(family.hw_key(c, e), "act#"):        # filed through stan-mcp, from family.log
            if act.get("tool") == "file_request" and act.get("ref") and act["ref"] not in found:
                what = act.get("what", "")
                found[act["ref"]] = {"ref": act["ref"], "email": e, "kind": _kind(act["ref"]),
                                     "details": what.split(": ", 1)[-1], "status": "Received; not reviewed yet.",
                                     "at": int(act.get("at", 0))}
    for _, f in state.query(_pk(c), "filed#"):
        if f.get("email") in people:
            found[f["ref"]] = {"ref": f["ref"], "email": f["email"], "kind": f.get("kind") or _kind(f["ref"]),
                               "details": f.get("details", ""), "status": "Received; not reviewed yet.",
                               "at": f.get("at", 0)}
    for _, u in state.query(_pk(c), "req#"):                          # oldest first: the last one wins
        r = found.get(u.get("ref"))
        if r and u.get("email") == r["email"]:
            r.update(status=u["status"], at=u["at"], updated=True, mid=u.get("mid"))
    out = list(found.values())
    for r in out:
        r["where"] = _where(c, r["email"])
        r["open"] = not _closed(r["status"])
        r["heard"] = _heard(c, r["email"], r.get("mid"))
    out.sort(key=lambda r: (-r["at"], r["ref"]))
    return out


def request_status(c_key, email, ref):
    """The latest status the office gave for this home's request, or for one it filed through stan-mcp; else None."""
    try:
        ups = [u for _, u in state.query(_pk(c_key), f"req#{ref}#") if u.get("email") == email]
        if ups:
            return ups[-1]["status"]
        f = state.get(_pk(c_key), f"filed#{ref}")
        return "Received; not reviewed yet." if f and f.get("email") == email else None
    except Exception:               # noqa: BLE001
        return None


def home_requests(c_key, email):
    """This home's requests (the record, filings and office updates), for my_requests: [{id, kind, details, status}]."""
    try:
        return [{"id": r["ref"], "kind": r["kind"], "details": r["details"], "status": r["status"]}
                for r in all_requests(c_key) if r["email"] == email]
    except Exception:               # noqa: BLE001
        return []


def update_request(c, ref, status, notify):
    r = next((x for x in all_requests(c) if x["ref"] == ref), None)
    if not r:
        return None
    t = family.now()
    mid = family.post_message(c, r["email"], "HOA Office",
                              f"Update on your {r['kind']} request {ref}: {status}") if notify else None
    state.put(_pk(c), f"req#{ref}#{t:.3f}", {"ref": ref, "email": r["email"], "kind": r["kind"], "status": status,
                                             "at": int(t), "notified": bool(notify), "mid": mid}, keep_days=365)
    return {**r, "status": status, "notified": bool(notify)}


# --------------------------------------------------------------------------
# Session
# --------------------------------------------------------------------------
def _cookie_value(c):
    exp = int(family.now()) + SESSION_S
    return f"{c}|{exp}|{family.sig('office-session', c, str(exp))}"


def _set_cookie(value, max_age=SESSION_S):
    return (f"office={urllib.parse.quote(value, safe='')}; HttpOnly; Secure; SameSite=Lax; Path=/office; "
            f"Max-Age={max_age}")


def _session(headers):
    """(community key, raw cookie value) for a valid, unexpired session cookie; else None."""
    raw = ""
    for part in (headers or {}).get("cookie", "").split(";"):
        k, _, v = part.strip().partition("=")
        if k == "office":
            raw = urllib.parse.unquote(v)
    try:
        c, exp, s = raw.rsplit("|", 2)
        if c in COMMUNITIES and int(exp) > family.now() and \
                hmac.compare_digest(s, family.sig("office-session", c, exp)):
            return c, raw
    except ValueError:
        pass
    return None


def _csrf(value):
    return family.sig("csrf", value)


# --------------------------------------------------------------------------
# Pages
# --------------------------------------------------------------------------
esc = html.escape
CSS = """
body{font-family:-apple-system,Segoe UI,Arial,sans-serif;background:#f4f5f7;color:#1d2330;margin:0;padding:16px}
main{max-width:860px;margin:0 auto;background:#fff;border-radius:14px;padding:24px;box-shadow:0 1px 3px #0002}
h1{font-size:24px;margin:0 0 12px} h2{font-size:19px;margin:26px 0 8px} p,li,td{font-size:16px;line-height:1.5}
textarea,input,select{width:100%;box-sizing:border-box;font-size:17px;padding:10px;border:1px solid #c8ccd4;
 border-radius:8px;margin:4px 0 12px;font-family:inherit}
input[type=checkbox]{width:auto;margin-right:8px}
button{font-size:17px;padding:12px 20px;border:0;border-radius:10px;color:#fff;background:#2f6fd0;cursor:pointer}
nav{display:flex;flex-wrap:wrap;gap:6px 14px;margin:0 0 18px;font-size:15px} nav a{color:#2f6fd0}
.small{color:#6b7380;font-size:14px} .tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:10px}
.tile{background:#f4f6fa;border-radius:10px;padding:12px} .tile b{display:block;font-size:26px}
.chart{display:flex;align-items:flex-end;gap:4px;height:130px;border-bottom:1px solid #c8ccd4;padding-top:8px}
.bar{flex:1;display:flex;flex-direction:column;justify-content:flex-end;align-items:center;font-size:11px;color:#6b7380}
.bar i{display:block;width:100%;background:#2f6fd0;border-radius:3px 3px 0 0} .bar i.u{background:#e0a33a;border-radius:0}
.days{display:flex;gap:4px} .days span{flex:1;text-align:center;font-size:11px;color:#6b7380}
.card{border:1px solid #e3e6eb;border-radius:10px;padding:12px;margin:8px 0} .urgent{border-color:#d0453a}
.tag{display:inline-block;font-size:12px;padding:2px 8px;border-radius:9px;background:#e8eefb;color:#2f6fd0}
.tag.red{background:#fbe9e8;color:#c0392b} .tag.grey{background:#eef0f3;color:#6b7380}
.ok{background:#eaf6ee;border-radius:10px;padding:12px} .err{background:#fbe9e8;border-radius:10px;padding:12px}
table{width:100%;border-collapse:collapse} td{padding:6px 4px;border-top:1px solid #eef0f3;vertical-align:top}
"""


def page(status, body, title="HOA office"):
    doc = ("<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,"
           f"initial-scale=1'><title>{esc(title)}</title><style>{CSS}</style></head><body><main>{body}</main>"
           "</body></html>")
    return {"statusCode": status, "headers": {"Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store",
                                              "X-Frame-Options": "DENY", "Referrer-Policy": "no-referrer"},
            "body": doc}


def redirect(location, cookie=None):
    r = {"statusCode": 302, "headers": {"Location": location, "Cache-Control": "no-store"}, "body": ""}
    if cookie:
        r["cookies"] = [cookie]                # what a Lambda function URL turns into Set-Cookie
        r["headers"]["Set-Cookie"] = cookie
    return r


def _nav(c):
    return (f"<p class='small'>{esc(COMMUNITIES[c]['profile']['name'])} &middot; office console</p>"
            "<nav><a href='/office/home'>Dashboard</a><a href='/office/announce'>Announce</a>"
            "<a href='/office/alert'>Urgent alert</a><a href='/office/requests'>Requests</a>"
            "<a href='/office/logout'>Sign out</a></nav>")


def _login_page(status=200, note=""):
    opts = "".join(f"<option value='{esc(k)}'>{esc(v['profile']['name'])}</option>" for k, v in COMMUNITIES.items())
    return page(status, f"<h1>HOA office sign in</h1>{note}<form method='post' action='/office/login'>"
                        f"<label>Community</label><select name='community'>{opts}</select>"
                        f"<label>Password</label><input type='password' name='password' autocomplete='current-password'>"
                        f"<button type='submit'>Sign in</button></form><p class='small'>For the board and the "
                        f"management company. The password comes from the stan-mcp operator.</p>")


def _hours(h):
    return f"{h:.1f} hours"


def _dashboard(c):
    today, week, month = numbers(c, 1), numbers(c, 7), numbers(c, 30)
    homes_total = len(COMMUNITIES[c]["homeowners"])
    days = _by_day(c, 14)
    top = max([d.get("answered", 0) + d.get("unanswered", 0) for _, d in days] + [1])
    bars = "".join(
        f"<div class='bar' title='{esc(family.say_date(day))}: {d.get('answered', 0)} answered, "
        f"{d.get('unanswered', 0)} not answered'>{(d.get('answered', 0) + d.get('unanswered', 0)) or ''}"
        f"<i class='u' style='height:{d.get('unanswered', 0) / top * 100:.0f}px'></i>"
        f"<i style='height:{d.get('answered', 0) / top * 100:.0f}px'></i></div>" for day, d in days)
    labels = "".join(f"<span>{family.DAYS[day.weekday()][:2]} {day.day}</span>" for day, _ in days)
    tiles = "".join(f"<div class='tile'><b>{v}</b>{esc(k)}</div>" for k, v in [
        ("questions answered today", today["answered"]), ("actions done today", today["actions"]),
        ("questions answered this week", week["answered"]), ("actions done this week", week["actions"]),
        ("homes using their panel this week", f"{week['homes']} of {homes_total}"),
        ("office calls saved this week", _hours(week["hours"]))])
    saved = (f"<p>This week {week['answered']} answered questions + {week['actions']} actions = {week['calls']} calls "
             f"the office didn't have to take; at an assumed {MINUTES_PER_CALL} minutes a call that is "
             f"<b>{_hours(week['hours'])}</b>. The last 30 days: {month['calls']} calls, "
             f"<b>{_hours(month['hours'])}</b>.</p>")
    topics = "".join(f"<li>{esc(t)} <span class='small'>({n})</span></li>" for t, n in month["topics"].most_common(10))
    unans = "".join(f"<li>{esc(t)} <span class='small'>({n})</span></li>"
                    for t, n in month["unanswered_topics"].most_common(10))
    open_r = [r for r in all_requests(c) if r["open"]]
    reqs = "".join(f"<tr><td><b>{esc(r['ref'])}</b><br><span class='small'>{esc(r['kind'])}</span></td>"
                   f"<td>{esc(r['where'])}<br><span class='small'>{esc(r['details'][:90])}</span></td>"
                   f"<td>{esc(r['status'])}</td></tr>" for r in open_r)
    anns = "".join(f"<li>{esc(a['title'] or a['text'][:50])} -- heard on {a['heard']} of {a['total']} panels</li>"
                   for a in announcements(c)[:5])
    import civic                                     # ballots and the next meeting's speakers and questions
    votes = ""
    for bid, r in civic.ballot_results(c).items():
        counts = ", ".join(f"{esc(str(k))}: {n}" for k, n in r["votes"].items()) or "no votes yet"
        votes += (f"<li><b>{esc(r['question'])}</b> -- {r['homes_voted']} of {r['homes']} homes voted"
                  f"{' (open)' if r.get('open') else ' (closed)'}<br><span class='small'>{counts}</span></li>")
    mr = civic.meeting_requests(c)
    speak = "".join(f"<li>{esc(x['address'])}: {esc(x['topic'])}</li>" for x in mr["speakers"])
    asks = "".join(f"<li>{esc(x['address'])}: {esc(x['question'])}</li>" for x in mr["questions"])
    civic_html = (f"<h2>Ballots</h2>{f'<ul>{votes}</ul>' if votes else '<p>None.</p>'}"
                  f"<h2>Next meeting: speakers</h2>{f'<ul>{speak}</ul>' if speak else '<p>No one signed up yet.</p>'}"
                  f"<h2>Questions for the open forum</h2>{f'<ul>{asks}</ul>' if asks else '<p>None yet.</p>'}")
    return page(200, f"{_nav(c)}<h1>Dashboard</h1><div class='tiles'>{tiles}</div>"
                     f"<h2>Office calls saved</h2>{saved}"
                     f"<h2>Questions, the last 14 days</h2><div class='chart'>{bars}</div><div class='days'>{labels}</div>"
                     f"<p class='small'>Blue: answered. Amber: not answered. \"This week\" is the last 7 days; the "
                     f"panel's own background checks (reminders, messages) are not counted.</p>"
                     f"<h2>Top topics, the last 30 days</h2>{f'<ol>{topics}</ol>' if topics else '<p>None yet.</p>'}"
                     f"<h2>Questions the panel couldn't answer</h2>"
                     f"{f'<ul>{unans}</ul>' if unans else '<p>None in the last 30 days.</p>'}"
                     f"<h2>Open requests ({len(open_r)})</h2>"
                     f"{f'<table>{reqs}</table>' if reqs else '<p>None open.</p>'}<p><a href='/office/requests'>"
                     f"Update a request</a></p><h2>Announcements</h2>"
                     f"{f'<ul>{anns}</ul>' if anns else '<p>None yet.</p>'}<p><a href='/office/announce'>Post an "
                     f"announcement</a></p>{civic_html}", "Dashboard")


def _announce_page(c, value, q, urgent_form, note=""):
    tmpl = {k: (t, m) for k, t, m in TEMPLATES}.get(q.get("t", ""))
    title, message = tmpl if tmpl else (q.get("title", ""), q.get("message", ""))
    urgent = urgent_form or q.get("urgent") == "on"
    picks = ""
    if urgent_form:
        picks = ("<p>Start from a template, then fill in the [brackets]: " + " &middot; ".join(
            f"<a href='/office/alert?t={k}'>{esc(t)}</a>" for k, t, _ in TEMPLATES) + "</p>")
    action = "/office/alert" if urgent_form else "/office/announce"
    form = (f"<form method='post' action='{action}'><input type='hidden' name='csrf' value='{_csrf(value)}'>"
            f"<label>Title</label><input name='title' maxlength='80' value='{esc(title)}'>"
            f"<label>Message (up to {MSG_MAX} characters)</label><textarea name='message' rows='5' "
            f"maxlength='{MSG_MAX}'>{esc(message)}</textarea>"
            f"<label><input type='checkbox' name='urgent'{' checked' if urgent else ''}>Urgent (the panel says "
            f"\"Urgent: The Board\")</label><p class='small'>Every panel in the community shows it and reads it "
            f"aloud. Keep it short and plain: the panel shows about the first {PANEL_SHOWS} characters, title "
            f"included.</p><button type='submit'>Send to every panel</button></form>")
    items = ""
    for a in announcements(c):
        missing = ", ".join(a["not_heard"]) or "everyone has heard it"
        items += (f"<div class='card{' urgent' if a['urgent'] else ''}'>"
                  f"{URGENT_TAG if a['urgent'] else ''}<b>{esc(a['title'])}</b> "
                  f"<span class='small'>{esc(family.say_date(family.local(a['at']).date()))}</span>"
                  f"<p>{esc(a['text'])}</p><p><span class='tag'>heard on {a['heard']} of {a['total']} panels</span>"
                  f"</p><p class='small'>Not heard yet: {esc(missing)}</p></div>")
    head = "Urgent alert" if urgent_form else "Announce"
    return page(200, f"{_nav(c)}<h1>{head}</h1>{note}{picks}{form}<h2>Sent</h2>{items or '<p>None yet.</p>'}", head)


def _requests_page(c, value, note=""):
    rows = ""
    for r in all_requests(c):
        tag = "<span class='tag'>open</span>" if r["open"] else "<span class='tag grey'>closed</span>"
        heard = ""
        if r.get("mid"):
            heard = " <span class='small'>(heard on the panel)</span>" if r["heard"] else \
                " <span class='small'>(not heard yet)</span>"
        rows += (f"<div class='card'><b>{esc(r['ref'])}</b> {tag} <span class='small'>{esc(r['kind'])} &middot; "
                 f"{esc(r['where'])}</span><p>{esc(r['details'])}</p><p><b>Status:</b> {esc(r['status'])}{heard}</p>"
                 f"<form method='post' action='/office/requests'><input type='hidden' name='csrf' "
                 f"value='{_csrf(value)}'><input type='hidden' name='ref' value='{esc(r['ref'])}'>"
                 f"<input name='status' maxlength='200' placeholder='New status, e.g. Approved on October 14'>"
                 f"<label><input type='checkbox' name='notify' checked>Tell the homeowner on their panel</label>"
                 f"<button type='submit'>Update</button></form></div>")
    return page(200, f"{_nav(c)}<h1>Requests</h1>{note}<p class='small'>Every homeowner's requests, newest update "
                     f"first. An update with the box ticked is shown and read aloud on that home's panel: \"Update on "
                     f"your ... request ...\".</p>{rows or '<p>No requests.</p>'}", "Requests")


def handle_office(path, method, q, headers):
    """Every /office path. q: query string + form, a dict of str. headers: lower-cased request headers."""
    try:
        return _handle(path.rstrip("/") or "/office", method, q or {}, headers or {})
    except Exception as x:          # noqa: BLE001
        print("office page failed:", x)
        return page(500, "<h1>Something went wrong</h1><p>Please try again.</p>")


def _handle(path, method, q, headers):
    if method not in ("GET", "POST"):
        return {"statusCode": 405, "headers": {"Allow": "GET, POST"}, "body": ""}
    if path == "/office/logout":
        return redirect("/office", _set_cookie("", 0))
    if path == "/office/login" and method == "POST":
        c = q.get("community", "")
        good = c in COMMUNITIES and os.environ.get("MCP_TOKEN") and \
            hmac.compare_digest((q.get("password") or "").strip(), office_password(c))
        if not good:
            return _login_page(403, "<p class='err'>That community and password don't match. Please try again.</p>")
        return redirect("/office/home", _set_cookie(_cookie_value(c)))
    s = _session(headers)
    if path in ("/office", "/office/login"):
        return redirect("/office/home") if s else _login_page()
    if not s:
        return redirect("/office")
    c, value = s
    if method == "POST" and not hmac.compare_digest(q.get("csrf", ""), _csrf(value)):
        return page(403, "<h1>That form has expired</h1><p>Please go back, reload the page and try again.</p>")
    if path == "/office/home":
        return _dashboard(c)
    if path in ("/office/announce", "/office/alert"):
        urgent_form = path == "/office/alert"
        if method == "POST":
            message = (q.get("message") or "").strip()
            if not message or "[" in message or len(message) > MSG_MAX:
                why = ("Fill in the [brackets] first." if "[" in message else
                       f"Keep the message to {MSG_MAX} characters." if message else "Write a message first.")
                return _announce_page(c, value, q, urgent_form, f"<p class='err'>{esc(why)}</p>")
            a = announce(c, q.get("title"), message, q.get("urgent") == "on")
            return _announce_page(c, value, {}, urgent_form,
                                  f"<p class='ok'>Sent to {len(a['msgs'])} panels. Each shows it and reads it aloud "
                                  f"within a few minutes of next being awake.</p>")
        return _announce_page(c, value, q, urgent_form)
    if path == "/office/requests":
        if method == "POST":
            ref, status = (q.get("ref") or "").strip(), (q.get("status") or "").strip()[:200]
            if not status:
                return _requests_page(c, value, "<p class='err'>Write the new status first.</p>")
            r = update_request(c, ref, status, q.get("notify") == "on")
            if not r:
                return page(404, f"{_nav(c)}<h1>No request {esc(ref)}</h1><p>It isn't one of this community's.</p>")
            told = " and told the homeowner on their panel" if r["notified"] else ""
            return _requests_page(c, value, f"<p class='ok'>Updated {esc(ref)}{told}.</p>")
        return _requests_page(c, value)
    return page(404, f"{_nav(c)}<h1>Not found</h1>")
