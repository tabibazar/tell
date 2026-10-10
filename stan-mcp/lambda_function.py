"""stan-mcp: an imaginary homeowners' association service, as an MCP server.

Sheppard Center HOA, a made-up community, so stan-claw has something to talk
to about life in an HOA: the bylaws, a homeowner's account, violation
notices, requests to the architectural committee, the board's next meeting.

A homeowner signs in first with their community and email (sign_in), which
returns a token the account tools need. The token is an HMAC of the pair
under MCP_TOKEN, so nothing is stored. This identifies; it does not verify --
there is no code by email, as befits a pretend service.

Runs as an AWS Lambda behind a function URL: MCP Streamable HTTP, one JSON
answer per POST, protocol 2025-06-18, tools only. Every request needs
`Authorization: Bearer <MCP_TOKEN>` (an environment variable; never in git).
Everything here is fiction.
"""
import hashlib
import hmac
import json
import os

PROTOCOL = "2025-06-18"

COMMUNITY = "Sheppard Center HOA (a fictional community of 212 homes)"

# Who may sign in: community -> email -> their home. One homeowner for now.
HOMEOWNERS = {
    "sheppard center": {
        "info@avriz.io": {"address": "27 Willow Lane", "lot": "Lot 114", "owner_since": "2021"},
    },
}

RULES = {
    "fences": ("Section 7.2", "Fences may be at most 6 feet tall in back yards and 4 feet in front, in white vinyl or "
               "natural cedar. Chain link is not allowed. Any new fence needs architectural approval first; "
               "review takes up to 30 days."),
    "parking": ("Section 9.1", "Cars park in garages or driveways. Street parking is allowed for guests up to 72 hours. "
                "Boats, RVs and trailers may stay at most 48 hours for loading. Commercial vehicles over one ton "
                "may not be kept overnight."),
    "pets": ("Section 11.4", "Up to three household pets. Dogs on a leash in common areas, waste picked up at once. "
             "Persistent barking after two written warnings is a violation with a $50 fine."),
    "paint": ("Section 7.5", "Exterior colours must come from the approved palette (14 colours, at the clubhouse and "
              "online). Repainting in the same colour needs no approval; a new colour does."),
    "trash": ("Section 10.2", "Bins may go to the curb after 6 pm the night before pickup (Tuesday) and must be back "
              "out of sight from the street by 8 pm on pickup day."),
    "short-term rentals": ("Section 12.3", "Rentals shorter than 30 days are not allowed. Longer leases must be "
                           "registered with management, with a copy of the lease, within 10 days."),
    "noise": ("Section 11.1", "Quiet hours are 10 pm to 7 am. Construction and lawn equipment only 8 am to 6 pm on "
              "weekdays, 9 am to 5 pm on Saturdays, never on Sundays."),
    "solar": ("Section 7.9", "Solar panels are allowed (state law protects them). Roof panels should sit flush and "
              "face the back or side where it does not cost more than 10% of output. Submit a request so the "
              "committee can note it; it cannot be refused on looks alone."),
    "satellite dishes": ("Section 7.10", "Dishes under 1 metre are allowed where reception works, preferably not "
                         "facing the street. No approval needed."),
    "holiday decorations": ("Section 8.3", "Up from 30 days before a holiday, down within 15 days after."),
    "pool": ("Section 13.1", "The community pool is open Memorial Day to Labor Day, 8 am to 9 pm. Two guests per "
             "home; children under 14 need an adult. Key fobs from management, $25 to replace."),
    "landscaping": ("Section 7.7", "Lawns kept under 6 inches, beds weeded. Removing a tree over 6 inches across "
                    "needs approval. Drought-tolerant landscaping is welcome and has a fast-track review."),
    "sheds": ("Section 7.4", "One shed per lot, at most 120 square feet and 10 feet high, behind the house, in a "
              "colour matching the home. Needs architectural approval."),
}

ACCOUNT = {
    "monthly_dues": 285.00,
    "balance_due": 595.00,
    "detail": "Dues for September and October ($570) plus one $25 late fee.",
    "late_fee_policy": "A $25 late fee after the 15th; after 90 days the account goes to collections and a lien may be filed.",
    "autopay": False,
    "last_payment": "August 3, 2026: $285.00",
    "special_assessment": "A one-time $400 roof-repair assessment for the clubhouse is due January 15, 2027.",
}

VIOLATIONS = [
    {"id": "V-2026-0412", "opened": "October 3, 2026", "rule": "Section 10.2 (trash)",
     "notice": "Trash bins visible from the street on a non-pickup day.",
     "status": "Courtesy notice. Cure by October 17 or a $50 fine applies."},
    {"id": "V-2026-0388", "opened": "September 20, 2026", "rule": "Section 7.4 (sheds)",
     "notice": "Shed painted a colour not on the approved palette.",
     "status": "Second notice. A hearing with the board is set for October 22; you may attend and explain, "
               "or apply for approval of the colour before then."},
]

