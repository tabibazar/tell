"""Civic life in the HOA, for stan-claw: guided architectural requests, notices
explained in plain words, a companion for board meetings, voting by voice, and
same-day alerts from the community's notices.

Exports (the controller wires them into lambda_function.py):
  CIVIC_TOOLS                [(name, description, properties, required, kind, fn)]
                             kind "public":  fn(c_key, args)
                             kind "private": fn(c_key, email, args)
  notice_reminders(c, e, now_ts) -> [{"id", "at", "say"}]   for the board's reminders
  meeting_requests(c)        -> {"speakers": [...], "questions": [...]}   for the office console
  ballot_results(c)          -> {ballot_id: {...}}                         for the office console
  BALLOTS                    the ballots, per community

What it remembers (state, pk family.hw_key(c, e)):
  sk "ext#<violation id>"    a request for more time on a notice
  sk "speak#<meeting date>"  signed up to speak at that meeting (one per meeting; a second call updates it)
  sk "ask#<time>#<rnd>"      a question for the open forum
  sk "vote#<ballot id>"      this home's vote {choice, at} (one per home; changeable until the ballot closes)

Everything here is simulated: nothing is really filed, and votes are a demo.
"""
import datetime as dt
import json
import re
import secrets

import family
import library
import state
from hoa_data import COMMUNITIES


def j(o):
    return json.dumps(o, ensure_ascii=False)


def today():
    return family.local(family.now()).date()


def lib():
    return library.lib()                     # looked up per call, so a test can swap the library


def clip(text, n):
    text = re.sub(r"\s+", " ", text or "").strip()
    return text if len(text) <= n else text[:n - 1].rsplit(" ", 1)[0] + "…"


def sentences(text):
    return [s.strip() for s in re.split(r"(?<!\bMr\.)(?<!\bMs\.)(?<!\bDr\.)(?<!\bMrs\.)(?<!\bSt\.)(?<=[.!?])\s+|\n+",
                                        text or "") if s.strip()]


def norm_colour(s):
    s = (s or "").lower().replace("grey", "gray").replace("colour", "color")
    s = re.sub(r"\b(the|paint|painted|color|shade)\b", " ", s)
    return re.sub(r"\s+", " ", re.sub(r"[^a-z0-9 ]", " ", s)).strip()


def arch_sections(c):
    """Every section of the architectural guidelines (all documents if there are none)."""
    docs = lib().docs.get(c, [])
    arch = [d for d in docs if d["type"] == "architectural"] or docs
    return [(d, s) for d in arch for s in d["sections"]]


def search(c, query, k, doc_type=None):
    hits = lib().search(c, query, k=k, doc_type=doc_type) if doc_type else []
    return hits or lib().search(c, query, k=k)


def violation(p, vid):
    vid = (vid or "").strip().upper()
    return next((v for v in p["violations"] if v["id"].upper() == vid), None)


def is_open(v):
    return not re.match(r"\s*(resolved|closed|dismissed)", v["status"], re.I)


def unknown_violation(p, vid):
    open_ = [v["id"] for v in p["violations"] if is_open(v)]
    return (f"I can't find a notice {vid or ''} on this account. "
            + (f"The open ones are: {', '.join(open_)}." if open_ else "There are no open notices."))


# ==========================================================================
# 1. Guided architectural request
# ==========================================================================
PROJECTS = ["fence", "shed", "deck", "patio", "paint", "solar", "addition", "window", "door", "roof", "other"]
PROJECT_WORDS = {"fence": ["fence"], "shed": ["shed", "outbuilding"], "deck": ["deck"], "patio": ["patio"],
                 "addition": ["addition"], "other": []}
RULE_KEY = {"fence": "fences", "shed": "sheds", "paint": "paint", "solar": "solar"}

APP_ITEMS = [   # (what the guideline text mentions, what that means for the application)
    (r"sketch|drawing|site plan|plot plan", "A simple drawing or site sketch showing where it goes, with measurements"),
    (r"photo", "Photographs of the spot as it is now"),
    (r"sample|chip", "The paint chip or material sample"),
    (r"specification|brochure|manufacturer", "The maker's specifications or brochure"),
    (r"contractor", "The contractor's name and proof of insurance"),
    (r"neighbo", "A note to the neighbours next door"),
    (r"dimension|measure", "The size and height"),
    (r"material", "The materials"),
    (r"colou?r", "The colour, by name and code"),
    (r"location|placement", "Where on the lot it goes"),
]
DEFAULT_CHECKLIST = ["A short description of the project", "A simple drawing showing where it goes, with measurements",
                     "The materials", "The colour, by name and code", "The contractor's name and proof of insurance",
                     "A note to the neighbours next door"]
