"""Friends and family: the people who look out for a homeowner.

A homeowner can have helpers (family_contacts / add_family_contact). stan-claw
can email them on the homeowner's behalf (notify_family), a weekly summary
goes to them when the homeowner agrees, a payment over $100 waits for one of
them to approve it, and every email carries a private link to a page where
they can write a message that shows on the homeowner's screen and is read
aloud. The board fetches those messages, and its spoken reminders, itself
(family_inbox and reminders, tools it calls directly, not listed to Claude).

The links are signed with MCP_TOKEN (HMAC), so they need no password and
cannot be forged; anyone holding one can only do what it says.

Times are the communities' local time (US Eastern, the DST rule written out
so nothing depends on the runtime's time-zone files).
"""
import datetime as dt
import hashlib
import hmac
import html
import json
import os
import re
import secrets
import time
import urllib.parse

import mail
import state
from hoa_data import COMMUNITIES

PAY_OK_LIMIT = 100.00          # payments above this wait for a family member's OK
DEMO = ("sheppard center", "info@avriz.io")
MONTHS = ["January", "February", "March", "April", "May", "June", "July", "August", "September", "October",
          "November", "December"]
DAYS = ["Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"]


def now():
    return time.time()


# --------------------------------------------------------------------------
# Local time
# --------------------------------------------------------------------------
def _nth_sunday(y, m, n):
    d = dt.date(y, m, 1)
    d += dt.timedelta(days=(6 - d.weekday()) % 7)
    return d + dt.timedelta(weeks=n - 1)


def _offset_h(ts):
    """-4 in summer time, -5 in winter (US rule: 2nd Sunday of March to 1st Sunday of November, 2 am)."""
    y = dt.datetime.fromtimestamp(ts, dt.timezone.utc).year
    start = dt.datetime.combine(_nth_sunday(y, 3, 2), dt.time(7), dt.timezone.utc).timestamp()
    end = dt.datetime.combine(_nth_sunday(y, 11, 1), dt.time(6), dt.timezone.utc).timestamp()
    return -4 if start <= ts < end else -5


def local(ts):
    return dt.datetime.fromtimestamp(ts + _offset_h(ts) * 3600, dt.timezone.utc).replace(tzinfo=None)


def at_local(d, hour, minute=0):
    """Unix time of local wall-clock d (a date) at hour:minute."""
    naive = dt.datetime.combine(d, dt.time(hour, minute)).replace(tzinfo=dt.timezone.utc).timestamp()
    return int(naive - _offset_h(naive + 5 * 3600) * 3600)


def say_date(d):
    return f"{DAYS[d.weekday()]}, {MONTHS[d.month - 1]} {d.day}"


def find_date(text, today):
    """The first 'October 17' (with an optional year) in text, as a date: this year, or next year if it
    passed more than 60 days ago. None when there is none."""
    m = re.search(r"(" + "|".join(MONTHS) + r") (\d{1,2})(?:, (\d{4}))?", text or "")
    if not m:
        return None
    mo, day = MONTHS.index(m.group(1)) + 1, int(m.group(2))
    year = int(m.group(3)) if m.group(3) else today.year
    try:
        d = dt.date(year, mo, day)
    except ValueError:
        return None
    if not m.group(3) and (today - d).days > 60:
        d = dt.date(year + 1, mo, day)
    return d


def find_time(text):
    """'7:00 pm' / '9 am' in text -> (hour, minute), or None."""
    m = re.search(r"(\d{1,2})(?::(\d{2}))? ?(am|pm)\b", text or "")
    if not m:
        return None
    h = int(m.group(1)) % 12 + (12 if m.group(3) == "pm" else 0)
    return h, int(m.group(2) or 0)


# --------------------------------------------------------------------------
# Who
# --------------------------------------------------------------------------
def hw_key(c, e):
    return f"hw#{c}|{e}"


def person(c, e):
    return COMMUNITIES[c]["homeowners"][e]


def who_line(c, e):
    p = person(c, e)
    return f"{p['name']} at {p['address']}, {COMMUNITIES[c]['profile']['name']}"


