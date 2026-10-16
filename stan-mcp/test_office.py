"""The HOA office console and its stats: python3 stan-mcp/test_office.py"""
import os, re, sys
sys.path.insert(0, os.path.dirname(__file__))
os.environ["MCP_TOKEN"] = "test-token"
os.environ.pop("STATE_TABLE", None)
os.environ.pop("MAIL_FROM", None)
import datetime as dt
import family, state
import office
from hoa_data import COMMUNITIES

NOW = dt.datetime(2026, 10, 10, 19, 0, tzinfo=dt.timezone.utc).timestamp()     # Saturday Oct 10, 3 pm EDT
clock = [NOW]
family.now = lambda: clock[0]
state.reset()

fails = 0
def check(c, what):
    global fails
    if not c:
        print("FAIL", what)
        fails += 1

C = "sheppard center"
HOMES = list(COMMUNITIES[C]["homeowners"])

def go(path, method="GET", q=None, cookie=None):
    return office.handle_office(path, method, q or {}, {"cookie": cookie} if cookie else {})

# Passwords and login
pw = office.office_password(C)
check(len(pw) == 10 and pw == family.sig("office", C)[:10], "password derived from MCP_TOKEN")
check(pw != office.office_password("oak ridge"), "a password per community")
r = go("/office")
check(r["statusCode"] == 200 and all(v["profile"]["name"] in r["body"] for v in COMMUNITIES.values())
      and pw not in r["body"], "login page lists the four communities and never the password")
r = go("/office/login", "POST", {"community": C, "password": "wrong"})
check(r["statusCode"] == 403 and "cookies" not in r, "bad password refused")
r = go("/office/login", "POST", {"community": "oak ridge", "password": pw})
check(r["statusCode"] == 403, "one community's password doesn't open another")
r = go("/office/login", "POST", {"community": C, "password": pw})
check(r["statusCode"] == 302 and r["headers"]["Location"] == "/office/home" and r.get("cookies"), "good password")
set_cookie = r["cookies"][0]
check(all(x in set_cookie for x in ("HttpOnly", "SameSite=Lax", "Path=/office", "Max-Age=43200")), "cookie flags")
COOKIE = set_cookie.split(";")[0]
check(go("/office", cookie=COOKIE)["headers"].get("Location") == "/office/home", "signed in: /office goes home")

# Pages refuse without a valid cookie
for p in ("/office/home", "/office/announce", "/office/alert", "/office/requests"):
    r = go(p)
    check(r["statusCode"] == 302 and r["headers"]["Location"] == "/office", f"{p} refused without a cookie")
check(go("/office/home", cookie="office=sheppard%20center%7C9999999999%7Cdeadbeef")["statusCode"] == 302,
      "forged cookie refused")
clock[0] = NOW + 13 * 3600
check(go("/office/home", cookie=COOKIE)["statusCode"] == 302, "expired cookie refused")
clock[0] = NOW
check(go("/office/home", cookie=COOKIE)["statusCode"] == 200, "dashboard with a cookie")
check(go("/office/logout", cookie=COOKIE)["cookies"][0].startswith("office=;"), "sign out clears the cookie")

# record(): anonymous stats
E = "maria.lopez@example.com"
office.record(C, E, "lookup_rule", {"topic": "Can Maria Lopez paint her fence purple? maria.lopez@example.com"},
              "Fences: white vinyl or natural wood, 6 ft at most.")
office.record(C, E, "search_documents", {"query": "pool hours on holidays"}, "The pool opens 9 to 8 ...")
office.record(C, "info@avriz.io", "my_account", {}, '{"balance_due": 0}')
office.record(C, None, "lookup_rule", {"topic": "drone flying"}, "No rule found about drone flying.")
office.record(C, E, "file_request", {"kind": "maintenance", "details": "Pothole by the mailbox"},
              'Filed maintenance request MNT-2026-4444 (simulated): "Pothole by the mailbox". ...')