PROJECT_ITEMS = {
    "fence": ["The fence height and material", "Whether it is in the front, back or side yard, and where the gate goes"],
    "shed": ["The shed's floor size and height", "Where it sits behind the house", "Its colour, matching the house"],
    "deck": ["The deck's size and height off the ground", "The materials and railing"],
    "patio": ["The patio's size and surface material"],
    "paint": ["The colour names for body, trim and door", "The paint chip or can label"],
    "solar": ["A roof layout of the panels", "The installer's name"],
    "addition": ["Architectural plans", "The county building permit, when it comes"],
    "window": ["The window style and frame colour"], "door": ["The door style and colour"],
    "roof": ["The roofing material and colour"],
}
FEET = re.compile(r"(\d+(?:\.\d+)?)[- ]?(?:feet|foot|ft)\b")
LOCS = {"front": r"\bfront\b", "back": r"\b(back|rear)\b", "side": r"\bside\b"}


def height_limits(texts, words):
    """[(feet, location or None, cite)] from clauses that name the project and a height limit."""
    out = []
    for text, cite in texts:
        for clause in re.split(r"[.;]\s+|\n", text):
            low = clause.lower()
            low = re.sub(r"\d+(?:\.\d+)? square (?:feet|foot|ft)", " ", low)
            if not any(w in low for w in words) or not re.search(r"tall|high|height|exceed", low):
                continue
            loc = next((k for k, rx in LOCS.items() if re.search(rx, low)), None)
            for m in FEET.finditer(low):
                out.append((float(m.group(1)), loc, cite))
    return out


def height_concern(c, project, height, location):
    words = PROJECT_WORDS.get(project)
    if not words or height is None:
        return None
    texts = [(s["text"], library.cite(d, s)) for d, s in arch_sections(c)
             if any(w in (s["title"] + " " + s["text"]).lower() for w in words)]
    limits = height_limits(texts, words)
    if not limits and RULE_KEY.get(project) in COMMUNITIES[c]["rules"]:
        limits = height_limits([(COMMUNITIES[c]["rules"][RULE_KEY[project]], "the community rules")], words)
    if not limits:
        return None
    where = next((k for k, rx in LOCS.items() if re.search(rx, (location or "").lower())), None)
    fit = [x for x in limits if x[1] == where] if where else []
    fit = fit or [x for x in limits if x[1] is None] or limits
    hi, lo = max(fit), min(fit)
    h = f"{height:g}-foot {project}"
    if height > hi[0]:
        return (f"A {h} is taller than the {hi[0]:g} feet the guidelines allow ({hi[2]}). "
                f"Check with the guideline below.")
    if height > lo[0]:
        return (f"A {h} may be too tall: the limit is {lo[0]:g} feet" + (f" in a {lo[1]} yard" if lo[1] else "")
                + f" and {hi[0]:g} feet" + (f" in a {hi[1]} yard" if hi[1] else "")
                + f" ({lo[2]}). Ask where it will go, and check with the guideline below.")
    return None


def palette(c):
    """[(sentence or table line listing approved colours, cite)] from the guidelines."""
    out = []
    for d, s in arch_sections(c):
        if not re.search(r"colou?r|palette", (s["title"] + " " + s["text"]).lower()):
            continue
        for sent in sentences(s["text"]):
            low = sent.lower()
            if (low.startswith("|") and not re.match(r"\|[\s|:-]*\|?$", low)) or \
                    re.search(r"\b(approved|options?|palette|schemes?)\b[^.]*?(\bare\b|:)", low) and \
                    len(re.findall(r"\b[A-Z][a-z]+", sent)) >= 4:
                out.append((sent, library.cite(d, s)))
    return out


def colour_concern(c, colour):
    want = norm_colour(colour)
    if not want:
        return None
    pal = palette(c)
    if not pal:
        return None
    rx = re.compile(r"\b" + re.escape(want) + r"\b")
    if any(rx.search(norm_colour(sent)) for sent, _ in pal):
        return None
    cites = sorted({ct for _, ct in pal})
    warn = ""
    for d, s in arch_sections(c):
        for sent in sentences(s["text"]):
            if rx.search(norm_colour(sent)) and re.search(r"\bnot\b", sent.lower()):
                warn = f" The guidelines say: \"{clip(sent, 220)}\" ({library.cite(d, s)})."
                break
        if warn:
            break
    return (f"\"{colour}\" is not named among the approved colours ({'; '.join(cites)}).{warn} "
            f"A colour off the list needs a variance. Check with the guideline below.")


def material_concern(c, project, material):
    m = (material or "").lower().strip()
    if not m:
        return None
    words = PROJECT_WORDS.get(project) or [project]
    for d, s in arch_sections(c):
        for sent in sentences(s["text"]):
            low = sent.lower()
            if m in low and any(w in low for w in words) and \
                    re.search(r"not (permitted|allowed)|prohibited|\bno\b|may not", low):
                return f"The guidelines may not allow {material}: \"{clip(sent, 220)}\" ({library.cite(d, s)}). " \
                       f"Check with the guideline below."
    return None


