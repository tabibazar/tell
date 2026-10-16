"""stan-mcp: an imaginary homeowners' association service, as an MCP server.

Four fictional communities (hoa_data.py) -- Sheppard Center (single-family),
Stan Meadows (gated townhomes), Harbor View (a condo tower) and Oak Ridge
(large lots, private roads) -- with their rules, amenities, finances, board,
events and residents, so stan-claw can talk about anything life in an HOA
brings up.

Public tools answer about a community; the homeowner's own account,
violations, requests, bookings and household need sign_in first (community +
email), which returns a homeowner token: an HMAC of the pair under MCP_TOKEN,
so nothing is stored. It identifies; it does not verify -- there is no code by
email, as befits a pretend service. Payments, bookings, passes, requests and
appeals are simulated: they return a confirmation but change nothing.

Runs as an AWS Lambda behind a function URL: MCP Streamable HTTP, one JSON
answer per POST, tools only. Every request needs
`Authorization: Bearer <MCP_TOKEN>` (an environment variable; never in git).
"""
import hashlib
import hmac
import json
import os
import re

import family
import civic
import library
import office
from family import ticket
from hoa_data import ALIASES, COMMUNITIES

PROTOCOL = "2025-06-18"


# --------------------------------------------------------------------------
# Finding things from what speech-to-text heard
# --------------------------------------------------------------------------
def norm_community(c):
    c = re.sub(r"[^a-z ]", " ", (c or "").lower())
    c = c.replace("shepherd", "sheppard").replace("shepard", "sheppard").replace("centre", "center")
    c = c.replace("harbour", "harbor").replace("oakridge", "oak ridge")
    c = re.sub(r"\b(hoa|community|homeowners|homeowner|owners|association|the|of|condominiums|condos|condo|"
               r"estates|townhome|townhomes|property|poa)\b", " ", c)
    return " ".join(c.split())


def find_community(c):
    n = norm_community(c)
    if n in COMMUNITIES:
        return n
    for key in COMMUNITIES:                       # "stan" -> stan meadows, "harbor" -> harbor view
        if n and (n in key or key in n or key.split()[0] == n.split()[0]):
            return key
    return None


def norm_email(e):
    e = (e or "").lower().strip()
    return e.replace(" at ", "@").replace(" dot ", ".").replace(" ", "")


def distance(a, b):
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def token_for(community, email):
    key = os.environ.get("MCP_TOKEN", "").encode()
    return "hw_" + hmac.new(key, f"{community}|{email}".encode(), hashlib.sha256).hexdigest()[:24]


def who(token):
    for c, data in COMMUNITIES.items():
        for e in data["homeowners"]:
            if token and hmac.compare_digest(token, token_for(c, e)):
                return c, e
    return None


def j(o):
    return json.dumps(o, ensure_ascii=False)


UNKNOWN = "I don't know that community. Communities on this service: {}."
NEEDS_SIGN_IN = ("Not signed in. Ask the homeowner which community they live in and their email, then call sign_in "
                 "and pass its homeowner_token.")


def names():
    return ", ".join(COMMUNITIES[k]["profile"]["name"] for k in COMMUNITIES)


# --------------------------------------------------------------------------
# The tools
# --------------------------------------------------------------------------
def t_list_communities(a):
    return j([{"name": d["profile"]["name"], "type": d["profile"]["type"], "homes": d["profile"]["homes"],
               "city": d["profile"]["city"]} for d in COMMUNITIES.values()])


def community_arg(a):
    c = find_community(a.get("community", ""))
    if c:
        return c, None
    who_ = who(a.get("homeowner_token", ""))         # signed in: their own community by default
    if who_ and not a.get("community"):
        return who_[0], None
    return None, UNKNOWN.format(names())


def public(fn):
    def run(a):
        c, err = community_arg(a)
        return err if err else fn(COMMUNITIES[c], a)
    return run


def t_community_info(d, a):
    return j({"profile": d["profile"], "management": d["management"],
              "amenities": list(d["amenities"]), "dues": d["dues"]})


def key_of(d):
    return next(k for k, v in COMMUNITIES.items() if v is d)


def sources(d, query, k=2):
    """The governing documents' own words on it, cited, for answers that can say where they come from."""
    hits = library.lib().search(key_of(d), query, k=k)
    return [{"cite": h["cite"], "text": h["text"][:900]} for h in hits]