office.record(C, E, "make_payment", {"amount": 50}, "Payment of $50.00 accepted (simulated), confirmation PAY-2026-1111.")
office.record(C, E, "make_payment", {"amount": 500}, "This $500.00 payment needs a family member's OK first -- ...")
office.record(C, E, "reminders", {}, '{"reminders": []}')                       # the board's polling: ignored
office.record(C, E, "family_inbox", {}, '{"messages": []}')
office.record(C, E, "pending_approvals", {}, json_no := '"No payments waiting."')
office.record(C, E, "lookup_rule", None, None)                                    # junk never raises
office.record(None, E, "lookup_rule", {}, "x")
office.record("nowhere", E, "lookup_rule", {}, "x")
office.record(C, E, "contacts", {"question": object()}, 12345)
check(office._answered("Fine") and not office._answered("No such thing") and not office._answered("Nothing yet")
      and not office._answered("Not signed in. Ask...") and not office._answered("I don't know that community.")
      and not office._answered(json_no) and not office._answered(""), "unanswered detection")
stats = [(sk, it) for (pk, sk), it in state._mem.items() if pk == f"office#{C}" and sk.startswith("stat#")]
check(len(stats) == 1 and stats[0][0] == "stat#2026-10-10", "one stats item for the local day")
raw = stats[0][1]["d"].lower()
check(all(x not in raw for x in ("maria", "lopez", "@", "avriz", "info")), "no email or name in the stats")
check(stats[0][1].get("ttl", 0) > NOW + 80 * 86400, "stats kept about 90 days")
d = state.get(f"office#{C}", "stat#2026-10-10")
check("reminders" not in d["tools"] and "family_inbox" not in d["tools"], "the board's polling isn't counted")
check({"fence", "purple", "paint"} <= set(d["topics"]) and "can" not in d["topics"] and "her" not in d["topics"],
      f"topic words, stop words removed ({sorted(d['topics'])})")
check(len(office.topic_words(C, {"query": "one two three four five six seven eight nine ten eleven"})) == 8,
      "at most 8 topic words")
check("drone flying" in d["unanswered_topics"], "an unanswered question's topic")
check("account balance" in d["topics"], "a tool with no free text gets a topic label")
n = office.numbers(C, 1)
# answered questions: fence, pool, my_account, contacts = 4; unanswered: drone, junk lookup_rule = 2;
# actions done: the filed request and the $50 payment = 2 (the $500 one waits for family)
check((n["answered"], n["unanswered"], n["actions"]) == (4, 2, 2), f"counts {n['answered'], n['unanswered'], n['actions']}")
check(n["homes"] == 2, f"homes using their panel ({n['homes']})")
check(n["calls"] == 6 and abs(n["hours"] - 0.4) < 1e-9 and office.calls_saved_hours(10, 5) == 1.0,
      "calls saved: (answered + actions) x 4 minutes")
clock[0] = NOW - 3 * 86400                                                       # Wednesday
office.record(C, E, "amenities", {"item": "tennis courts"}, "Two courts ...")
clock[0] = NOW - 10 * 86400                                                      # outside this week
office.record(C, E, "amenities", {"item": "tennis courts"}, "Two courts ...")
clock[0] = NOW
check(office.numbers(C, 7)["answered"] == 5 and office.numbers(C, 14)["answered"] == 6, "this week vs 14 days")
h = go("/office/home", cookie=COOKIE)["body"]
check("0.5 hours" in h and "7 calls" in h and "4 minutes" in h and "drone flying" in h and "fence" in h, "dashboard numbers")
check(h.count("class='bar'") == 14 and "<script" not in h, "a 14-day bar chart without scripts")
check("maria" not in h.lower(), "dashboard names no one in the stats")
check("MNT-2026-2210" in h and "ARC-2026-1240" in h and "ARC-2026-1187" not in h, "open requests (approved is closed)")