def review_info(c):
    d = COMMUNITIES[c]
    rule = d["rules"].get("architectural review", "")
    hits = search(c, "review timeline respond days deemed approval", 3, "architectural")
    hit = next((h for h in hits if re.search(r"\d+ days", h["text"])), None)
    text = (hit["text"] if hit else "") + " " + rule
    m = re.search(r"within (\d+) (business )?days", text)
    days = int(m.group(1)) if m else None
    deemed = bool(re.search(r"deemed approv|counts as approval", text, re.I))
    meets = next((v for k, v in d["committees"].items() if "architectur" in k or "review" in k), "")
    say = (f"The committee answers within {days} {m.group(2) or ''}days of a complete application. " if days
           else "Ask management how long the committee takes. ")
    if deemed:
        say += f"If it has not answered in {days or 30} days, the request counts as approved, exactly as filed. "
    if meets:
        say += f"The committee: {meets}"
    return {"say": say.strip(), "days": days, "no_answer_counts_as_approval": deemed,
            "cite": hit["cite"] if hit else "the community rules"}


def checklist(c, project):
    hits = search(c, "how to apply application submit request attach form", 2, "architectural")
    text = " ".join(h["text"] for h in hits).lower()
    items = [say for rx, say in APP_ITEMS if re.search(rx, text)]
    if len(items) < 3:
        items = list(DEFAULT_CHECKLIST)
    for x in PROJECT_ITEMS.get(project, []):
        if x not in items:
            items.append(x)
    return items, ([h["cite"] for h in hits] if hits else [])


def num(x):
    try:
        return float(re.findall(r"\d+(?:\.\d+)?", str(x))[0]) if x not in (None, "") else None
    except IndexError:
        return None


def t_plan_architectural(c, e, a):
    project = (a.get("project") or "other").strip().lower()
    project = next((p for p in PROJECTS if project.startswith(p) or p in project), "other")
    height = num(a.get("height_ft"))
    material, colour, location = (str(a.get(k) or "").strip() for k in ("material", "color", "location"))
    size, details = str(a.get("size") or "").strip(), str(a.get("details") or "").strip()

    q = " ".join(x for x in (project, material, colour, location) if x)
    guidelines = [{"cite": h["cite"], "text": clip(h["text"], 700)} for h in search(c, q, 3, "architectural")]
    items, from_ = checklist(c, project)
    concerns = [x for x in (height_concern(c, project, height, location), colour_concern(c, colour),
                            material_concern(c, project, material)) if x]
    ask = {"fence": ["height_ft", "material", "color", "location"], "shed": ["size", "height_ft", "color", "location"],
           "deck": ["size", "height_ft", "material"], "patio": ["size", "material"], "paint": ["color"],
           "solar": ["location"], "addition": ["size", "details"], "window": ["material", "color"],
           "door": ["color"], "roof": ["material", "color"]}.get(project, ["details"])
    missing = [k for k in ask if not a.get(k)]
    summary = ", ".join(x for x in (project, f"{height:g} ft high" if height is not None else "", size, material,
                                     colour, location, details) if x)
    return j({
        "project": project,
        "checklist": items, "checklist_from": from_,
        "guidelines": guidelines,
        "concerns": concerns or ["Nothing stands out against the guidelines below."],
        "still_to_ask": missing,
        "review": review_info(c),
        "next": f"If it looks right, file it with file_request kind architectural and these details: {summary}.",
    })


# ==========================================================================
# 2. Explain my notice, and respond
# ==========================================================================
ARCH_RULES = ("shed", "fence", "paint", "architect", "solar", "landscap", "colour", "color", "deck")
MONTH_RX = "(?:" + "|".join(family.MONTHS) + ")"


def opened_date(v, now_d):
    return family.find_date(v.get("opened", ""), now_d) or dt.date.min


def fine_row(fines, *words):
    return next((f for f in fines if all(w in f["violation"].lower() for w in words)), None)