def t_search_documents(d, a):
    q = a.get("query") or a.get("topic") or ""
    k = max(1, min(int(a.get("k") or 4), 8))
    hits = library.lib().search(key_of(d), q, k=k)
    if not hits:
        names = ", ".join(doc["title"] for doc in library.lib().docs.get(key_of(d), []))
        return f"Nothing in {d['profile']['name']}'s documents matches that. The documents: {names or 'none on file'}."
    return j([{"cite": h["cite"], "document": h["document"], "section": h["section"], "text": h["text"][:1500]}
              for h in hits])


def t_read_document(d, a):
    L = library.lib()
    doc = L.find_doc(key_of(d), a.get("document", ""))
    if not doc:
        names = ", ".join(f"{x['title']} ({x['slug']})" for x in L.docs.get(key_of(d), []))
        return f"No document called that. {d['profile']['name']} has: {names or 'none on file'}."
    if a.get("section"):
        sec = L.section(doc, a["section"])
        if not sec:
            return f"No section {a['section']} in {doc['title']}. Sections: " + ", ".join(x["heading"] for x in doc["sections"])
        return j({"cite": library.cite(doc, sec), "document": doc["title"], "section": sec["heading"], "text": sec["text"]})
    return j({"document": doc["title"], "type": doc["type"], "adopted": doc.get("adopted", ""),
              "amended": doc.get("amended", ""), "sections": [x["heading"] for x in doc["sections"]],
              "how": "read_document again with a section number for its text"})


def t_lookup_rule(d, a):
    summary = lookup_rule_summary(d, a)
    src = sources(d, a.get("topic") or "")
    return summary + ("\nFrom the documents: " + j(src) if src else "")


def lookup_rule_summary(d, a):
    words = re.findall(r"[a-z]+", (a.get("topic") or "").lower())
    rules = d["rules"]
    phrase = " ".join(words)
    for alias, key in ALIASES.items():
        if (alias in words or (" " in alias and alias in phrase)) and key in rules:
            return f"{d['profile']['name']}, {key}: {rules[key]}"
    for key, text in rules.items():
        for kw in key.split():
            if any(len(w) >= 4 and w[:4] == kw[:4] for w in words):
                return f"{d['profile']['name']}, {key}: {text}"
    return (f"No rule on that in {d['profile']['name']}'s documents. Rules cover: {', '.join(rules)}. "
            "Anything not covered is up to the board; ask management.")


def t_fines_schedule(d, a):
    return j(d["fines"])


def t_amenities(d, a):
    want = (a.get("amenity") or "").lower()
    if want:
        for name, info in d["amenities"].items():
            if want in name or name in want or want[:4] == name[:4]:
                return j({name: info})
        return f"{d['profile']['name']} has no amenity called that. It has: {', '.join(d['amenities'])}."
    return j(d["amenities"])


def t_events_and_notices(d, a):
    return j({"events": d["events"], "notices": d["notices"], "schedule": d["schedule"]})


def t_documents(d, a):
    lib_docs = [{"title": x["title"], "id": x["slug"], "type": x["type"], "adopted": x.get("adopted", ""),
                 "amended": x.get("amended", "")} for x in library.lib().docs.get(key_of(d), [])]
    return j({"documents": d["documents"], "library": lib_docs, "finances": d["finances"], "insurance": d["insurance"],
              "how_to_get_them": f"Copies from {d['management']['company']} ({d['management']['email']}) or the "
                                 f"portal; owners have a right to inspect the records within 10 business days."})


def t_board_and_meetings(d, a):
    return j({"board": d["board"], "committees": d["committees"], "meetings": d["meetings"]})


def t_who_fixes_what(d, a):
    item = (a.get("item") or "").lower()
    resp = d["responsibilities"]
    if item:
        words = re.findall(r"[a-z]+", item)
        for key, text in resp.items():
            if any(len(w) >= 4 and w[:4] in key for w in words):
                return f"{key}: {text}"
        return f"Not in the responsibility list. Listed: {', '.join(resp)}. Ask management for anything else."
    return j(resp)


def t_contacts(d, a):
    return j({"management": d["management"], "vendors": d["vendors"], "emergency": d["emergency"]})