def helpers(c, e):
    got = [d for _, d in state.query(hw_key(c, e), "helper#")]
    if not got and (c, e) == DEMO and os.environ.get("FAMILY_EMAIL") and not state.get(hw_key(c, e), "helpers-set"):
        got = [{"name": os.environ.get("FAMILY_NAME", "Reza"), "relation": os.environ.get("FAMILY_RELATION", "family"),
                "email": os.environ["FAMILY_EMAIL"]}]
    return got


def names_of(hs):
    n = [h["name"] for h in hs]
    return n[0] if len(n) == 1 else ", ".join(n[:-1]) + " and " + n[-1] if n else "no one"


def consent(c, e):
    d = state.get(hw_key(c, e), "consent")
    return d["summary"] if d else (c, e) == DEMO


# --------------------------------------------------------------------------
# Signed links
# --------------------------------------------------------------------------
def sig(*parts):
    return hmac.new(os.environ.get("MCP_TOKEN", "").encode(), "|".join(parts).encode(), hashlib.sha256).hexdigest()[:32]


def base_url():
    return os.environ.get("PUBLIC_URL", "").rstrip("/")


def family_link(c, e, helper_email):
    h = f"{c}|{e}"
    q = urllib.parse.urlencode({"h": h, "f": helper_email, "s": sig("family", h, helper_email)})
    return f"{base_url()}/family?{q}"


def approve_link(aid):
    return f"{base_url()}/approve?" + urllib.parse.urlencode({"id": aid, "s": sig("approve", aid)})


def email_helpers(c, e, subject, text, extra_links=()):
    """Each helper gets their own copy, with their own message link."""
    sent = []
    for h in helpers(c, e):
        links = list(extra_links) + [(f"Send {person(c, e)['name']} a message on the screen", family_link(c, e, h["email"]))]
        if mail.send([h["email"]], subject, f"Hello {h['name']},\n\n" + text, links):
            sent.append(h["name"])
    return sent


def ticket(prefix, *parts):
    return f"{prefix}-2026-{int(hashlib.sha256('|'.join(parts).encode()).hexdigest()[:6], 16) % 9000 + 1000}"


# --------------------------------------------------------------------------
# Activity, for the weekly summary
# --------------------------------------------------------------------------
QUIET = {"family_inbox", "reminders", "send_family_summary", "family_summary_consent", "family_contacts",
         "pending_approvals", "family_photos", "family_media", "checkin_missed", "daily_checkin_consent",
         "message_heard", "open_ballots", "explain_violation", "meeting_companion", "plan_architectural"}


def describe(tool, a, result):
    amt = a.get("amount")
    return {
        "my_account": "checked their account balance",
        "violations": "looked at their violation notices",
        "my_requests": "looked at their open requests and bookings",
        "my_household": "looked at their household record",
        "make_payment": (f"asked to pay ${float(amt or 0):,.2f} (waiting for a family OK)" if "needs" in result
                         else f"paid ${float(amt or 0):,.2f} toward their balance"),
        "payment_plan": f"asked management for a {a.get('months', 3)}-month payment plan",
        "appeal_violation": f"appealed notice {str(a.get('violation_id', '')).upper()}",
        "file_request": f"filed {'an' if str(a.get('kind', '')).startswith('a') else 'a'} {a.get('kind', '')} request: "
                        f"{str(a.get('details', ''))[:80]}",
        "reserve_amenity": f"booked the {a.get('amenity', '')} for {a.get('when', '')}",
        "guest_pass": f"registered a guest, {a.get('guest_name', '')}, for {a.get('date', '')}",
        "update_household": f"updated their {a.get('kind', 'household')} record",
        "resale_certificate": "ordered a resale certificate",
        "notify_family": "sent family a message",
        "add_family_contact": f"added {a.get('name', 'someone')} as a family contact",
        "cast_vote": "voted on a community ballot",                    # never how they voted
        "request_extension": f"asked for more time on notice {str(a.get('violation_id', '')).upper()}",
        "register_to_speak": f"signed up to speak at the board meeting about {str(a.get('topic', ''))[:60]}",
        "submit_forum_question": "sent a question to the board's open forum",
    }.get(tool, f"used {tool}")