def t_explain_violation(c, e, a):
    d, p = COMMUNITIES[c], family.person(c, e)
    now_d = today()
    if a.get("violation_id"):
        v = violation(p, a["violation_id"])
        if not v:
            return unknown_violation(p, a["violation_id"])
    else:
        open_ = [v for v in p["violations"] if is_open(v)]
        if not open_:
            return "There are no open notices on this account. Nothing needs doing."
        v = max(open_, key=lambda x: opened_date(x, now_d))
    st, rule_ = v["status"], v["rule"]
    low = (st + " " + v["notice"] + " " + rule_).lower()
    architectural = (any(w in rule_.lower() for w in ARCH_RULES)
                     or bool(re.search(r"palette|without (?:architectural )?approval|unapproved", v["notice"], re.I)))

    deadline = None
    m = re.search(r"by (" + MONTH_RX + r" \d{1,2}(?:, \d{4})?)", st)
    if m:
        deadline = family.find_date(m.group(1), now_d)
    else:
        m = re.search(r"within (\d+) days", st)
        od = opened_date(v, now_d)
        if m and od != dt.date.min:
            deadline = od + dt.timedelta(days=int(m.group(1)))
    hearing = None
    m = re.search(r"[Hh]earing[^.]*? on (" + MONTH_RX + r" \d{1,2}(?:, \d{4})?)", st)
    if m:
        hd = family.find_date(m.group(1), now_d)
        nxt = d["meetings"]["next"]
        same = hd and family.find_date(nxt["when"], now_d) == hd
        hearing = {"date": family.say_date(hd) if hd else m.group(1),
                   "where": nxt["where"] if same else "ask management",
                   "time": nxt["when"].split(", ")[-1] if same and family.find_time(nxt["when"]) else "",
                   "say": "The board hears it at its meeting. You may go, in person or by video, and explain; "
                          "bring photos or a paint sample if it helps."}

    fines = d["fines"]
    money = next((x for x in sentences(st) if "$" in x), None)
    if architectural:
        row = fine_row(fines, "architectural")
    elif "second" in low or "hearing" in low:
        row = fine_row(fines, "continuing")
    elif "courtesy" in low:
        row = fine_row(fines, "second")
    else:
        row = None
    policy = search(c, f"fine schedule {rule_} {'second notice' if 'courtesy' in low else 'hearing daily fine'}",
                    1, "enforcement")
    if_not_fixed = {"say": (f"The notice says: {clip(money, 160)}" if money else
                            (f"Next step under the fine schedule: {row['violation']}, {row['fine']}." if row else
                             "The notice names no fine; the fine policy below applies.")),
                    "schedule": row, "policy": [{"cite": h["cite"], "text": clip(h["text"], 500)} for h in policy]}

    rule_hits = lib().search(c, f"{rule_} {v['notice']}", k=2)
    fix = {"trash": "put the bins back out of sight", "shed": "repaint the shed to match the house's approved colour",
           "grill": "move the grill away from the building", "parking": "move the vehicle",
           "balcon": "take the grill off the balcony", "septic": "send in the septic pumping receipt"}
    what_fix = next((w for k, w in fix.items() if k in low), f"put right what the notice describes: {v['notice']}")
    options = [{"option": "Fix it" + (f" by {family.say_date(deadline)}" if deadline else ""),
                "how": what_fix[0].upper() + what_fix[1:] + ". Then the notice closes with no fine."}]
    if v.get("appealable") and is_open(v):
        options += [{"option": "Appeal", "how": "If they think the notice is wrong, appeal_violation files an appeal; "
                                                "the board hears it and fines are paused."},
                    {"option": "Ask for more time", "how": "request_extension asks management for up to 60 more days."}]
    if not is_open(v):
        options = [{"option": "Nothing to do", "how": "This notice is closed; no fine is owed for it."}]
    elif architectural:
        options.append({"option": "Apply for approval", "how": "file_request kind architectural asks the committee "
                                                               "to approve it as it is (plan_architectural helps)."})
    return j({
        "notice": {"id": v["id"], "opened": v.get("opened", ""), "rule": rule_, "what": v["notice"], "status": st,
                   "open": is_open(v)},
        "deadline": family.say_date(deadline) if deadline else None,
        "if_not_fixed": if_not_fixed,
        "rule": [{"cite": h["cite"], "text": clip(h["text"], 700)} for h in rule_hits],
        "options": options,
        "hearing": hearing,
    })


def t_request_extension(c, e, a):
    p = family.person(c, e)
    vid = (a.get("violation_id") or "").strip().upper()
    v = violation(p, vid)
    if not v:
        return unknown_violation(p, vid)
    if not v.get("appealable") or not is_open(v):
        return (f"Notice {v['id']} can't be given more time here: {v['status']} "
                f"Management can explain the options: {COMMUNITIES[c]['management']['phone']}.")
    try:
        days = int(num(a.get("days")) or 30)
    except (TypeError, ValueError):
        days = 30
    days = max(1, min(days, 60))
    reason = clip(str(a.get("reason") or ""), 300)
    ref = family.ticket("EXT", e, v["id"])
    again = state.get(family.hw_key(c, e), f"ext#{v['id']}") is not None
    state.put(family.hw_key(c, e), f"ext#{v['id']}", {"violation_id": v["id"], "days": days, "reason": reason,
                                                       "ref": ref, "at": int(family.now()), "status": "waiting"})
    return (f"{'Updated the' if again else 'Sent a'} request for {days} more days on notice {v['id']} "
            f"(simulated), reference {ref}" + (f": \"{reason}\"" if reason else "") + ". "
            f"Management decides within 5 business days, and the deadline is paused until they answer.")


# ==========================================================================
# 3. Meeting companion
# ==========================================================================
DECISION = re.compile(r"\bmov(?:ed|es)\b|\bmotion\b|\bcarried\b|\bvoted\b|\bresolved\b|\bdecided\b|\bapproved\b", re.I)
ROUTINE = re.compile(r"adjourn|call to order|roll call|approval of (?:the )?(?:\w+ )?minutes", re.I)


def next_meeting(c):
    nxt = COMMUNITIES[c]["meetings"]["next"]
    md = family.find_date(nxt["when"], today())
    tm = family.find_time(nxt["when"])
    ts = family.at_local(md, *tm) if md and tm else None
    return nxt, md, ts