def t_sign_in(a):
    c = find_community(a.get("community", ""))
    if not c:
        return UNKNOWN.format(names())
    e = norm_email(a.get("email", ""))
    people = COMMUNITIES[c]["homeowners"]
    match = next((known for known in people if distance(e, known) <= 2), None)
    if not match:
        return f"No homeowner with that email in {COMMUNITIES[c]['profile']['name']}. Check the spelling, or contact management."
    p = people[match]
    return (f"Signed in: {p['name']}, {p['address']} ({p['lot']}), {COMMUNITIES[c]['profile']['name']}. "
            f"homeowner_token: {token_for(c, match)}")


def private(fn):
    """A signed-in tool; each call goes in the homeowner's activity (family.log) for the weekly summary.
    run.tool, its name, is set when the tool list is built."""
    def run(a):
        w = who(a.get("homeowner_token", ""))
        if not w:
            return NEEDS_SIGN_IN
        c, e = w
        out = fn(COMMUNITIES[c], COMMUNITIES[c]["homeowners"][e], e, a)
        try:
            family.log(c, e, run.tool, a, out)
        except Exception as x:                  # noqa: BLE001 -- the summary's log never costs the answer
            print("activity log failed:", x)
        return out
    run.tool = ""
    return run


def fam(fn):
    """A family tool: family.t_* take the community key and the email."""
    return lambda d, p, e, a: fn(next(k for k, v in COMMUNITIES.items() if v is d), e, a)


def t_my_account(d, p, e, a):
    return j({"name": p["name"], "home": f"{p['address']} ({p['lot']})", "community": d["profile"]["name"],
              "occupancy": p["occupancy"], "dues": d["dues"], **p["account"]})


def t_make_payment(d, p, e, a):
    amt = float(a.get("amount") or 0)
    bal = p["account"]["balance_due"]
    if amt <= 0:
        return f"How much would they like to pay? The balance is ${bal:,.2f}."
    if amt > family.PAY_OK_LIMIT:
        return family.ask_approval(next(k for k, v in COMMUNITIES.items() if v is d), e, amt)
    return (f"Payment of ${amt:,.2f} accepted (simulated), confirmation {ticket('PAY', e, str(amt))}. "
            f"The balance would be ${max(bal - amt, 0):,.2f}. It posts in 1-2 business days.")


def t_payment_plan(d, p, e, a):
    bal = p["account"]["balance_due"]
    if p["account"].get("payment_plan"):
        return f"Already on a plan: {p['account']['payment_plan']}"
    if bal <= 0:
        return "The account is paid up; no plan needed."
    months = int(a.get("months") or 3)
    return (f"Policy: {d['dues'].get('payment_plans', 'ask management')}. A {months}-month plan on ${bal:,.2f} "
            f"would be ${bal / months:,.2f} a month on top of dues. Request {ticket('PLN', e, str(months))} sent to "
            f"management for approval (simulated); late fees stop while the plan is kept.")


def t_violations(d, p, e, a):
    return j(p["violations"] or "No open violations.")


def t_appeal_violation(d, p, e, a):
    vid = (a.get("violation_id") or "").upper()
    v = next((x for x in p["violations"] if x["id"] == vid), None)
    if not v:
        return f"No violation {vid} on this account. Open ones: {', '.join(x['id'] for x in p['violations']) or 'none'}."
    if not v.get("appealable"):
        return f"{vid} is resolved or not appealable: {v['status']}"
    when = d["meetings"]["next"]["when"]
    return (f"Appeal of {vid} filed (simulated), reference {ticket('APL', e, vid)}: \"{a.get('reason', '')[:200]}\". "
            f"The board will hear it at the next meeting, {when}; fines are paused until then.")


def t_my_requests(d, p, e, a):
    reqs = office.home_requests(key_of(d), e) or p["requests"]       # with the office's updates and new filings
    return j({"requests": reqs or "None open.", "reservations": p["reservations"] or "None."})


def t_file_request(d, p, e, a):
    kind = a.get("kind") if a.get("kind") in ("architectural", "maintenance", "complaint", "renovation") else "complaint"
    prefix = {"architectural": "ARC", "maintenance": "MNT", "complaint": "CMP", "renovation": "REN"}[kind]
    details = a.get("details", "")
    nxt = {"architectural": "The architectural committee decides within 30 days; no answer in 30 days counts as approval.",
           "maintenance": "Management looks at it within 3 business days (the same day for anything unsafe).",
           "complaint": "Management sends a courtesy notice; your name stays confidential.",
           "renovation": "Needs the renovation application, the contractor's insurance and the deposit before work starts."}[kind]
    ref = ticket(prefix, e, details)
    office.note_request(key_of(d), e, ref, kind, details)
    return f"Filed {kind} request {ref} (simulated): \"{details[:200]}\". {nxt}"