DOES = {"make_payment", "payment_plan", "appeal_violation", "file_request", "reserve_amenity", "guest_pass",
        "update_household", "resale_certificate", "cast_vote", "request_extension", "register_to_speak",
        "submit_forum_question"}


def log(c, e, tool, a, result):
    if tool in QUIET or (tool in DOES and "(simulated)" not in result
                         and not (tool == "make_payment" and "needs a family" in result)):
        return                                  # an action that didn't happen (a pool can't be booked) isn't news
    t = now()
    state.put(hw_key(c, e), f"act#{t:.3f}#{secrets.token_hex(3)}", {"at": t, "tool": tool, "what": describe(tool, a, result),
                                             "did": tool in DOES and "(simulated)" in result,
                                             "ref": (re.findall(r"\b[A-Z]{2,3}-2026-\d{4}\b", result) or [""])[0]},
              keep_days=90)


# --------------------------------------------------------------------------
# Messages for the screen
# --------------------------------------------------------------------------
def post_message(c, e, sender, text):
    t = now()
    mid = secrets.token_hex(4)
    state.put(hw_key(c, e), f"msg#{t:.3f}#{mid}", {"id": mid, "from": sender[:30], "text": text[:300], "at": int(t),
                                                   "delivered": False}, keep_days=30)
    return mid


# --------------------------------------------------------------------------
# The tools (each gets the community key c and email e of the signed-in homeowner)
# --------------------------------------------------------------------------
def t_family_contacts(c, e, a):
    hs = helpers(c, e)
    if not hs:
        return "No family contacts yet. add_family_contact adds one (name, relation, email)."
    return json.dumps([{k: h[k] for k in ("name", "relation", "email")} for h in hs])


def t_add_family_contact(c, e, a):
    name, email = (a.get("name") or "").strip(), (a.get("email") or "").strip().lower().replace(" at ", "@").replace(" dot ", ".").replace(" ", "")
    if not name or not re.fullmatch(r"[^@\s]+@[^@\s]+\.[a-z]{2,}", email):
        return "I need the person's name and a full email address."
    hs = helpers(c, e)
    if not state.get(hw_key(c, e), "helpers-set"):          # the demo's seeded contact becomes a real one
        for h in hs:
            state.put(hw_key(c, e), f"helper#{h['email']}", h)
        state.put(hw_key(c, e), "helpers-set", {"at": int(now())})
    state.put(hw_key(c, e), f"helper#{email}", {"name": name[:30], "relation": (a.get("relation") or "family")[:30],
                                                "email": email})
    return (f"Added {name} ({a.get('relation') or 'family'}) as a family contact. They can now get messages from "
            f"{person(c, e)['name']}, approve payments over ${PAY_OK_LIMIT:,.0f}, and send messages to the screen.")


def t_notify_family(c, e, a):
    msg = (a.get("message") or "").strip()
    if not msg:
        return "What would they like to say?"
    hs = helpers(c, e)
    want = (a.get("to") or "").lower()
    if want:
        hs = [h for h in hs if want in h["name"].lower() or want in h["relation"].lower()] or hs
    if not hs:
        return "There is no family contact to send it to yet. add_family_contact adds one."
    p = person(c, e)
    sent = []
    for h in hs:
        if mail.send([h["email"]], f"A message from {p['name']} via stan-claw",
                     f"Hello {h['name']},\n\n{p['name']} ({p['address']}) asked stan-claw to send you this:\n\n"
                     f"“{msg[:600]}”\n\nYou can reply with a message that shows on their screen and is read "
                     f"aloud to them.", [("Reply on their screen", family_link(c, e, h["email"]))]):
            sent.append(h["name"])
    return f"Sent to {names_of([{'name': n} for n in sent])} by email." if sent else "The email could not be sent just now."