def speak_minutes(c):
    m = re.search(r"(\d+) minutes?", COMMUNITIES[c]["meetings"].get("speaking", ""))
    return int(m.group(1)) if m else 3


def decided_last_time(c):
    mins = [d for d in lib().docs.get(c, []) if d["type"] == "minutes"]
    if not mins:
        return None, []
    doc = max(mins, key=lambda d: d.get("adopted", ""))
    out = []
    for s in doc["sections"]:
        if ROUTINE.search(s["title"]) or not DECISION.search(s["text"]):
            continue
        said = " ".join(x for x in sentences(s["text"]) if DECISION.search(x)) or s["text"]
        out.append({"title": s["title"], "cite": library.cite(doc, s), "text": clip(said, 300)})
    return doc, out


def t_meeting_companion(c, a):
    d = COMMUNITIES[c]
    mt = d["meetings"]
    nxt, md, _ = next_meeting(c)
    doc, decided = decided_last_time(c)
    on_ballot = [{"id": b["id"], "question": b["question"], "closes": family.say_date(dt.date.fromisoformat(b["closes"]))}
                 for b in BALLOTS.get(c, []) if ballot_open(b)]
    return j({
        "meeting": {"when": nxt["when"], "where": nxt["where"], "agenda": nxt["agenda"]},
        "speaking": {"how": mt.get("speaking", ""), "minutes_each": speak_minutes(c),
                     "sign_up": "register_to_speak signs them up; submit_forum_question sends a question instead."},
        "voting": {"how": mt.get("voting", ""), "quorum": mt.get("quorum", ""),
                   "by_voice": "open_ballots lists what is open; cast_vote records this home's vote.",
                   "open_now": on_ballot},
        "decided_last_time": {"minutes": doc["title"] if doc else "No minutes on file.",
                              "decisions": decided},
        "minutes_posted": mt.get("minutes", ""),
    })


def t_register_to_speak(c, e, a):
    topic = clip(str(a.get("topic") or ""), 200)
    if not topic:
        return "What would they like to speak about?"
    nxt, md, ts = next_meeting(c)
    key = f"speak#{md.isoformat() if md else 'next'}"
    pk = family.hw_key(c, e)
    again = state.get(pk, key) is not None
    p = family.person(c, e)
    state.put(pk, key, {"topic": topic, "at": int(family.now()), "name": p["name"], "address": p["address"],
                        "email": e, "meeting": nxt["when"]})
    n = speak_minutes(c)
    late = ""
    if ts and "24 hours" in COMMUNITIES[c]["meetings"].get("speaking", "") and ts - family.now() < 86400:
        late = " It is less than a day before the meeting, so they may also need to sign in at the door."
    return (f"{'Updated: their topic is now' if again else 'Signed up to speak about'} \"{topic}\" at the board "
            f"meeting, {nxt['when']}, {nxt['where']} (simulated). Each speaker has {n} minutes in the open forum; "
            f"the chair will call their name.{late}")


def t_submit_forum_question(c, e, a):
    q = clip(str(a.get("question") or ""), 400)
    if not q:
        return "What would they like to ask the board?"
    nxt, _, _ = next_meeting(c)
    t = family.now()
    p = family.person(c, e)
    state.put(family.hw_key(c, e), f"ask#{t:.3f}#{secrets.token_hex(3)}",
              {"question": q, "at": int(t), "name": p["name"], "address": p["address"], "email": e,
               "meeting": nxt["when"]})
    return (f"Sent to the board for the open forum at the meeting on {nxt['when']} (simulated): \"{q}\". "
            f"The board answers it at the meeting, and the answer goes in the minutes.")


def meeting_requests(c):
    """For the office console: who signed up to speak, and the questions sent in, oldest first."""
    speakers, questions = [], []
    for e in COMMUNITIES[c]["homeowners"]:
        pk = family.hw_key(c, e)
        speakers += [dict(x, meeting_date=sk.split("#", 1)[1]) for sk, x in state.query(pk, "speak#")]
        questions += [x for _, x in state.query(pk, "ask#")]
    return {"speakers": sorted(speakers, key=lambda x: x["at"]), "questions": sorted(questions, key=lambda x: x["at"])}