def t_reserve_amenity(d, p, e, a):
    want = (a.get("amenity") or "").lower()
    name, info = next(((n, i) for n, i in d["amenities"].items() if want and (want in n or n in want or want[:4] == n[:4])),
                      (None, None))
    if not name:
        return f"No amenity called that. {d['profile']['name']} has: {', '.join(d['amenities'])}."
    if not info.get("reservable"):
        return f"The {name} can't be booked; it is first come, first served. {info.get('rules', '')}"
    return (f"Booked the {name} for {a.get('when', 'the time asked')} (simulated), confirmation "
            f"{ticket('RSV', e, name, a.get('when', ''))}. Fee: {info.get('fee', 'free')}. Rules: {info.get('rules', '')} "
            f"Booking terms: {info.get('booking', '')}")


def t_guest_pass(d, p, e, a):
    code = int(hashlib.sha256(f"{e}|{a.get('guest_name')}|{a.get('date')}".encode()).hexdigest()[:4], 16) % 9000 + 1000
    parking = d["rules"].get("parking", "")
    extra = f" Gate one-time code {code}." if d["profile"].get("gated") else ""
    return (f"Guest pass for {a.get('guest_name', 'the guest')} on {a.get('date', 'the day asked')} "
            f"(plate {a.get('plate', 'not given')}) registered (simulated).{extra} Parking: {parking}")


def t_my_household(d, p, e, a):
    return j({"name": p["name"], "home": f"{p['address']} ({p['lot']})", "occupancy": p["occupancy"], **p["household"]})


def t_update_household(d, p, e, a):
    return (f"Updated (simulated): {a.get('kind', 'record')} -- \"{a.get('details', '')[:200]}\". "
            f"Reference {ticket('HH', e, a.get('details', ''))}. Management keeps pet, vehicle and tenant records for "
            f"parking, gate and emergency use.")


def t_resale_certificate(d, p, e, a):
    return (f"Resale (estoppel) certificate ordered (simulated), {ticket('RES', e)}: it shows the balance, any open "
            f"violations, pending assessments and the documents a buyer receives. Fee $250 ($100 more for rush), "
            f"ready in 10 business days (3 with rush). Current balance on the account: ${p['account']['balance_due']:,.2f}.")


TOK = {"homeowner_token": {"type": "string", "description": "from sign_in"}}
COM = {"community": {"type": "string", "description": "the community's name, as the person said it"}}


def schema(props=None, required=None):
    return {"type": "object", "properties": props or {}, **({"required": required} if required else {})}


