"""Exercise civic.py (guided requests, notices explained, meetings, voting, notice alerts): python3 stan-mcp/test_civic.py"""
import json, os, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ["MCP_TOKEN"] = "test-token"
os.environ["PUBLIC_URL"] = "https://example.test/"
for k in ("STATE_TABLE", "MAIL_FROM", "FAMILY_EMAIL"):
    os.environ.pop(k, None)
import datetime as dt
import family, library, mail, state
import civic
from hoa_data import COMMUNITIES

NOW = dt.datetime(2026, 10, 10, 19, 0, tzinfo=dt.timezone.utc).timestamp()     # Saturday Oct 10, 3 pm EDT
family.now = lambda: NOW

fails = 0
def check(c, what):
    global fails
    if not c:
        print("FAIL", what)
        fails += 1

SC, ME, MARIA, JAMES = "sheppard center", "info@avriz.io", "maria.lopez@example.com", "james.chen@example.com"
TOOLS = {t[0]: t for t in civic.CIVIC_TOOLS}

def call(name, c, e=None, **a):
    t = TOOLS[name]
    return t[5](c, a) if t[4] == "public" else t[5](c, e, a)

def at(y, m, d, h, mi=0):
    return family.at_local(dt.date(y, m, d), h, mi)

# --------------------------------------------------------------------------
# The exports' shape
# --------------------------------------------------------------------------
check(all(len(t) == 6 and t[4] in ("public", "private") and callable(t[5]) for t in civic.CIVIC_TOOLS), "tool tuples")
check(len(TOOLS) == len(civic.CIVIC_TOOLS) == 8, "eight tools, unique names")
check(all(set(t[3]) <= set(t[2]) and "homeowner_token" not in t[2] and "community" not in t[2]
          for t in civic.CIVIC_TOOLS), "required within properties; no token/community")
check(set(TOOLS) == {"plan_architectural", "explain_violation", "request_extension", "meeting_companion",
                     "register_to_speak", "submit_forum_question", "open_ballots", "cast_vote"}, "tool names")

# --------------------------------------------------------------------------
# A small library of our own
# --------------------------------------------------------------------------
tmp = tempfile.mkdtemp()
os.makedirs(os.path.join(tmp, "sheppard_center"))
def doc(name, text):
    open(os.path.join(tmp, "sheppard_center", name), "w").write(text)
doc("architectural.md", """# Architectural Guidelines
<!-- type: architectural; adopted: 2002-05-01 -->
## Article 2 - Applying
### 2.1 How to Apply
Apply on the form, attaching a site sketch, photographs, paint samples and the contractor's details.
### 2.2 Review Timeline
The Committee must respond within 30 days of a complete application. If it has not, the application is deemed approved.
## Article 3 - Colors
### 3.1 Body Colors
The approved body colors are Colonial White, Dove Gray, Sage, Wheat and Slate Blue.
### 3.3 Colors Outside the Palette
A brighter green sold as Meadow Green is not Sage and is not on the approved palette.
## Article 4 - Fences
### 4.1 Fences
Fences shall be no taller than 6 feet. Chain link is not permitted for a fence.
## Article 5 - Sheds
### 5.1 Sheds
A shed may be no larger than 120 square feet and no taller than 10 feet.
""")
doc("enforcement.md", """# Fine and Enforcement Policy
<!-- type: enforcement; adopted: 2010-01-01 -->
## Part 4 - Fines
### 4.1 Fine Schedule
A second notice carries a $50 fine; after a hearing, $25 a day up to $1,000.
""")
doc("rules.md", """# Rules and Regulations
<!-- type: rules; adopted: 2010-01-01 -->
## Article 4 - Trash
### 4.2 Trash Bins
Trash bins go back out of street view by 8 pm on pickup day.
""")
doc("minutes-a-new.md", """# Minutes, September 24, 2026
<!-- type: minutes; adopted: 2026-09-24 -->
## Business
### 1. Call to order
The President called the meeting to order. A motion to proceed carried.
### 2. Clubhouse roof vote
Ms. Brooks moved to put the roof assessment to a vote on October 22. Motion carried 4-0.
### 3. Pool bids
Management described the pool bids. No action was taken.
### 4. Adjournment
Ms. Park moved to adjourn. Motion carried.
""")
doc("minutes-b-old.md", """# Minutes, August 27, 2026
<!-- type: minutes; adopted: 2026-08-27 -->
## Business
### 1. Pond fountain
Mr. Lund moved to buy a pond fountain. Motion carried 5-0.
""")
library._LIB = library.Library(tmp)
state.reset()