# ==========================================================================
# 4. Voting by voice
# ==========================================================================
BALLOTS = {
    "sheppard center": [
        {"id": "SC-ROOF", "kind": "assessment", "opens": "2026-10-08", "closes": "2026-10-22",
         "question": "Approve a special assessment of $400 per home for the clubhouse roof, due January 15, 2027?",
         "choices": ["Yes", "No"],
         "explain": "The clubhouse roof needs replacing. If this passes, every home pays $400 once, due January 15, "
                    "2027. The vote closes at the board meeting on October 22."},
        {"id": "SC-DUES", "kind": "assessment", "opens": "2026-10-08", "closes": "2026-10-22",
         "question": "Approve the 2027 budget, with dues rising from $285 to $299 a month?",
         "choices": ["Yes", "No"],
         "explain": "The 2027 budget raises the monthly dues by $14, from $285 to $299, for landscaping, insurance "
                    "and management costs. The vote closes at the board meeting on October 22."},
        {"id": "SC-BOARD", "kind": "election", "opens": "2027-01-07", "closes": "2027-01-21", "pick": 2,
         "question": "Elect two members to the board of directors",
         "choices": ["Evelyn Marsh", "Samuel Okafor", "Kevin O'Neill", "Gloria Trent"],
         "explain": "Two board seats are open. Each home may choose up to two people. Kevin O'Neill is the current "
                    "secretary; Evelyn Marsh is a retired teacher on the social committee; Samuel Okafor is an "
                    "accountant who joined the landscape committee; Gloria Trent runs the neighbourhood watch. "
                    "Electronic voting opens January 7 and closes at the annual meeting on January 21."},
    ],
    "stan meadows": [
        {"id": "SM-GATE", "kind": "assessment", "opens": "2026-10-01", "closes": "2026-11-10",
         "question": "Approve spending $18,000 from the reserve fund on the gate camera upgrade?",
         "choices": ["Yes", "No"],
         "explain": "SecureEntry Systems would replace the main gate's cameras and recorder for $18,000, paid from "
                    "savings already set aside, so dues do not change. The vote closes at the November 10 meeting."},
    ],
    "harbor view": [
        {"id": "HV-ELEVATOR", "kind": "assessment", "opens": "2026-10-01", "closes": "2026-10-28",
         "question": "Approve a special assessment of about $3,550 per unit, payable over 12 months, to modernise "
                     "both elevators?",
         "choices": ["Yes", "No"],
         "explain": "LiftCo would modernise Elevator A and Elevator B for $640,000. Each unit would pay about $3,550, "
                    "spread over 12 months (about $296 a month). In this building votes count by unit share; this "
                    "demo counts one per unit. The vote closes at the October 28 meeting."},
    ],
    "oak ridge": [
        {"id": "OR-PAVING", "kind": "assessment", "opens": "2026-10-01", "closes": "2026-11-14",
         "question": "Approve the $85,000 contract with Turner Excavating to pave the lower half of Ridge Road?",
         "choices": ["Yes", "No"],
         "explain": "Turner Excavating would grade and pave the lower half of Ridge Road for $85,000. The vote "
                    "closes at the November 14 meeting in the community barn."},
        {"id": "OR-PLOW", "kind": "assessment", "opens": "2026-10-01", "closes": "2026-11-14",
         "question": "Renew the snow plow contract with Hollis Plowing for this winter?",
         "choices": ["Yes", "No"],
         "explain": "Hollis Plowing clears the community roads after each storm. This renews their contract for the "
                    "coming winter. The vote closes at the November 14 meeting."},
    ],
}
YES = re.compile(r"\b(yes|yeah|yep|approve|aye|in favou?r|agree)\b", re.I)
NO = re.compile(r"\b(no|nope|nay|against|reject|oppose|disapprove)\b", re.I)


def ballot_open(b, d=None):
    d = d or today()
    return dt.date.fromisoformat(b.get("opens", "2000-01-01")) <= d <= dt.date.fromisoformat(b["closes"])


def find_ballot(c, bid):
    bid = (bid or "").strip().lower()
    return next((b for b in BALLOTS.get(c, []) if b["id"].lower() == bid), None)


def match_choice(b, said):
    """The ballot's own choices named in what was said: [] none, more than allowed means ask again."""
    said = (said or "").strip()
    low = said.lower()
    if [x.lower() for x in b["choices"]] == ["yes", "no"]:
        y, n = bool(YES.search(said)), bool(NO.search(said))
        return ["Yes"] if y and not n else ["No"] if n and not y else (["Yes", "No"] if y else [])
    got = []
    for ch in b["choices"]:
        names = [ch.lower()] + [w for w in re.findall(r"[a-z']+", ch.lower()) if len(w) >= 3]
        if any(re.search(r"\b" + re.escape(w) + r"\b", low) for w in names):
            got.append(ch)
    return got


def say_choice(chosen):
    return chosen[0] if len(chosen) == 1 else " and ".join(chosen)


def t_open_ballots(c, e, a):
    pk = family.hw_key(c, e)
    now_d = today()
    out, coming = [], []
    for b in BALLOTS.get(c, []):
        closes = dt.date.fromisoformat(b["closes"])
        if ballot_open(b, now_d):
            out.append({"id": b["id"], "question": b["question"], "choices": b["choices"],
                        "pick": b.get("pick", 1), "closes": family.say_date(closes), "explain": b["explain"],
                        "kind": b["kind"], "this_home_has_voted": state.get(pk, f"vote#{b['id']}") is not None})
        elif now_d < dt.date.fromisoformat(b.get("opens", "2000-01-01")):
            coming.append({"id": b["id"], "question": b["question"],
                           "opens": family.say_date(dt.date.fromisoformat(b["opens"]))})
    if not out and not coming:
        return "There are no votes open right now."
    return j({"open": out or "No votes are open right now.", "coming": coming,
              "how": "cast_vote with the ballot id and their choice; read it back and record it only when they say yes."})