TOOLS = [
    ("list_communities", "The communities on this service.", schema(), t_list_communities),
    ("community_info", "A community's overview: type, size, location, management company, amenities, dues.",
     schema(COM, ["community"]), public(t_community_info)),
    ("lookup_rule", "What a community's rules say about a topic: fences, parking, pets, paint, trash, rentals, noise, "
     "solar, signs, flags, sheds, grills, balconies, renovations, horses, septic, and more.",
     schema({**COM, "topic": {"type": "string"}}, ["community", "topic"]), public(t_lookup_rule)),
    ("fines_schedule", "The community's fine schedule and how long a homeowner has to fix a violation.",
     schema(COM, ["community"]), public(t_fines_schedule)),
    ("amenities", "Amenities (pool, clubhouse, gym, courts, guest suite, elevator, trails...): hours, rules, fees, "
     "how to book. Leave amenity empty for all.",
     schema({**COM, "amenity": {"type": "string"}}, ["community"]), public(t_amenities)),
    ("events_and_notices", "Upcoming events, current notices (water shut-offs, closures) and the trash, recycling, "
     "landscaping and snow schedule.", schema(COM, ["community"]), public(t_events_and_notices)),
    ("search_documents", "Search the community's governing documents (CC&Rs, bylaws, rules, architectural guidelines "
     "with approved colours, fine and collection policies, budget, minutes ...) and get the exact sections, cited. Use it "
     "for any question about what the rules say, fees, deadlines, procedures or what the board decided.",
     schema({**COM, **TOK, "query": {"type": "string", "description": "what to look for, in plain words"},
             "k": {"type": "integer", "description": "how many sections, 1-8 (default 4)"}}, ["query"]),
     public(t_search_documents)),
    ("read_document", "Read a governing document: its list of sections, or one section's full text (give its number, "
     "e.g. 7.4). `document` is its id or name from the documents tool, e.g. ccrs, bylaws, rules, minutes.",
     schema({**COM, **TOK, "document": {"type": "string"}, "section": {"type": "string"}}, ["document"]),
     public(t_read_document)),
    ("documents", "The governing documents (with the library of full texts), budget, reserve fund and study, insurance, "
     "and how to get copies.",
     schema(COM, ["community"]), public(t_documents)),
    ("board_and_meetings", "Board members, committees, the next meeting and agenda, how to speak, elections, voting.",
     schema(COM, ["community"]), public(t_board_and_meetings)),
    ("who_fixes_what", "Who is responsible for repairing something: roof, shared fence, sidewalk, trees, plumbing, "
     "windows, roads... Leave item empty for the whole list.",
     schema({**COM, "item": {"type": "string"}}, ["community"]), public(t_who_fixes_what)),
    ("contacts", "Management office, after-hours and emergency numbers, and the community's vendors.",
     schema(COM, ["community"]), public(t_contacts)),
    ("sign_in", "Sign a homeowner in before anything about their own account, violations, requests, bookings or "
     "household. Ask which community they live in and their email first. Returns a homeowner_token.",
     schema({**COM, "email": {"type": "string"}}, ["community", "email"]), t_sign_in),
    ("my_account", "The signed-in homeowner's account: balance, dues, late fees, payment history, plan, assessments.",
     schema(TOK, ["homeowner_token"]), private(t_my_account)),
    ("make_payment", "Pay toward the signed-in homeowner's balance (simulated; nothing is charged). Payments over $100 "
     "wait for a family member's OK by email; the tool says so.",
     schema({**TOK, "amount": {"type": "number"}}, ["homeowner_token", "amount"]), private(t_make_payment)),
    ("payment_plan", "Ask for a payment plan on an overdue balance (simulated request to management).",
     schema({**TOK, "months": {"type": "integer"}}, ["homeowner_token"]), private(t_payment_plan)),
    ("violations", "Open violation notices on the signed-in homeowner's home, with deadlines and hearings.",
     schema(TOK, ["homeowner_token"]), private(t_violations)),
    ("appeal_violation", "Appeal a violation to the board (simulated).",
     schema({**TOK, "violation_id": {"type": "string"}, "reason": {"type": "string"}},
            ["homeowner_token", "violation_id", "reason"]), private(t_appeal_violation)),
    ("my_requests", "The signed-in homeowner's open requests (architectural, maintenance, renovation, resale) and bookings.",
     schema(TOK, ["homeowner_token"]), private(t_my_requests)),
    ("file_request", "File an architectural, maintenance, renovation request or a complaint (simulated). Returns a ticket.",
     schema({**TOK, "kind": {"type": "string", "enum": ["architectural", "maintenance", "renovation", "complaint"]},
             "details": {"type": "string"}}, ["homeowner_token", "kind", "details"]), private(t_file_request)),
    ("reserve_amenity", "Book a reservable amenity (clubhouse, courts, guest suite, party room, service elevator, "
     "barn...) for a date and time (simulated).",
     schema({**TOK, "amenity": {"type": "string"}, "when": {"type": "string"}}, ["homeowner_token", "amenity", "when"]),
     private(t_reserve_amenity)),
    ("guest_pass", "Register a guest's visit and parking (and a one-time gate code in gated communities), simulated.",
     schema({**TOK, "guest_name": {"type": "string"}, "date": {"type": "string"}, "plate": {"type": "string"}},
            ["homeowner_token", "guest_name", "date"]), private(t_guest_pass)),
    ("my_household", "The signed-in homeowner's household record: pets, vehicles, tenant, gate code, fob, locker.",
     schema(TOK, ["homeowner_token"]), private(t_my_household)),
    ("update_household", "Register or change a pet, vehicle, tenant or emergency contact (simulated).",
     schema({**TOK, "kind": {"type": "string", "enum": ["pet", "vehicle", "tenant", "emergency contact"]},
             "details": {"type": "string"}}, ["homeowner_token", "kind", "details"]), private(t_update_household)),
    ("resale_certificate", "Order the resale (estoppel) certificate needed to sell the home (simulated).",
     schema(TOK, ["homeowner_token"]), private(t_resale_certificate)),
    ("notify_family", "Email the homeowner's family contacts a message from them (\"tell my daughter I'm fine\"). "
     "`to` picks one by name or relation; empty sends to all.",
     schema({**TOK, "message": {"type": "string"}, "to": {"type": "string"}}, ["homeowner_token", "message"]),
     private(fam(family.t_notify_family))),
    ("family_contacts", "The homeowner's family contacts: who gets their messages, approves payments over $100 and "
     "can send messages to their screen.", schema(TOK, ["homeowner_token"]), private(fam(family.t_family_contacts))),
    ("add_family_contact", "Add a trusted family member or friend as a contact (name, relation, email).",
     schema({**TOK, "name": {"type": "string"}, "relation": {"type": "string"}, "email": {"type": "string"}},
            ["homeowner_token", "name", "email"]), private(fam(family.t_add_family_contact))),
    ("send_family_summary", "Email the family contacts this week's summary now: what the homeowner asked about and "
     "did, and what needs attention.", schema(TOK, ["homeowner_token"]), private(fam(family.t_send_family_summary))),
    ("family_summary_consent", "Turn the Sunday weekly summary to family on or off, as the homeowner wishes.",
     schema({**TOK, "on": {"type": "boolean"}}, ["homeowner_token", "on"]), private(fam(family.t_family_summary_consent))),
    ("pending_approvals", "Payments waiting for a family member's OK, and whether they were approved.",
     schema(TOK, ["homeowner_token"]), private(fam(family.t_pending_approvals))),
    ("daily_checkin_consent", "Turn the weekday morning check-in on or off: around 11 am stan-claw says good morning, "
     "and if nobody answers it emails the family. Only with the homeowner's agreement.",
     schema({**TOK, "on": {"type": "boolean"}}, ["homeowner_token", "on"]), private(fam(family.t_daily_checkin_consent))),
]
TOOLS += [(n, desc, schema({**TOK, **props}, ["homeowner_token", *req]), private(fam(fn))) if kind == "private"
          else (n, desc, schema({**COM, **TOK, **props}, list(req)), public(lambda d, a, fn=fn: fn(key_of(d), a)))
          for n, desc, props, req, kind, fn in civic.CIVIC_TOOLS]