# CSRF
page = go("/office/announce", cookie=COOKIE)["body"]
CSRF = re.search(r"name='csrf' value='([0-9a-f]+)'", page).group(1)
before = len(state.query(family.hw_key(C, HOMES[0]), "msg#"))
r = go("/office/announce", "POST", {"title": "Pool", "message": "The pool closes Monday."}, cookie=COOKIE)
check(r["statusCode"] == 403 and len(state.query(family.hw_key(C, HOMES[0]), "msg#")) == before, "missing CSRF refused")
r = go("/office/announce", "POST", {"title": "Pool", "message": "x", "csrf": "0" * 32}, cookie=COOKIE)
check(r["statusCode"] == 403, "wrong CSRF refused")
other = go("/office/login", "POST", {"community": "oak ridge", "password": office.office_password("oak ridge")})
OAK = other["cookies"][0].split(";")[0]
check(go("/office/announce", "POST", {"message": "x", "csrf": CSRF}, cookie=OAK)["statusCode"] == 403,
      "a CSRF value works only with its own session")
check(go("/office/requests", "POST", {"ref": "MNT-2026-2210", "status": "x"}, cookie=COOKIE)["statusCode"] == 403,
      "requests POST needs CSRF too")

# Announcements reach every homeowner; heard counts
r = go("/office/announce", "POST", {"title": "Pool closing", "message": "The pool closes for the season on Monday, "
                                    "October 12.", "csrf": CSRF}, cookie=COOKIE)
check(r["statusCode"] == 200 and f"Sent to {len(HOMES)} panels" in r["body"], "announcement sent")
ann = [a for _, a in state.query(f"office#{C}", "ann#")]
check(len(ann) == 1 and set(ann[0]["msgs"]) == set(HOMES), "the announcement keeps {email: message id}")
for e in HOMES:
    m = next(x for _, x in state.query(family.hw_key(C, e), "msg#") if x["text"].startswith("Pool closing"))
    check(m["from"] == "The Board" and m["text"].startswith("Pool closing. The pool closes") and m["id"] == ann[0]["msgs"][e],
          f"in {e}'s inbox")
check(not state.query(family.hw_key("oak ridge", "sarah.mills@example.com"), "msg#"), "not in another community")
check(f"heard on 0 of {len(HOMES)} panels" in go("/office/announce", cookie=COOKIE)["body"], "heard 0 at first")
check(dict(office.HIDDEN_TOOLS)["message_heard"] is office.message_heard, "message_heard is a hidden tool")
for e in HOMES[:2]:
    check(office.message_heard(C, e, {"id": ann[0]["msgs"][e]}) == "ok", "message_heard says ok")
office.message_heard(C, HOMES[2], {"id": "not-its-id"})                          # a different message: no change
check(office.message_heard(C, HOMES[3], {}) == "ok" and office.message_heard(C, HOMES[3], None) == "ok", "junk ok")
b = go("/office/announce", cookie=COOKIE)["body"]
check(f"heard on 2 of {len(HOMES)} panels" in b, "heard count goes up")
p2, p3 = (COMMUNITIES[C]["homeowners"][e] for e in HOMES[2:4])
check(p2["address"] in b and p3["address"] in b and COMMUNITIES[C]["homeowners"][HOMES[0]]["address"] not in b.split("Not heard yet")[1],
      "who hasn't heard, by address")
check(not any(e in b for e in HOMES), "no emails on the page")
check(f"heard on 2 of {len(HOMES)} panels" in go("/office/home", cookie=COOKIE)["body"], "dashboard heard count")
r = go("/office/alert", cookie=COOKIE)
check("Water shut-off" in r["body"] and "name='urgent' checked" in r["body"], "alert page: templates, urgent ticked")
check("Elevator out of service" in go("/office/alert", q={"t": "elevator"}, cookie=COOKIE)["body"], "a template fills in")
r = go("/office/alert", "POST", {"title": "Water shut-off", "message": "Water off on [day].", "urgent": "on",
                                 "csrf": CSRF}, cookie=COOKIE)
check("Fill in the [brackets]" in r["body"] and len(state.query(f"office#{C}", "ann#")) == 1, "unfilled template refused")
go("/office/alert", "POST", {"title": "Water shut-off", "message": "Water off Tuesday 9 to noon.", "urgent": "on",
                             "csrf": CSRF}, cookie=COOKIE)
m = [x for _, x in state.query(family.hw_key(C, HOMES[1]), "msg#") if "Water off" in x["text"]]
check(len(m) == 1 and m[0]["from"] == "Urgent: The Board", "urgent alert sender")
r = go("/office/announce", "POST", {"message": "x" * 401, "csrf": CSRF}, cookie=COOKIE)
check("400 characters" in r["body"] and len(state.query(f"office#{C}", "ann#")) == 2, "over 400 characters refused")