def attention(c, e):
    d, p = COMMUNITIES[c], person(c, e)
    out = []
    bal = p["account"]["balance_due"]
    if bal > 0:
        out.append(f"Balance due: ${bal:,.2f} ({p['account']['detail']})")
    for v in p["violations"]:
        out.append(f"Notice {v['id']} ({v['rule']}): {v['status']}")
    out.append(f"Next board meeting: {d['meetings']['next']['when']}, {d['meetings']['next']['where']}.")
    for r in p["requests"]:
        out.append(f"Open request {r['id']}: {r['details']} -- {r['status']}")
    for _, ap in state.query(hw_key(c, e), "appr#"):
        full = state.get(f"approval#{ap['id']}", "approval")
        if full and full["status"] == "waiting":
            out.append(f"Waiting for your OK: a ${full['amount']:,.2f} payment ({approve_link(ap['id'])})")
    return out


def summary_text(c, e, days=7):
    since = now() - days * 86400
    acts = [d for _, d in state.query(hw_key(c, e), "act#") if d["at"] >= since]
    lines = [f"Here is this week's update for {who_line(c, e)}.", ""]
    if acts:
        lines.append("What they asked stan-claw about:")
        by_day = {}
        for a in acts:
            day = say_date(local(a["at"]).date())
            by_day.setdefault(day, [])
            if a["what"] not in by_day[day]:
                by_day[day].append(a["what"])
        lines += [f"- {day}: {'; '.join(w)}." for day, w in by_day.items()]
    else:
        lines.append("They did not ask stan-claw about HOA matters this week.")
    did = [a for a in acts if a["did"]]
    if did:
        lines += ["", "Filed or done (all simulated in this demo):"]
        lines += [f"- {a['what']}" + (f" (reference {a['ref']})" if a["ref"] else "") for a in did]
    lines += ["", "Worth keeping an eye on:"] + [f"- {x}" for x in attention(c, e)]
    return "\n".join(lines)


def send_summary(c, e):
    return email_helpers(c, e, f"Weekly update: {who_line(c, e)}", summary_text(c, e))


def t_send_family_summary(c, e, a):
    if not helpers(c, e):
        return "There is no family contact to send it to yet."
    sent = send_summary(c, e)
    return f"This week's summary was emailed to {names_of([{'name': n} for n in sent])}." if sent else "The email could not be sent just now."


def t_family_summary_consent(c, e, a):
    on = a.get("on")
    on = on if isinstance(on, bool) else str(on).lower() in ("true", "yes", "on", "1")
    state.put(hw_key(c, e), "consent", {"summary": on, "at": int(now())})
    return ("Done: a short weekly summary will go to their family contacts on Sunday mornings." if on
            else "Done: no weekly summary will be sent.")


def weekly_all():
    n = 0
    for c, d in COMMUNITIES.items():
        for e in d["homeowners"]:
            if consent(c, e) and helpers(c, e) and send_summary(c, e):
                n += 1
    return n