def t_cast_vote(c, e, a):
    b = find_ballot(c, a.get("ballot_id"))
    if not b:
        ids = [f"{x['id']} ({x['question']})" for x in BALLOTS.get(c, []) if ballot_open(x)]
        return f"I can't find that ballot. Open now: {'; '.join(ids) if ids else 'none'}."
    now_d = today()
    if now_d > dt.date.fromisoformat(b["closes"]):
        return f"The vote on \"{b['question']}\" closed on {family.say_date(dt.date.fromisoformat(b['closes']))}."
    if not ballot_open(b, now_d):
        return (f"The vote on \"{b['question']}\" opens on "
                f"{family.say_date(dt.date.fromisoformat(b['opens']))}. It can't be cast before then.")
    chosen = match_choice(b, a.get("choice"))
    pick = b.get("pick", 1)
    if not chosen:
        return f"I didn't catch a choice on that ballot. The choices are: {', '.join(b['choices'])}."
    if len(chosen) > pick:
        return (f"That names more than {'one choice' if pick == 1 else f'{pick} people'}. "
                f"Which {'one' if pick == 1 else pick}? The choices are: {', '.join(b['choices'])}.")
    confirm = a.get("confirm")
    if not (confirm is True or str(confirm).strip().lower() in ("true", "yes", "1")):
        return (f"Read this back and wait for a clear yes: \"Shall I record your vote: {say_choice(chosen)} on "
                f"{b['question'].rstrip('?')}?\" Then call cast_vote again with confirm true. Nothing is recorded yet.")
    pk = family.hw_key(c, e)
    before = state.get(pk, f"vote#{b['id']}")
    choice = chosen[0] if pick == 1 else chosen
    t = int(family.now())
    state.put(pk, f"vote#{b['id']}", {"choice": choice, "at": t})
    ref = family.ticket("VOTE", e, b["id"])
    closes = family.say_date(dt.date.fromisoformat(b["closes"]))
    p = family.person(c, e)
    witnessed = family.email_helpers(
        c, e, f"A vote was {'changed' if before else 'cast'} for {p['name']}",
        f"{p['name']} {'changed their' if before else 'cast a'} vote through stan-claw today on this question:\n\n"
        f"{b['question']}\n\nReceipt {ref}. How they voted stays private; this note is only so someone in the "
        f"family knows a vote was recorded. Voting closes {closes}.")
    out = (f"{'Changed: the vote is now' if before else 'Recorded:'} {say_choice(chosen)} on \"{b['question']}\" "
           f"(simulated). Receipt {ref}. "
           + ("This replaces the earlier vote from this home. " if before else "")
           + f"One vote per home; it can be changed until voting closes on {closes}.")
    if witnessed:
        out += (f" {family.names_of([{'name': n} for n in witnessed])} got an email that a vote was recorded, "
                f"without how they voted.")
    return out


def ballot_results(c):
    """For the office console: the counts so far (never who voted how)."""
    homes = COMMUNITIES[c]["profile"]["homes"]
    out = {}
    for b in BALLOTS.get(c, []):
        votes, n = {ch: 0 for ch in b["choices"]}, 0
        for e in COMMUNITIES[c]["homeowners"]:
            v = state.get(family.hw_key(c, e), f"vote#{b['id']}")
            if not v:
                continue
            n += 1
            for ch in (v["choice"] if isinstance(v["choice"], list) else [v["choice"]]):
                votes[ch] = votes.get(ch, 0) + 1
        out[b["id"]] = {"question": b["question"], "votes": votes, "homes_voted": n, "homes": homes,
                        "closes": b["closes"], "open": ballot_open(b)}
    return out


# ==========================================================================
# 5. Same-day alerts from notices
# ==========================================================================
STREET = re.compile(r"\b(?i:water|power|gas|electric\w*)\b[^.]*?\bon ([A-Z][a-z]+ (?:Lane|Road|Court|Circle|"
                    r"Drive|Street|Way|Avenue|Place))")


def notice_dates(text, now_d):
    """[(date, is_deadline)] for each 'October 14' / 'October 14 and 15' in text, skipping 'since ...'."""
    out = []
    for m in re.finditer(r"(\bsince |\bby )?(" + MONTH_RX + r") (\d{1,2}(?:(?:, | and |-| to )\d{1,2})*)"
                         r"(?:, (\d{4}))?", text):
        if (m.group(1) or "").strip() == "since":
            continue
        for day in re.findall(r"\d{1,2}", m.group(3)):
            d = family.find_date(f"{m.group(2)} {day}" + (f", {m.group(4)}" if m.group(4) else ""), now_d)
            if d:
                out.append((d, (m.group(1) or "").strip() == "by"))
    return out