MEETING = {
    "when": "Thursday, October 22, 2026, 7:00 pm",
    "where": "The clubhouse, 1 Meadow Circle (and by video; the link is sent the day before)",
    "agenda": ["2027 budget and dues (a proposed rise from $285 to $299 a month)", "Clubhouse roof assessment",
               "Violation hearings, including V-2026-0388", "Pool resurfacing bids", "Homeowner open forum"],
    "speaking": "Sign up with management at least 24 hours before; each homeowner gets 3 minutes in the open forum.",
}

MANAGEMENT = {
    "company": "Lakeside Property Management (fictional)",
    "manager": "Dana Ortiz, community manager",
    "phone": "555-0142", "email": "sheppardcenter@lakeside.example",
    "hours": "Weekdays 9 am to 5 pm; an after-hours line for emergencies such as a burst pipe in a common area.",
}

def _norm_community(c):
    import re
    c = re.sub(r"[^a-z ]", " ", (c or "").lower())
    c = c.replace("shepherd", "sheppard").replace("shepard", "sheppard").replace("centre", "center")
    c = re.sub(r"\b(hoa|community|homeowners|association|the|of)\b", " ", c)
    return " ".join(c.split())


def _norm_email(e):
    e = (e or "").lower().strip()
    e = e.replace(" at ", "@").replace(" dot ", ".").replace(" ", "")
    return e


def _distance(a, b):
    """Edit distance, for an email heard slightly wrong ("avris" for "avriz")."""
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def _token_for(community, email):
    key = os.environ.get("MCP_TOKEN", "").encode()
    return "hw_" + hmac.new(key, f"{community}|{email}".encode(), hashlib.sha256).hexdigest()[:24]


def sign_in(community, email):
    c = _norm_community(community)
    if c not in HOMEOWNERS:
        return None, (f"I don't know a community called \"{community}\". Communities on this service: " +
                      ", ".join(k.title() for k in HOMEOWNERS) + ".")
    e = _norm_email(email)
    match = next((known for known in HOMEOWNERS[c] if _distance(e, known) <= 2), None)
    if not match:
        return None, f"No homeowner with that email in {c.title()}. Check the spelling, or contact management."
    home = HOMEOWNERS[c][match]
    return _token_for(c, match), (f"Signed in: {match}, {home['address']} ({home['lot']}), {c.title()}. "
                                  f"Pass homeowner_token to the account tools.")


def _who(token):
    """The (community, email) a homeowner token belongs to, or None."""
    for c, people in HOMEOWNERS.items():
        for e in people:
            if token and hmac.compare_digest(token, _token_for(c, e)):
                return c, e
    return None


NEEDS_SIGN_IN = ("Not signed in. Ask the homeowner which community they live in and their email, then call sign_in "
                 "and pass its homeowner_token.")

TOOLS = [
    {"name": "sign_in", "description": "Sign a homeowner in before anything about their own account, violations or "
     "requests. Ask them which community they live in and their email address first. Returns a homeowner_token.",
     "inputSchema": {"type": "object", "properties": {"community": {"type": "string"}, "email": {"type": "string"}},
                     "required": ["community", "email"]}},
    {"name": "lookup_rule", "description": "What Sheppard Center HOA's bylaws say about a topic (fences, parking, pets, paint, "
     "trash, short-term rentals, noise, solar, satellite dishes, holiday decorations, pool, landscaping, sheds).",
     "inputSchema": {"type": "object", "properties": {"topic": {"type": "string"}}, "required": ["topic"]}},
    {"name": "my_account", "description": "The signed-in homeowner's HOA account: dues, balance, late fees, last payment, "
     "assessments. Needs homeowner_token from sign_in.",
     "inputSchema": {"type": "object", "properties": {"homeowner_token": {"type": "string", "description": "from sign_in"}}, "required": ["homeowner_token"]}},
    {"name": "violations", "description": "Open violation notices on the signed-in homeowner's lot, with deadlines and "
     "hearings. Needs homeowner_token from sign_in.",
     "inputSchema": {"type": "object", "properties": {"homeowner_token": {"type": "string", "description": "from sign_in"}}, "required": ["homeowner_token"]}},
    {"name": "file_request", "description": "File an architectural request (deck, fence, paint, solar...), a maintenance "
     "request for a common area, or a complaint, for the signed-in homeowner. Returns a ticket number and what happens "
     "next. Needs homeowner_token from sign_in.",
     "inputSchema": {"type": "object", "properties": {
         "homeowner_token": {"type": "string", "description": "from sign_in"},
         "kind": {"type": "string", "enum": ["architectural", "maintenance", "complaint"]},
         "details": {"type": "string"}}, "required": ["homeowner_token", "kind", "details"]}},
    {"name": "next_board_meeting", "description": "When and where the board meets next, the agenda, and how to speak.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "contact_management", "description": "How to reach the community's property management company.",
     "inputSchema": {"type": "object", "properties": {}}},
]