def t_reminders(c, e, a):
    """For the board: what to say on its own over the next 8 days (and the last 6 hours, to catch up)."""
    d, p = COMMUNITIES[c], person(c, e)
    t = now()
    today = local(t).date()
    out = []

    def add(rid, when, say):
        if t - 6 * 3600 <= when <= t + 8 * 86400:
            out.append({"id": rid, "at": when, "say": say})

    trash = d["schedule"].get("trash", "")
    pick = next((i for i, n in enumerate(DAYS) if n in trash), None)
    if pick is not None and "any time" not in trash:
        for k in range(0, 9):
            day = today + dt.timedelta(days=k)
            if day.weekday() == pick:
                add(f"trash-{day}", at_local(day - dt.timedelta(days=1), 18),
                    f"Tonight is trash night. The bins can go out to the curb now; pickup is tomorrow, {DAYS[pick]}.")
    nxt = d["meetings"]["next"]
    md = find_date(nxt["when"], today)
    if md:
        tm = find_time(nxt["when"])
        add(f"meeting-{md}", at_local(md, 10),
            f"The board meeting is today{' at ' + nxt['when'].split(', ')[-1] if tm else ''}, at {nxt['where']}.")
    due = d["dues"].get("due", "")
    if "1st" in due:
        first = (today.replace(day=1) + dt.timedelta(days=32)).replace(day=1)
        dd = first
    else:
        dd = find_date(due, today)
    if dd:
        amount = d["dues"].get("monthly") or d["dues"].get("annual")
        add(f"dues-{dd}", at_local(dd - dt.timedelta(days=3), 10),
            f"A reminder: your HOA dues of ${amount:,.0f} are due on {say_date(dd)}."
            + (f" Your balance right now is ${p['account']['balance_due']:,.0f}." if p["account"]["balance_due"] > 0 else ""))
    for v in p["violations"]:
        st = v["status"]
        m = re.search(r"by ((?:" + "|".join(MONTHS) + r") \d{1,2})", st)
        if m:
            vd = find_date(m.group(1), today)
            add(f"fix-{v['id']}", at_local(vd - dt.timedelta(days=2), 10),
                f"A reminder about the {v['rule']} notice: it needs fixing by {say_date(vd)}. {st.split('. ')[-1]}")
        m = re.search(r"[Hh]earing[^.]*? on ((?:" + "|".join(MONTHS) + r") \d{1,2})", st)
        if m:
            hd = find_date(m.group(1), today)
            add(f"hearing-{v['id']}", at_local(hd - dt.timedelta(days=1), 10),
                f"Tomorrow, {say_date(hd)}, the board hears the {v['rule']} notice at its meeting. You may go and explain.")
    for r in p["reservations"]:
        rd = find_date(r["when"], today)
        if rd:
            add(f"booking-{r['amenity']}-{rd}", at_local(rd - dt.timedelta(days=1), 16),
                f"Tomorrow you have the {r['amenity']} booked: {r['when']}.")
    out.sort(key=lambda x: x["at"])
    return json.dumps({"reminders": out, "checkin": checkin_on(c, e)})


def t_family_inbox(c, e, a):
    """For the board: messages from family not yet shown; marks them delivered unless take is false."""
    take = a.get("take", True) not in (False, "false", 0)
    out = []
    for sk, m in state.query(hw_key(c, e), "msg#"):
        if not m["delivered"]:
            out.append({**{k: m[k] for k in ("id", "from", "text", "at")}, **({"photo": m["photo"]} if m.get("photo") else {})})
            if take:
                state.put(hw_key(c, e), sk, {**m, "delivered": True}, keep_days=30)
    return json.dumps({"messages": out})


PHOTO_MAX = 150 * 1024          # bytes of JPEG, after the page shrinks it to 480 x 480 (the board's flash is small)
PHOTOS_KEPT = 10


def add_photo(c, e, sender, jpeg, caption):
    t = now()
    pid = secrets.token_hex(4)
    state.put(hw_key(c, e), f"photo#{t:.3f}#{pid}", {"id": pid, "from": sender[:30], "caption": caption[:120],
                                                      "at": int(t)}, keep_days=120, blob=jpeg)
    for sk, _ in state.query(hw_key(c, e), "photo#")[:-PHOTOS_KEPT]:        # the frame keeps the newest ten
        state.delete(hw_key(c, e), sk)
    state.put(hw_key(c, e), f"msg#{t:.3f}#{pid}", {"id": pid, "from": sender[:30], "text": caption[:300],
                                                   "at": int(t), "photo": pid, "delivered": False}, keep_days=30)
    return pid


def jpeg_ok(b):
    """A baseline JPEG the board can decode: the JPEG start bytes, no progressive frame (SOF2)."""
    return len(b) <= PHOTO_MAX and b[:3] == b"\xff\xd8\xff" and b"\xff\xc2" not in b


def t_family_photos(c, e, a):
    """For the board: the photo frame's pictures, newest last."""
    return json.dumps({"photos": [{"id": d["id"], "from": d["from"], "caption": d["caption"], "at": d["at"]}
                                  for _, d in state.query(hw_key(c, e), "photo#")]})


def t_family_media(c, e, a):
    """For the board: one photo, as base64."""
    import base64
    for sk, d in state.query(hw_key(c, e), "photo#"):
        if d["id"] == a.get("id"):
            b = state.get_blob(hw_key(c, e), sk)
            return json.dumps({"id": d["id"], "kind": "jpeg", "b64": base64.b64encode(b).decode() if b else ""})
    return json.dumps({"id": a.get("id"), "kind": "", "b64": ""})