def t_board_reminders(c, e, a):
    """The board's spoken reminders: the HOA records' (family) and today's notices (civic)."""
    out = json.loads(family.t_reminders(c, e, a))
    seen = {r["id"] for r in out["reminders"]}
    out["reminders"] += [r for r in civic.notice_reminders(c, e, family.now()) if r["id"] not in seen]
    out["reminders"].sort(key=lambda r: r["at"])
    return json.dumps(out)


# Called by the stan-claw board itself, not offered to Claude.
HIDDEN = [
    ("reminders", private(fam(t_board_reminders))),
    ("family_inbox", private(fam(family.t_family_inbox))),
    ("family_photos", private(fam(family.t_family_photos))),
    ("family_media", private(fam(family.t_family_media))),
    ("checkin_missed", private(fam(family.t_checkin_missed))),
] + [(n, private(fam(f))) for n, f in office.HIDDEN_TOOLS]
for _n, _f in [(n, f) for n, _, _, f in TOOLS] + HIDDEN:
    if hasattr(_f, "tool"):
        _f.tool = _n
TOOL_LIST = [{"name": n, "description": d, "inputSchema": s} for n, d, s, _ in TOOLS]
TOOL_FN = {n: f for n, _, _, f in TOOLS}
TOOL_FN.update(dict(HIDDEN))


def call_tool(name, args):
    fn = TOOL_FN.get(name)
    if not fn:
        return None
    out = fn(args)
    try:                                             # anonymised stats for the HOA office's dashboard
        w = who(args.get("homeowner_token", ""))
        c_key = w[0] if w else community_arg(args)[0]
        office.record(c_key, w[1] if w else None, name, args, out if isinstance(out, str) else "")
    except Exception as x:                           # noqa: BLE001 -- stats never cost an answer
        print("stats failed:", x)
    return out