ALIASES = {"dog": "pets", "cat": "pets", "bark": "pets", "garbage": "trash", "bin": "trash", "bins": "trash",
           "recycling": "trash", "rent": "short-term rentals", "rental": "short-term rentals", "airbnb": "short-term rentals",
           "lease": "short-term rentals", "tree": "landscaping", "lawn": "landscaping", "grass": "landscaping",
           "colour": "paint", "color": "paint", "car": "parking", "boat": "parking", "truck": "parking", "rv": "parking",
           "quiet": "noise", "loud": "noise", "music": "noise", "dish": "satellite dishes", "christmas": "holiday decorations",
           "lights": "holiday decorations", "swim": "pool", "shed": "sheds", "panel": "solar", "panels": "solar"}


def lookup_rule(topic):
    import re
    words = re.findall(r"[a-z]+", (topic or "").lower())
    for w in words:                                   # a synonym first: "my dog" -> pets
        if w in ALIASES:
            key = ALIASES[w]
            return f"{RULES[key][0]}, {key}: {RULES[key][1]}"
    for key, (section, text) in RULES.items():        # then the topic's own words, by their first four letters
        for kw in key.split():
            if any(len(w) >= 4 and w[:4] == kw[:4] for w in words):
                return f"{section}, {key}: {text}"
    return ("No rule by that name in the Sheppard Center bylaws. Topics covered: " + ", ".join(RULES) +
            ". Anything not covered is up to the board; ask management.")


def file_request(kind, details):
    kind = kind if kind in ("architectural", "maintenance", "complaint") else "complaint"
    prefix = {"architectural": "ARC", "maintenance": "MNT", "complaint": "CMP"}[kind]
    ticket = f"{prefix}-2026-{int(hashlib.sha256(details.encode()).hexdigest()[:6], 16) % 9000 + 1000}"
    nxt = {"architectural": "The architectural committee meets on the first Monday of the month and decides within 30 "
                            "days; if they have not answered in 30 days the request counts as approved.",
           "maintenance": "Management will look at it within 3 business days (same day for anything unsafe).",
           "complaint": "Management will contact the neighbour with a courtesy notice; your name stays confidential."}[kind]
    return f"Filed {kind} request {ticket}: \"{details[:200]}\". {nxt}"


def call_tool(name, args):
    if name == "sign_in":
        token, text = sign_in(args.get("community", ""), args.get("email", ""))
        return text + (f" homeowner_token: {token}" if token else "")
    if name in ("my_account", "violations", "file_request"):
        who = _who(args.get("homeowner_token", ""))
        if not who:
            return NEEDS_SIGN_IN
        home = HOMEOWNERS[who[0]][who[1]]
        if name == "my_account":
            return json.dumps({"community": COMMUNITY, "email": who[1], "home": home, **ACCOUNT})
        if name == "violations":
            return json.dumps(VIOLATIONS)
        return file_request(args.get("kind", ""), args.get("details", ""))
    if name == "lookup_rule":
        return lookup_rule(args.get("topic", ""))
    if name == "next_board_meeting":
        return json.dumps(MEETING)
    if name == "contact_management":
        return json.dumps(MANAGEMENT)
    return None


def rpc(msg):
    """One JSON-RPC message in; (http status, reply dict or None)."""
    if not isinstance(msg, dict) or "method" not in msg:
        return 400, {"jsonrpc": "2.0", "id": None, "error": {"code": -32600, "message": "not a single JSON-RPC request"}}
    mid, method, params = msg.get("id"), msg["method"], msg.get("params") or {}
    if mid is None:
        return 202, None                              # a notification: accepted
    ok = lambda result: (200, {"jsonrpc": "2.0", "id": mid, "result": result})
    if method == "initialize":
        return ok({"protocolVersion": params.get("protocolVersion") or PROTOCOL,
                   "capabilities": {"tools": {"listChanged": False}},
                   "serverInfo": {"name": "stan-mcp", "version": "0.1.0"},
                   "instructions": "Sheppard Center HOA, a fictional homeowners' association. Use the tools to answer "
                                   "a homeowner's questions about rules, dues, violations, requests and meetings."})
    if method == "ping":
        return ok({})
    if method == "tools/list":
        return ok({"tools": TOOLS})
    if method == "tools/call":
        text = call_tool(params.get("name"), params.get("arguments") or {})
        if text is None:
            return 200, {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "no such tool"}}
        return ok({"content": [{"type": "text", "text": text}]})
    return 200, {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}}


def handler(event, context):
    headers = {k.lower(): v for k, v in (event.get("headers") or {}).items()}
    method = event.get("requestContext", {}).get("http", {}).get("method", "POST")
    token = os.environ.get("MCP_TOKEN", "")
    given = headers.get("authorization", "")
    if not token or not hmac.compare_digest(given, "Bearer " + token):
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
    if msg.get("method") == "initialize":
        out["headers"]["Mcp-Session-Id"] = hashlib.sha256(os.urandom(16)).hexdigest()[:32]
    return out