def checkin_on(c, e):
    d = state.get(hw_key(c, e), "checkin")
    return bool(d and d["on"])


def t_daily_checkin_consent(c, e, a):
    on = a.get("on")
    on = on if isinstance(on, bool) else str(on).lower() in ("true", "yes", "on", "1")
    state.put(hw_key(c, e), "checkin", {"on": on, "at": int(now())})
    return ("Done: on weekday mornings stan-claw will say good morning around 11, and if it hears nothing back "
            "it will let their family know by email." if on else "Done: no morning check-in.")


def t_checkin_missed(c, e, a):
    """For the board: nobody answered the morning check-in. Emails the helpers, once a day."""
    day = str(local(now()).date())
    if state.get(hw_key(c, e), f"checkin-sent#{day}"):
        return "Already sent today."
    if not helpers(c, e):
        return "No family contact to tell."
    p = person(c, e)
    sent = email_helpers(
        c, e, f"stan-claw hasn't heard from {p['name']} this morning",
        f"stan-claw said good morning to {p['name']} at {p['address']} today ({a.get('asked', 'this morning')}) and "
        f"again a little later, and heard nothing back, and nobody has touched the screen since early morning.\n\n"
        f"This may well be nothing -- they may simply be out, or in another room. You may want to give them a call.")
    state.put(hw_key(c, e), f"checkin-sent#{day}", {"at": int(now())}, keep_days=7)
    return f"Told {names_of([{'name': n} for n in sent])}." if sent else "The email could not be sent just now."


def ask_approval(c, e, amount):
    """A payment over the limit: waits for a family OK. Returns the text for Claude."""
    hs = helpers(c, e)
    p = person(c, e)
    if not hs:
        return (f"Payments over ${PAY_OK_LIMIT:,.0f} need a family member's OK first, and no family contact is set up. "
                f"They can pay ${PAY_OK_LIMIT:,.0f} or less now, or add a family contact (add_family_contact).")
    aid = secrets.token_hex(5)
    state.put(f"approval#{aid}", "approval", {"id": aid, "c": c, "e": e, "amount": amount, "status": "waiting",
                                              "at": int(now())}, keep_days=60)
    state.put(hw_key(c, e), f"appr#{aid}", {"id": aid}, keep_days=60)
    sent = email_helpers(
        c, e, f"Please approve: ${amount:,.2f} HOA payment for {p['address']}",
        f"{p['name']} asked stan-claw to pay ${amount:,.2f} toward their {COMMUNITIES[c]['profile']['name']} HOA "
        f"balance (${p['account']['balance_due']:,.2f} due).\n\nPayments over ${PAY_OK_LIMIT:,.0f} wait for a family "
        f"member's OK, to guard against mistakes and scams. Nothing is paid until you approve.",
        [("Approve or decline", approve_link(aid))])
    return (f"This ${amount:,.2f} payment needs a family member's OK first -- a safety check on payments over "
            f"${PAY_OK_LIMIT:,.0f}. I emailed {names_of([{'name': n} for n in sent]) if sent else 'their family'}; once "
            f"approved it goes through, and a message will show on the screen. Nothing has been paid yet. "
            f"(Approval {aid})")


def t_pending_approvals(c, e, a):
    out = []
    for _, ap in state.query(hw_key(c, e), "appr#"):
        full = state.get(f"approval#{ap['id']}", "approval")
        if full:
            out.append({"approval": full["id"], "amount": full["amount"], "status": full["status"],
                        **({"confirmation": full["confirmation"]} if full.get("confirmation") else {})})
    return json.dumps(out or "No payments waiting.")