# Requests: status updates spoken on the right home, request_status, note_request
check(office.request_status(C, "info@avriz.io", "MNT-2026-2210") is None, "no office update yet")
r = go("/office/requests", cookie=COOKIE)
check(all(x in r["body"] for x in ("MNT-2026-2210", "ARC-2026-1187", "RES-2026-0093")), "requests page lists all")
n_maria = len(state.query(family.hw_key(C, E), "msg#"))
r = go("/office/requests", "POST", {"ref": "MNT-2026-2210", "status": "Fixed on October 9.", "notify": "on",
                                    "csrf": CSRF}, cookie=COOKIE)
check(r["statusCode"] == 200 and "told the homeowner" in r["body"], "status updated")
m = [x for _, x in state.query(family.hw_key(C, "info@avriz.io"), "msg#") if "MNT-2026-2210" in x["text"]]
check(len(m) == 1 and m[0]["text"] == "Update on your maintenance request MNT-2026-2210: Fixed on October 9."
      and m[0]["from"] == "HOA Office", "spoken update on the right home")
check(len(state.query(family.hw_key(C, E), "msg#")) == n_maria, "not posted to another home")
check(office.request_status(C, "info@avriz.io", "MNT-2026-2210") == "Fixed on October 9.", "request_status")
check(office.request_status(C, E, "MNT-2026-2210") is None, "request_status only for the home's own request")
check("MNT-2026-2210" not in go("/office/home", cookie=COOKIE)["body"], "a fixed request leaves the open list")
check("not heard yet" in go("/office/requests", cookie=COOKIE)["body"], "the update's heard state shows")
n_avriz = len(state.query(family.hw_key(C, "info@avriz.io"), "msg#"))
go("/office/requests", "POST", {"ref": "ARC-2026-1240", "status": "Committee meets Nov 2.", "csrf": CSRF}, cookie=COOKIE)
check(office.request_status(C, "priya.patel@example.com", "ARC-2026-1240") == "Committee meets Nov 2."
      and not any("ARC-2026-1240" in mm["text"] for _, mm in state.query(family.hw_key(C, "priya.patel@example.com"), "msg#")),
      "an update without notify isn't posted")
r = go("/office/requests", "POST", {"ref": "ARC-2026-007", "status": "x", "notify": "on", "csrf": CSRF}, cookie=COOKIE)
check(r["statusCode"] == 404, "another community's request refused")
office.note_request(C, "james.chen@example.com", "CMP-2026-5555", "complaint", "Barking dog at night on Maple Ridge")
r = go("/office/requests", cookie=COOKIE)["body"]
check("CMP-2026-5555" in r and "Barking dog" in r and "112 Maple Ridge Drive" in r, "note_request listed")
check(office.request_status(C, "james.chen@example.com", "CMP-2026-5555") == "Received; not reviewed yet.",
      "a new filing's status")
check(any(x["id"] == "CMP-2026-5555" for x in office.home_requests(C, "james.chen@example.com")), "home_requests")
family.log(C, "maria.lopez@example.com", "file_request", {"kind": "maintenance", "details": "Loose paver on the path"},
           'Filed maintenance request MNT-2026-7777 (simulated): "Loose paver on the path". ...')
check("MNT-2026-7777" in go("/office/requests", cookie=COOKIE)["body"], "a filing found in the activity log")
check("CMP-2026-5555" in go("/office/home", cookie=COOKIE)["body"], "new filings are open requests on the dashboard")
check(office._closed("Approved on Oct 2.") and office._closed("Fixed on October 9.") and not office._closed("Not fixed yet")
      and not office._closed("Paperwork incomplete") and not office._closed("Repair booked"), "closed detection")
check(go("/office/nope", cookie=COOKIE)["statusCode"] == 404 and go("/office/home", "PUT")["statusCode"] == 405,
      "unknown page, bad method")

print("office tests:", "all passed" if not fails else f"{fails} failed")
sys.exit(1 if fails else 0)