def affects(c, e, n):
    """A utility shut-off on one street, or one riser stack, only matters to the people there."""
    addr = family.person(c, e)["address"]
    text = f"{n.get('what', '')} {n.get('when', '')}"
    m = STREET.search(text)
    if m and m.group(1).lower() not in addr.lower():
        return False
    m = re.search(r"\bstack (\d+)", text, re.I)
    unit = re.search(r"\b(\d{3,4})\b", addr)
    if m and unit and not unit.group(1).endswith(m.group(1)[-2:]):
        return False
    return True


def notice_reminders(c, e, now_ts):
    """Reminders for notices dated today (said at 9:00 local) or tomorrow (said at 17:00 local today).

    Kept: reminders whose time is between 6 hours ago (so a board that was off catches up, as t_reminders
    does) and 30 hours ahead. The board de-duplicates by id."""
    now_d = family.local(now_ts).date()
    tomorrow = now_d + dt.timedelta(days=1)
    out = []
    for n in COMMUNITIES[c].get("notices", []):
        if not affects(c, e, n):
            continue
        what = n.get("what", "").strip()
        what_l = what[:1].lower() + what[1:] if what[1:2].islower() else what
        when = n.get("when", "")
        span = re.search(r"\d{1,2}(?::\d{2})? ?(?:am|pm) to \d{1,2}(?::\d{2})? ?(?:am|pm)", when)
        times = f", {span.group(0)}" if span else ""
        slug = re.sub(r"[^a-z0-9]+", "-", what.lower()).strip("-")[:40].rstrip("-")
        dates = notice_dates(f"{what} {when}", now_d)
        has_today = any(d == now_d for d, _ in dates)
        for d, deadline in dates:
            if d == now_d:
                at = family.at_local(d, 9)
                say = (f"Today is the last day for this: {what_l}." if deadline
                       else f"A notice for today: {what_l}{times}.")
            elif d == tomorrow and not has_today:
                at = family.at_local(now_d, 17)
                say = (f"Tomorrow, {family.say_date(d)}, is the last day for this: {what_l}." if deadline
                       else f"A notice for tomorrow, {family.say_date(d)}: {what_l}{times}.")
            else:
                continue
            if now_ts - 6 * 3600 <= at <= now_ts + 30 * 3600:
                out.append({"id": f"notice-{slug}-{d.isoformat()}", "at": at, "say": say})
    out.sort(key=lambda x: x["at"])
    return out


# ==========================================================================
# The tools
# ==========================================================================
S = {"type": "string"}
CIVIC_TOOLS = [
    ("plan_architectural",
     "Before filing an architectural request (fence, shed, deck, patio, paint, solar, addition, window, door, roof): "
     "checks the plan against the community's architectural guidelines. First interview the person ONE question at "
     "a time for the missing details (what, how tall, what material, what colour, where on the lot), then call this. "
     "Explain any concern simply and kindly, say how long review takes, then offer to file it with file_request.",
     {"project": {"type": "string", "enum": PROJECTS}, "height_ft": {"type": "number"}, "material": S, "color": S,
      "location": {"type": "string", "description": "where on the lot: front yard, back yard, side, roof ..."},
      "size": S, "details": S},
     ["project"], "private", t_plan_architectural),
    ("explain_violation",
     "Explains a violation notice in plain words: what it is about, the deadline, what happens if it is not fixed, "
     "the rule it cites, any hearing, and their choices (fix it, appeal, ask for more time, apply for approval). "
     "With no violation_id, the newest open notice. Say it slowly, one point at a time.",
     {"violation_id": S}, [], "private", t_explain_violation),
    ("request_extension",
     "Asks management for more time to fix a violation notice (1 to 60 days, 30 if not said). Confirm the notice "
     "and the number of days with the person before calling.",
     {"violation_id": S, "days": {"type": "integer", "minimum": 1, "maximum": 60}, "reason": S},
     ["violation_id"], "private", t_request_extension),
    ("meeting_companion",
     "Everything about the next board meeting: when and where, the agenda, how to speak, how to vote, what is open "
     "for a vote, and what the board decided at its last meeting (from the minutes, cited).",
     {}, [], "public", t_meeting_companion),
    ("register_to_speak",
     "Signs the homeowner up to speak in the open forum at the next board meeting, on a topic. One sign-up per "
     "meeting; calling again changes the topic.",
     {"topic": S}, ["topic"], "private", t_register_to_speak),
    ("submit_forum_question",
     "Sends a question to the board for the open forum at the next meeting, for someone who would rather not speak.",
     {"question": S}, ["question"], "private", t_submit_forum_question),
    ("open_ballots",
     "The community votes open now (and coming soon), each explained in plain words, and whether this home has "
     "already voted.", {}, [], "private", t_open_ballots),
    ("cast_vote",
     "Records this home's vote on an open ballot. Always call first WITHOUT confirm: it returns a read-back to say. "
     "Only after the person clearly says yes, call again with confirm true. One vote per home; voting again "
     "changes it until the ballot closes. Family contacts get a note that a vote was cast, never the choice.",
     {"ballot_id": S, "choice": S, "confirm": {"type": "boolean"}},
     ["ballot_id", "choice"], "private", t_cast_vote),
]