# --------------------------------------------------------------------------
# The two pages family open from their email (no bearer token: the link is signed)
# --------------------------------------------------------------------------
PAGE = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>stan-claw</title><style>
body{{font-family:-apple-system,Segoe UI,Arial,sans-serif;background:#f4f5f7;color:#1d2330;margin:0;padding:24px}}
main{{max-width:520px;margin:0 auto;background:#fff;border-radius:14px;padding:28px;box-shadow:0 1px 3px #0002}}
h1{{font-size:24px;margin:0 0 12px}} p{{font-size:18px;line-height:1.5}}
textarea,input{{width:100%;box-sizing:border-box;font-size:18px;padding:12px;border:1px solid #c8ccd4;border-radius:8px;margin:6px 0 14px}}
button{{font-size:19px;padding:14px 22px;border:0;border-radius:10px;color:#fff;background:#2f6fd0;margin-right:10px;cursor:pointer}}
button.no{{background:#8a929e}} .small{{color:#8a929e;font-size:14px}}
</style></head><body><main>{body}</main></body></html>"""


# Shrinks the chosen photo to fit 480 x 480 (the screen) as a baseline JPEG, in the browser, before it is sent.
PHOTO_JS = """<script>
document.getElementById('ph').addEventListener('submit', function (ev) {
  var f = document.getElementById('pf').files[0];
  if (document.getElementById('pd').value) return;
  ev.preventDefault();
  if (!f) { alert('Choose a photo first'); return; }
  var img = new Image();
  img.onload = function () {
    var k = Math.min(480 / img.width, 480 / img.height, 1), c = document.createElement('canvas');
    c.width = Math.round(img.width * k); c.height = Math.round(img.height * k);
    c.getContext('2d').drawImage(img, 0, 0, c.width, c.height);
    var q = 0.85, d = c.toDataURL('image/jpeg', q);
    while (d.length > 190000 && q > 0.3) { q -= 0.1; d = c.toDataURL('image/jpeg', q); }
    document.getElementById('pd').value = d;
    document.getElementById('pb').textContent = 'Sending...';
    document.getElementById('ph').submit();
  };
  img.src = URL.createObjectURL(f);
});
</script>"""


def page(status, body):
    return {"statusCode": status, "headers": {"Content-Type": "text/html; charset=utf-8", "Cache-Control": "no-store"},
            "body": PAGE.format(body=body)}


def handle_page(path, method, q):
    """/family and /approve. q: the query string, or for a POST the form, as a dict of strings."""
    esc = html.escape
    if path == "/family":
        h, f = q.get("h", ""), q.get("f", "")
        if not hmac.compare_digest(q.get("s", ""), sig("family", h, f)) or "|" not in h:
            return page(403, "<h1>This link isn't valid</h1><p>Please use the link from the latest email.</p>")
        c, e = h.split("|", 1)
        if c not in COMMUNITIES or e not in COMMUNITIES[c]["homeowners"]:
            return page(404, "<h1>Not found</h1>")
        p = person(c, e)
        me = next((x for x in helpers(c, e) if x["email"] == f), {"name": "Family"})
        hidden = "".join(f"<input type='hidden' name='{k}' value='{esc(q.get(k, ''))}'>" for k in ("h", "f", "s"))
        if method == "POST" and q.get("photo"):
            import base64
            try:
                jpeg = base64.b64decode(q["photo"].split(",", 1)[-1], validate=False)
            except ValueError:
                jpeg = b""
            if not jpeg_ok(jpeg):
                return page(400, "<h1>That photo didn't work</h1><p>Please try another picture (a JPEG or a phone "
                                 "photo; the page shrinks it for the screen).</p>")
            add_photo(c, e, (q.get("from") or me["name"]).strip() or me["name"], jpeg, (q.get("caption") or "").strip())
            return page(200, f"<h1>Photo sent</h1><p>{esc(p['name'])} will see it on the stan-claw screen within a few "
                             f"minutes of it next being awake, and it joins the pictures shown while the screen "
                             f"rests.</p>")
        if method == "POST":
            text = (q.get("text") or "").strip()
            if not text:
                return page(400, "<h1>The message was empty</h1><p>Go back and write a few words.</p>")
            waiting = [m for _, m in state.query(hw_key(c, e), "msg#") if not m["delivered"]]
            if len(waiting) >= 20:
                return page(429, "<h1>Plenty waiting already</h1><p>The screen has 20 unread messages; try later.</p>")
            post_message(c, e, (q.get("from") or me["name"]).strip() or me["name"], text)
            return page(200, f"<h1>Sent</h1><p>{esc(p['name'])} will see it on the stan-claw screen, read aloud, "
                             f"within a few minutes of the screen next being awake.</p>"
                             f"<p class='small'>Messages wait while it sleeps (evenings and weekends).</p>")
        return page(200, f"<h1>A message for {esc(p['name'])}</h1><p>It appears on the stan-claw screen at "
                         f"{esc(p['address'])} and is read aloud. Keep it short and warm.</p>"
                         f"<form method='post'>{hidden}<label>From</label><input name='from' maxlength='30' "
                         f"value='{esc(me['name'])}'><label>Message</label><textarea name='text' rows='4' "
                         f"maxlength='300' placeholder='Hi! I will call you tonight at 7.'></textarea>"
                         f"<button type='submit'>Send to the screen</button></form>"
                         f"<h1 style='margin-top:34px'>A photo for {esc(p['name'])}</h1><p>It shows on the screen, and "
                         f"then while the screen rests, with the other recent photos.</p>"
                         f"<form method='post' id='ph'>{hidden}<input type='hidden' name='photo' id='pd'>"
                         f"<input type='hidden' name='from' value='{esc(me['name'])}'>"
                         f"<input type='file' id='pf' accept='image/*'><label>Caption (optional)</label>"
                         f"<input name='caption' maxlength='120' placeholder='The kids at the lake'>"
                         f"<button type='submit' id='pb'>Send the photo</button></form>{PHOTO_JS}")
    if path == "/approve":
        aid = q.get("id", "")
        if not hmac.compare_digest(q.get("s", ""), sig("approve", aid)):
            return page(403, "<h1>This link isn't valid</h1>")
        ap = state.get(f"approval#{aid}", "approval")
        if not ap:
            return page(404, "<h1>This request has expired</h1>")
        c, e = ap["c"], ap["e"]
        p = person(c, e)
        if ap["status"] != "waiting":
            return page(200, f"<h1>Already {esc(ap['status'])}</h1><p>The ${ap['amount']:,.2f} payment was "
                             f"{esc(ap['status'])}. Nothing more to do.</p>")
        if method == "POST" and q.get("d") in ("yes", "no"):
            fam = helpers(c, e)
            by = fam[0]["name"] if fam else "Family"
            if q["d"] == "yes":
                conf = ticket("PAY", e, str(ap["amount"]), aid)
                state.put(f"approval#{aid}", "approval", {**ap, "status": "approved", "confirmation": conf,
                                                          "decided": int(now())}, keep_days=60)
                post_message(c, e, by, f"I approved your ${ap['amount']:,.0f} HOA payment. It's done -- confirmation "
                                       f"{conf}.")
                return page(200, f"<h1>Approved</h1><p>The ${ap['amount']:,.2f} payment is made (simulated), "
                                 f"confirmation {esc(conf)}. {esc(p['name'])} will see a note on the screen.</p>")
            state.put(f"approval#{aid}", "approval", {**ap, "status": "declined", "decided": int(now())}, keep_days=60)
            post_message(c, e, by, f"Let's hold off on the ${ap['amount']:,.0f} HOA payment for now. I'll give you "
                                   f"a call about it.")
            return page(200, f"<h1>Declined</h1><p>Nothing was paid. {esc(p['name'])} will see a note on the screen "
                             f"saying you'll call.</p>")
        form = "".join(f"<input type='hidden' name='{k}' value='{esc(q.get(k, ''))}'>" for k in ("id", "s"))
        return page(200, f"<h1>Approve a payment?</h1><p><b>{esc(p['name'])}</b> ({esc(p['address'])}) asked "
                         f"stan-claw to pay <b>${ap['amount']:,.2f}</b> toward their HOA balance of "
                         f"${p['account']['balance_due']:,.2f}.</p><form method='post'>{form}"
                         f"<button name='d' value='yes'>Approve</button><button class='no' name='d' value='no'>"
                         f"Decline</button></form><p class='small'>A demo service: no money moves.</p>")
    return page(404, "<h1>Not found</h1>")