# --------------------------------------------------------------------------
# MCP over HTTP
# --------------------------------------------------------------------------
def rpc(msg):
    if not isinstance(msg, dict) or "method" not in msg:
        return 400, {"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "not a single JSON-RPC request"}}
    mid, method, params = msg.get("id"), msg["method"], msg.get("params") or {}
    if mid is None:
        return 202, None
    ok = lambda result: (200, {"jsonrpc": "2.0", "id": mid, "result": result})
    if method == "initialize":
        return ok({"protocolVersion": params.get("protocolVersion") or PROTOCOL,
                   "capabilities": {"tools": {"listChanged": False}},
                   "serverInfo": {"name": "stan-mcp", "version": "0.3.0"},
                   "instructions": "A fictional homeowners' association service for four communities: " + names() +
                                   ". Public tools answer about a community; before anything about a homeowner's own "
                                   "account, violations, requests, bookings or household, ask which community they "
                                   "live in and their email, and call sign_in. Many homeowners here are older: be "
                                   "patient, explain HOA terms in plain words, go one step at a time, say amounts and "
                                   "dates clearly, and describe and confirm any payment, booking, appeal or request "
                                   "before making it. Homeowners can have family contacts: notify_family emails them "
                                   "a message, payments over $100 wait for a family member's OK (say so kindly; it "
                                   "protects against scams), and family can send messages to the homeowner's screen. "
                                   "For what the rules, policies or minutes actually say, use search_documents and answer in plain "
                                   "words, saying which document and section it comes from (for example \"the CC&Rs, "
                                   "section 7.4, say ...\"). Never pay anyone the service does not list. A weekday morning check-in (daily_checkin_consent) "
                                   "only with the homeowner's clear agreement."})
    if method == "ping":
        return ok({})
    if method == "tools/list":
        return ok({"tools": TOOL_LIST})
    if method == "tools/call":
        text = call_tool(params.get("name"), params.get("arguments") or {})
        if text is None:
            return 200, {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "no such tool"}}
        return ok({"content": [{"type": "text", "text": text}]})
    return 200, {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}}


def form(event):
    """The query string, and for a POST the urlencoded form too, as one dict."""
    from urllib.parse import parse_qsl
    q = dict(parse_qsl(event.get("rawQueryString") or ""))
    if event.get("requestContext", {}).get("http", {}).get("method") == "POST":
        body = event.get("body") or ""
        if event.get("isBase64Encoded"):
            import base64
            body = base64.b64decode(body).decode()
        q.update(parse_qsl(body))
    return q


def handler(event, context):
    if event.get("weekly_summary"):                       # the Sunday EventBridge rule
        return {"sent": family.weekly_all()}
    headers = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
    method = event.get("requestContext", {}).get("http", {}).get("method", "POST")
    path = event.get("rawPath") or "/"
    if path.startswith("/mcp/"):                    # pages linked from the server's own address (.../mcp)
        path = path[4:]
    if path.startswith("/office"):                  # the HOA office console: its own login and cookie
        headers["cookie"] = "; ".join(event.get("cookies") or []) or headers.get("cookie", "")
        return office.handle_office(path, method, form(event), headers)
    if path in ("/family", "/approve") and method in ("GET", "POST"):   # family's pages: the link is signed
        return family.handle_page(path, method, form(event))
    token = os.environ.get("MCP_TOKEN", "")
    if not token or not hmac.compare_digest(headers.get("authorization", ""), "Bearer " + token):
        return {"statusCode": 401, "headers": {"WWW-Authenticate": "Bearer"}, "body": ""}
    if method != "POST":
        return {"statusCode": 405, "headers": {"Allow": "POST"}, "body": ""}
    body = event.get("body") or ""
    if event.get("isBase64Encoded"):
        import base64
        body = base64.b64decode(body).decode()
    try:
        msg = json.loads(body)
    except ValueError:
        return {"statusCode": 400, "headers": {"Content-Type": "application/json"},
                "body": json.dumps({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "parse error"}})}
    status, reply = rpc(msg)
    out = {"statusCode": status, "headers": {"Content-Type": "application/json"}, "body": json.dumps(reply) if reply else ""}
    if isinstance(msg, dict) and msg.get("method") == "initialize":
        out["headers"]["Mcp-Session-Id"] = hashlib.sha256(os.urandom(16)).hexdigest()[:32]
    return out