# --------------------------------------------------------------------------
# 1. plan_architectural
# --------------------------------------------------------------------------
r = json.loads(call("plan_architectural", SC, ME, project="fence", height_ft=7, material="white vinyl", color="Sage",
                    location="back yard"))
check(any("7-foot" in x and "6 feet" in x and "Architectural Guidelines" in x for x in r["concerns"]),
      "7 ft fence flagged against 'no taller than 6 feet'")
check(r["guidelines"] and r["guidelines"][0]["cite"].startswith("Architectural Guidelines")
      and all(len(g["text"]) <= 700 for g in r["guidelines"]) and len(r["guidelines"]) <= 3, "guidelines cited, short")
check(not any("approved colours" in x for x in r["concerns"]), "Sage is approved")
check(any("drawing" in x or "sketch" in x for x in r["checklist"]) and any("contractor" in x for x in r["checklist"]),
      "checklist from the how-to-apply section")
check(r["review"]["days"] == 30 and r["review"]["no_answer_counts_as_approval"] is True
      and "counts as approved" in r["review"]["say"], "review time and deemed approval")
check(r["next"].startswith("If it looks right, file it with file_request kind architectural") and "7 ft" in r["next"],
      "next step names file_request")
r = json.loads(call("plan_architectural", SC, ME, project="fence", height_ft=5))
check(not any("foot" in x and "taller" in x for x in r["concerns"]), "5 ft fence passes")
check(set(r["still_to_ask"]) == {"material", "color", "location"}, "still to ask")
r = json.loads(call("plan_architectural", SC, ME, project="fence", height_ft="6", color="Meadow Green",
                    material="chain link"))
check(any("Meadow Green" in x and "not named among the approved colours" in x and "Section 3.1" in x
          for x in r["concerns"]), "off-palette colour flagged")
check(any("chain link" in x for x in r["concerns"]), "chain link flagged")
check(not any("taller" in x for x in r["concerns"]), "6 ft is within 6 feet")
r = json.loads(call("plan_architectural", SC, ME, project="paint", color="dove grey"))
check(not any("approved colours" in x for x in r["concerns"]), "grey/gray and case ignored")
r = json.loads(call("plan_architectural", SC, ME, project="shed", height_ft=12))
check(any("12-foot shed" in x and "10 feet" in x for x in r["concerns"]), "shed height flagged (not the square feet)")
r = json.loads(call("plan_architectural", SC, ME, project="shed", height_ft=9))
check(not any("taller" in x for x in r["concerns"]), "9 ft shed passes")

# --------------------------------------------------------------------------
# 2. explain_violation, request_extension
# --------------------------------------------------------------------------
r = json.loads(call("explain_violation", SC, ME))
check(r["notice"]["id"] == "V-2026-0412", "newest open notice by default")
check(r["deadline"] == "Saturday, October 17", "deadline in words")
check("$50" in r["if_not_fixed"]["say"] and r["if_not_fixed"]["schedule"]["fine"] == "$50", "fine if not fixed")
check(r["rule"] and len(r["rule"]) <= 2 and r["rule"][0]["cite"].startswith("Rules and Regulations"), "rule cited")
opts = " ".join(o["option"] for o in r["options"])
check("Fix it by Saturday, October 17" in opts and "Appeal" in opts and "more time" in opts
      and "Apply for approval" not in opts and r["hearing"] is None, "options for a trash notice")
r = json.loads(call("explain_violation", SC, ME, violation_id="v-2026-0388"))
check(r["hearing"] and r["hearing"]["date"] == "Thursday, October 22" and r["hearing"]["time"] == "7:00 pm"
      and "Clubhouse" in r["hearing"]["where"], "hearing named")
check(any(o["option"] == "Apply for approval" for o in r["options"]), "architectural notice: apply for approval")
check("$250" in r["if_not_fixed"]["say"], "architectural fine stage")
check("can't find" in call("explain_violation", SC, ME, violation_id="V-1999-0001"), "unknown violation")
check("no open notices" in call("explain_violation", SC, JAMES), "no open notices")
r = json.loads(call("explain_violation", SC, JAMES, violation_id="V-2026-0301"))
check(r["notice"]["open"] is False and [o["option"] for o in r["options"]] == ["Nothing to do"], "a closed notice")
r = json.loads(call("explain_violation", "stan meadows", "nora.james@example.com"))
check(r["deadline"] == "Tuesday, October 13", "deadline from opened + 14 days")
check(not any(o["option"] == "Apply for approval" for o in r["options"]), "a grill on a patio is not architectural")

out = call("request_extension", SC, ME, violation_id="v-2026-0412", reason="I am in hospital this week")
ext = state.get(family.hw_key(SC, ME), "ext#V-2026-0412")
check(ext and ext["days"] == 30 and ext["reason"].startswith("I am in hospital") and ext["status"] == "waiting",
      "extension stored, 30 days by default")
check(family.ticket("EXT", ME, "V-2026-0412") in out and "5 business days" in out and "paused" in out, "extension answer")
call("request_extension", SC, ME, violation_id="V-2026-0412", days=90)
check(state.get(family.hw_key(SC, ME), "ext#V-2026-0412")["days"] == 60, "days capped at 60")
check("can't find" in call("request_extension", SC, ME, violation_id="V-0000"), "extension: unknown id")
out = call("request_extension", SC, JAMES, violation_id="V-2026-0301")
check("can't be given more time" in out and not state.get(family.hw_key(SC, JAMES), "ext#V-2026-0301"),
      "extension: resolved notice refused")
out = call("request_extension", "oak ridge", "tony.russo@example.com", violation_id="OR-2026-012")
check("can't be given more time" in out, "extension: non-appealable refused")

# --------------------------------------------------------------------------
# 3. Meetings
# --------------------------------------------------------------------------
r = json.loads(call("meeting_companion", SC))
check(r["meeting"]["when"].startswith("Thursday, October 22") and r["meeting"]["agenda"], "next meeting")
check(r["speaking"]["minutes_each"] == 3 and "24 hours" in r["speaking"]["how"], "how to speak")
check("Proxies" in r["voting"]["how"] and any(b["id"] == "SC-ROOF" for b in r["voting"]["open_now"]), "how to vote")
dec = r["decided_last_time"]
titles = [x["title"] for x in dec["decisions"]]
check(dec["minutes"] == "Minutes, September 24, 2026", "newest minutes chosen")
check(titles == ["Clubhouse roof vote"], f"decisions only, no routine items ({titles})")
check(dec["decisions"][0]["cite"] == "Minutes, September 24, 2026, item 2"
      and "Ms. Brooks moved" in dec["decisions"][0]["text"] and len(dec["decisions"][0]["text"]) <= 300,
      "decision cited and quoted")

out = call("register_to_speak", SC, ME, topic="the pool bids")
check("3 minutes" in out and "October 22" in out and "7:00 pm" in out, "speaker confirmation")
out = call("register_to_speak", SC, ME, topic="the clubhouse roof")
check(out.startswith("Updated"), "second sign-up updates")
sp = state.query(family.hw_key(SC, ME), "speak#")
check(len(sp) == 1 and sp[0][0] == "speak#2026-10-22" and sp[0][1]["topic"] == "the clubhouse roof", "one per meeting")
check("2 minutes" in call("register_to_speak", "harbor view", "chen.wei@example.com", topic="elevators"),
      "harbor view's 2-minute rule")
check(call("register_to_speak", SC, MARIA, topic="").startswith("What would"), "topic needed")
out = call("submit_forum_question", SC, MARIA, question="When will the pool reopen?")
call("submit_forum_question", SC, MARIA, question="Can we have a bench on the loop?")
check("open forum" in out and "pool reopen" in out, "question confirmation")
check(len(state.query(family.hw_key(SC, MARIA), "ask#")) == 2, "two questions kept at the same instant")
mr = civic.meeting_requests(SC)
check(len(mr["speakers"]) == 1 and mr["speakers"][0]["topic"] == "the clubhouse roof"
      and mr["speakers"][0]["meeting_date"] == "2026-10-22" and mr["speakers"][0]["name"] == "Reza", "console: speakers")
check(len(mr["questions"]) == 2 and mr["questions"][0]["name"] == "Maria Lopez", "console: questions")

# --------------------------------------------------------------------------
# 4. Voting
# --------------------------------------------------------------------------
for c in COMMUNITIES:
    check(any(civic.ballot_open(b) for b in civic.BALLOTS.get(c, [])), f"{c} has an open ballot")
    check(all(set(b) >= {"id", "question", "choices", "closes", "explain", "kind"}
              and b["kind"] in ("assessment", "election", "amendment") for b in civic.BALLOTS[c]), f"{c} ballot shape")
r = json.loads(call("open_ballots", SC, ME))
ids = [b["id"] for b in r["open"]]
check(ids == ["SC-ROOF", "SC-DUES"] and not any(b["this_home_has_voted"] for b in r["open"]), "open ballots, not voted")
check(r["coming"] and r["coming"][0]["id"] == "SC-BOARD", "the election is coming")

out = call("cast_vote", SC, ME, ballot_id="sc-roof", choice="yes please")
check("Shall I record your vote: Yes on Approve a special assessment of $400" in out and "Nothing is recorded" in out,
      "read-back before recording")
check(state.get(family.hw_key(SC, ME), "vote#SC-ROOF") is None, "nothing recorded without confirm")
call("cast_vote", SC, ME, ballot_id="SC-ROOF", choice="yes", confirm=False)
call("cast_vote", SC, ME, ballot_id="SC-ROOF", choice="yes", confirm="false")
check(state.get(family.hw_key(SC, ME), "vote#SC-ROOF") is None, "confirm false records nothing")
out = call("cast_vote", SC, ME, ballot_id="SC-ROOF", choice="yes", confirm=True)
v = state.get(family.hw_key(SC, ME), "vote#SC-ROOF")
check(v == {"choice": "Yes", "at": int(NOW)}, "vote stored")
check(family.ticket("VOTE", ME, "SC-ROOF") in out and out.startswith("Recorded: Yes"), "receipt")
r = json.loads(call("open_ballots", SC, ME))
check([b["this_home_has_voted"] for b in r["open"]] == [True, False], "voted flag")
check("choice" not in json.dumps(r["open"][0]).replace('"choices"', ""), "open_ballots never says the choice")
out = call("cast_vote", SC, ME, ballot_id="SC-ROOF", choice="No, I'm against it", confirm=True)
check(out.startswith("Changed: the vote is now No") and "replaces the earlier vote" in out, "vote changed, said so")
check(state.get(family.hw_key(SC, ME), "vote#SC-ROOF")["choice"] == "No", "one vote per home: the latest")
check("didn't catch" in call("cast_vote", SC, ME, ballot_id="SC-ROOF", choice="maybe", confirm=True), "unknown choice")
check("more than one" in call("cast_vote", SC, ME, ballot_id="SC-ROOF", choice="yes no", confirm=True), "ambiguous")
check("can't find that ballot" in call("cast_vote", SC, ME, ballot_id="SC-NOPE", choice="yes", confirm=True),
      "unknown ballot")
check("opens on Thursday, January 7" in call("cast_vote", SC, ME, ballot_id="SC-BOARD", choice="Marsh", confirm=True),
      "not open yet")

# the witness email: Maria has a helper
state.put(family.hw_key(SC, MARIA), "helper#ann@example.com", {"name": "Ann", "relation": "daughter",
                                                               "email": "ann@example.com"})
mail.outbox.clear()
out = call("cast_vote", SC, MARIA, ballot_id="SC-ROOF", choice="Yes", confirm=True)
check(len(mail.outbox) == 1 and mail.outbox[0]["to"] == ["ann@example.com"], "helper emailed")
m = mail.outbox[0]
check("vote was cast" in m["subject"] and "clubhouse roof" in m["text"] and family.ticket("VOTE", MARIA, "SC-ROOF") in m["text"],
      "witness email says a vote happened")
import re
check(not re.search(r"\b(yes|no)\b", m["subject"] + " " + m["text"], re.I), "witness email without the choice")
check("Ann got an email" in out and "without how they voted" in out, "the person is told")
mail.outbox.clear()
call("cast_vote", SC, ME, ballot_id="SC-DUES", choice="yes", confirm=True)
check(not mail.outbox, "no helpers, no email")

res = civic.ballot_results(SC)
check(res["SC-ROOF"]["votes"] == {"Yes": 1, "No": 1} and res["SC-ROOF"]["homes_voted"] == 2
      and res["SC-ROOF"]["homes"] == 212, "results counted")
check(res["SC-DUES"]["votes"] == {"Yes": 1, "No": 0} and res["SC-BOARD"]["homes_voted"] == 0, "other ballots")

# the election, in January: two picks
family.now = lambda: at(2027, 1, 10, 12)
out = call("cast_vote", SC, MARIA, ballot_id="SC-BOARD", choice="Marsh and Okafor")
check("Shall I record your vote: Evelyn Marsh and Samuel Okafor on Elect two members" in out, "election read-back")
check("more than 2" in call("cast_vote", SC, MARIA, ballot_id="SC-BOARD", choice="Marsh, Okafor and Trent",
                            confirm=True), "at most two")
mail.outbox.clear()
call("cast_vote", SC, MARIA, ballot_id="SC-BOARD", choice="Evelyn Marsh and Gloria Trent", confirm=True)
check(state.get(family.hw_key(SC, MARIA), "vote#SC-BOARD")["choice"] == ["Evelyn Marsh", "Gloria Trent"],
      "two picks stored")
check(mail.outbox and not any(n in mail.outbox[0]["text"] + mail.outbox[0]["subject"] for n in ("Marsh", "Trent")),
      "election witness email without names")
check(civic.ballot_results(SC)["SC-BOARD"]["votes"]["Gloria Trent"] == 1, "election counted")
check("closed on Thursday, October 22" in call("cast_vote", SC, ME, ballot_id="SC-ROOF", choice="yes", confirm=True),
      "closed ballot refused")
family.now = lambda: NOW

# --------------------------------------------------------------------------
# 5. Notice reminders
# --------------------------------------------------------------------------
r = civic.notice_reminders(SC, ME, at(2026, 10, 12, 15))          # Willow Lane water shut-off is tomorrow
check(len(r) == 1 and r[0]["id"].startswith("notice-water-shut-off") and r[0]["id"].endswith("-2026-10-13"),
      f"tomorrow's notice ({r})")
check(r and r[0]["at"] == at(2026, 10, 12, 17) and "tomorrow, Tuesday, October 13" in r[0]["say"]
      and "9 am to 1 pm" in r[0]["say"], "said at 5 pm the day before")
check(civic.notice_reminders(SC, MARIA, at(2026, 10, 12, 15)) == [], "not for Heron Court")
r = civic.notice_reminders(SC, ME, at(2026, 10, 13, 7))
check(len(r) == 1 and r[0]["at"] == at(2026, 10, 13, 9) and r[0]["say"].startswith("A notice for today"),
      "said at 9 am on the day")
check(civic.notice_reminders(SC, ME, at(2026, 10, 13, 20)) == [], "stale after the day's 9 am")
check(civic.notice_reminders(SC, ME, at(2026, 10, 10, 15)) == [], "nothing three days ahead")
r = civic.notice_reminders("stan meadows", "omar.haddad@example.com", at(2026, 10, 14, 8))
check([x["id"][-10:] for x in r] == ["2026-10-14"], "two-day notice: today only, not also tomorrow")
r = civic.notice_reminders("stan meadows", "omar.haddad@example.com", at(2026, 10, 13, 12))
check([x["id"][-10:] for x in r] == ["2026-10-14"], "two-day notice: the first day tomorrow")
check(civic.notice_reminders("harbor view", "chen.wei@example.com", at(2026, 10, 19, 15)) == [],
      "stack 07 hot water not for unit 905")
r = civic.notice_reminders("oak ridge", "tony.russo@example.com", at(2026, 11, 14, 15))
check(len(r) == 1 and "last day" in r[0]["say"] and "snow plow" in r[0]["say"], "a 'by' deadline tomorrow")
check(civic.notice_reminders(SC, ME, at(2026, 10, 12, 15)) == civic.notice_reminders(SC, ME, at(2026, 10, 12, 15)),
      "stable ids")

# --------------------------------------------------------------------------
# Every tool, every community, on the real library (no crash, JSON where promised)
# --------------------------------------------------------------------------
library._LIB = None
state.reset()
for c, d in COMMUNITIES.items():
    e = next(iter(d["homeowners"]))
    for name in ("plan_architectural", "explain_violation", "meeting_companion", "open_ballots"):
        try:
            out = call(name, c, e, project="fence", height_ft=7, color="purple")
            if not out.startswith("There are no"):
                json.loads(out)
        except Exception as x:                  # noqa: BLE001
            check(False, f"{name} on {c}: {x!r}")
    for name, a in (("register_to_speak", {"topic": "roads"}), ("submit_forum_question", {"question": "why?"})):
        check("simulated" in call(name, c, e, **a), f"{name} on {c}")
    b = next(b for b in civic.BALLOTS[c] if civic.ballot_open(b))
    check("Recorded" in call("cast_vote", c, e, ballot_id=b["id"], choice=b["choices"][0], confirm=True), f"vote {c}")
    check(civic.ballot_results(c)[b["id"]]["homes_voted"] == 1, f"results {c}")
    check(len(civic.meeting_requests(c)["questions"]) == 1, f"requests {c}")
library._LIB = None

print("civic: all passed" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
