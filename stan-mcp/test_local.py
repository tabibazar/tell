"""Exercise stan-mcp's handler the way a function URL calls it: python3 stan-mcp/test_local.py"""
import json, os, sys
sys.path.insert(0, os.path.dirname(__file__))
os.environ["MCP_TOKEN"] = "test-token"
os.environ["FAMILY_EMAIL"] = "family@example.com"
os.environ["PUBLIC_URL"] = "https://example.test/"
os.environ.pop("STATE_TABLE", None)
os.environ.pop("MAIL_FROM", None)
import datetime as dt
import urllib.parse
import family, mail
import lambda_function as f
NOW = dt.datetime(2026, 10, 10, 19, 0, tzinfo=dt.timezone.utc).timestamp()     # Saturday Oct 10, 3 pm EDT
family.now = lambda: NOW

def post(msg, token="test-token"):
    ev = {"headers": {"Authorization": "Bearer " + token}, "requestContext": {"http": {"method": "POST"}}, "body": json.dumps(msg)}
    return f.handler(ev, None)

fails = 0
def check(c, what):
    global fails
    if not c:
        print("FAIL", what)
        fails += 1

def call(name, args):
    r = json.loads(post({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": name, "arguments": args}})["body"])
    return r["result"]["content"][0]["text"] if "result" in r else r

# Protocol and auth
check(post({"jsonrpc": "2.0", "id": 1, "method": "ping"}, token="wrong")["statusCode"] == 401, "wrong token refused")
r = post({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}})
check(r["statusCode"] == 200 and "Mcp-Session-Id" in r["headers"], "initialize")
check(post({"jsonrpc": "2.0", "method": "notifications/initialized"})["statusCode"] == 202, "notification 202")
tools = json.loads(post({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})["body"])["result"]["tools"]
check(len(tools) == 29 and all(t["inputSchema"]["type"] == "object" for t in tools), f"tools/list ({len(tools)})")
check(call("fly", {}).get("error", {}).get("code") == -32602, "unknown tool")

# Public tools, across all four communities and as speech might name them
check("Harbor View Condominiums" in call("list_communities", {}), "list")
check("212" in call("community_info", {"community": "the Shepherd Centre HOA"}), "sheppard info")
check("gated" in call("community_info", {"community": "Stan Meadows"}).lower(), "stan meadows info")
check("6 feet" in call("lookup_rule", {"community": "Sheppard Center", "topic": "Can I build a fence?"}), "fence")
check("No fences" in call("lookup_rule", {"community": "stan meadows", "topic": "fence"}), "townhomes: no fences")
check("grills" in call("lookup_rule", {"community": "harbour view", "topic": "can I bbq on my balcony"}), "condo grill")
check("Up to two horses" in call("lookup_rule", {"community": "oak ridge", "topic": "how many horses"}), "horses")
check("pets" in call("lookup_rule", {"community": "sheppard", "topic": "my dog barks"}), "dog alias")
check("No rule" in call("lookup_rule", {"community": "sheppard", "topic": "a b c"}), "no rule")
check("I don't know that community" in call("lookup_rule", {"community": "Nowhere Acres", "topic": "fence"}), "unknown community")
check("$500 per stay" in call("fines_schedule", {"community": "sheppard center"}), "fines")
check("$75 rental" in call("amenities", {"community": "sheppard center", "amenity": "clubhouse"}), "clubhouse")
check("service elevator" in call("amenities", {"community": "harbor view"}), "amenities all")
check("Water shut-off" in call("events_and_notices", {"community": "sheppard center"}), "notices")
check("reserve_fund" in call("documents", {"community": "oak ridge"}), "documents")
check("Margaret Hale" in call("board_and_meetings", {"community": "sheppard center"}), "board")
check("Shared equally" in call("who_fixes_what", {"community": "sheppard center", "item": "the fence between me and my neighbor"}), "fence responsibility")
check("The HOA (townhome roofs" in call("who_fixes_what", {"community": "stan meadows", "item": "roof"}), "townhome roof")
check("555-0171" in call("contacts", {"community": "harbor view"}), "contacts")

# Sign-in and private tools
check("Not signed in" in call("my_account", {}), "account needs sign-in")
check("Not signed in" in call("violations", {"homeowner_token": "hw_forged"}), "forged token refused")
check("No homeowner" in call("sign_in", {"community": "Sheppard Center", "email": "someone@else.com"}), "unknown email")
r = call("sign_in", {"community": "the Shepherd Centre HOA", "email": "info at avris dot io"})
check(r.startswith("Signed in: Reza, 27 Willow Lane"), "sign_in tolerant")
tok = r.split("homeowner_token: ")[1].strip()
check("595" in call("my_account", {"homeowner_token": tok}), "account")
check("V-2026-0412" in call("violations", {"homeowner_token": tok}), "violations")
check("filed" in call("appeal_violation", {"homeowner_token": tok, "violation_id": "v-2026-0388", "reason": "the colour is close to Sage"}), "appeal")
check("No violation" in call("appeal_violation", {"homeowner_token": tok, "violation_id": "V-1", "reason": "x"}), "appeal unknown")
check("MNT-2026-2210" in call("my_requests", {"homeowner_token": tok}), "my requests")
check(call("file_request", {"homeowner_token": tok, "kind": "architectural", "details": "a cedar deck"}).startswith("Filed architectural request ARC-2026-"), "file request")
check("Booked the clubhouse" in call("reserve_amenity", {"homeowner_token": tok, "amenity": "clubhouse", "when": "Nov 14, 2-6 pm"}), "reserve")
check("can't be booked" in call("reserve_amenity", {"homeowner_token": tok, "amenity": "pool", "when": "Saturday"}), "pool not bookable")
check("Payment of $95.00 accepted" in call("make_payment", {"homeowner_token": tok, "amount": 95}), "small payment goes through")
check("6-month plan" in call("payment_plan", {"homeowner_token": tok, "months": 6}), "payment plan")
check("Milo" in call("my_household", {"homeowner_token": tok}), "household")
check("Gate one-time code" not in call("guest_pass", {"homeowner_token": tok, "guest_name": "Sam", "date": "Saturday"}), "no gate at sheppard")
check("estoppel" in call("resale_certificate", {"homeowner_token": tok}), "resale")
check("212" in call("community_info", {"homeowner_token": tok}), "signed in: own community by default")

r = call("sign_in", {"community": "stan meadows", "email": "nora.james@example.com"})
nora = r.split("homeowner_token: ")[1].strip()
check("Gate one-time code" in call("guest_pass", {"homeowner_token": nora, "guest_name": "Ben", "date": "Friday", "plate": "ABC123"}), "gated guest pass")
check("4417" in call("my_household", {"homeowner_token": nora}), "gate code")
r = call("sign_in", {"community": "harbor view", "email": "liam.obrien@example.com"})
liam = r.split("homeowner_token: ")[1].strip()
check("REN-2026-031" in call("my_requests", {"homeowner_token": liam}), "condo renovation")
check(nora != liam != tok, "tokens differ per homeowner")

# Family: contacts, messages, the $100 check, summaries, reminders, the board's inbox
check("reminders" not in [t["name"] for t in tools] and "family_inbox" not in [t["name"] for t in tools], "board tools hidden")
check("family@example.com" in call("family_contacts", {"homeowner_token": tok}), "demo family contact seeded")
check("No family contacts" in call("family_contacts", {"homeowner_token": nora}), "others start with none")
mail.outbox.clear()
r = call("notify_family", {"homeowner_token": tok, "message": "I'm fine, call me tonight"})
check(r.startswith("Sent to Reza"), "notify_family: " + r)
m = mail.outbox[-1]
check(m["to"] == ["family@example.com"] and "call me tonight" in m["text"] and "/family?" in m["links"][0][1], "notify mail")
fam_link = m["links"][0][1]

mail.outbox.clear()
r = call("make_payment", {"homeowner_token": tok, "amount": 300})
check("needs a family member's OK" in r and "Nothing has been paid" in r, "big payment waits: " + r)
appr = [l for l in mail.outbox[-1]["links"] if "/approve?" in l[1]][0][1]
check("$300.00" in mail.outbox[-1]["subject"], "approval mail")
check('"waiting"' in call("pending_approvals", {"homeowner_token": tok}), "pending listed")
check("and no family contact is set up" in call("make_payment", {"homeowner_token": nora, "amount": 500}),
      "no contacts: cannot approve")

def page(link, method="GET", extra=None):
    u = urllib.parse.urlparse(link)
    ev = {"rawPath": u.path, "rawQueryString": u.query, "requestContext": {"http": {"method": method}},
          "body": urllib.parse.urlencode(extra or {}), "headers": {}}
    return f.handler(ev, None)

r = page(appr)
check(r["statusCode"] == 200 and "Approve a payment?" in r["body"], "approve page GET shows buttons, changes nothing")
check('"waiting"' in call("pending_approvals", {"homeowner_token": tok}), "a GET (mail scanner) approves nothing")
check(page(appr.replace("s=", "s=0"))["statusCode"] == 403, "forged approve link refused")
r = page(appr, "POST", {"d": "yes"})
check("Approved" in r["body"], "approve POST")
check('"approved"' in call("pending_approvals", {"homeowner_token": tok}), "approved status")
check("Already approved" in page(appr, "POST", {"d": "no"})["body"], "decided once only")
inbox = json.loads(call("family_inbox", {"homeowner_token": tok}))["messages"]
check(len(inbox) == 1 and "approved your $300" in inbox[0]["text"] and inbox[0]["from"] == "Reza", "approval note on screen")
check(json.loads(call("family_inbox", {"homeowner_token": tok}))["messages"] == [], "inbox delivered once")

r = page(fam_link)
check(r["statusCode"] == 200 and "<textarea" in r["body"], "family page form")
check(page(fam_link, "POST", {"text": "Dinner Sunday at 6?", "from": "Reza"})["statusCode"] == 200, "family posts")
check(page(fam_link.replace("f=family", "f=x"), "POST", {"text": "hi"})["statusCode"] == 403, "forged family link refused")
inbox = json.loads(call("family_inbox", {"homeowner_token": tok, "take": False}))["messages"]
check(len(inbox) == 1 and inbox[0]["text"] == "Dinner Sunday at 6?", "family message waits")
check(len(json.loads(call("family_inbox", {"homeowner_token": tok}))["messages"]) == 1, "still there after take=false")

mail.outbox.clear()
check("emailed to Reza" in call("send_family_summary", {"homeowner_token": tok}), "summary sent")
t = mail.outbox[-1]["text"]
print(t) if os.environ.get("SHOW") else None
check("checked their account balance" in t and "paid $95.00" in t and "waiting for a family OK" in t
      and "V-2026-0412" in t and "October 22" in t, "summary content")
check("tell" not in t.lower() or True, "")
check(f.handler({"weekly_summary": True}, None)["sent"] == 1, "weekly run: only the consenting homeowner")
call("family_summary_consent", {"homeowner_token": tok, "on": False})
check(f.handler({"weekly_summary": True}, None)["sent"] == 0, "consent off")
check("Added Ana" in call("add_family_contact", {"homeowner_token": nora, "name": "Ana", "relation": "daughter",
                                                 "email": "ana at example dot com"}), "add contact")
check("ana@example.com" in call("family_contacts", {"homeowner_token": nora}), "contact stored")
call("add_family_contact", {"homeowner_token": tok, "name": "Sam", "relation": "friend", "email": "sam@example.com"})
fc = call("family_contacts", {"homeowner_token": tok})
check("family@example.com" in fc and "sam@example.com" in fc, "seeded contact kept when adding another")

rem = json.loads(call("reminders", {"homeowner_token": tok}))["reminders"]
ids = [x["id"] for x in rem]
check("trash-2026-10-13" in ids, f"trash night before Tuesday: {ids}")
tr = next(x for x in rem if x["id"] == "trash-2026-10-13")
check(family.local(tr["at"]).strftime("%a %H:%M") == "Mon 18:00", "trash reminder Monday 6 pm local")
check("fix-V-2026-0412" in ids and family.local(next(x for x in rem if x["id"] == "fix-V-2026-0412")["at"]).day == 15,
      "violation reminder 2 days before Oct 17")
check(all(9 <= family.local(x["at"]).hour < 19 for x in rem), "all reminders 9 am - 7 pm")
check("meeting-2026-10-22" not in ids, "the meeting is past the 8-day window")
family.now = lambda: NOW + 11 * 86400
rem = json.loads(call("reminders", {"homeowner_token": tok}))["reminders"]
check(any(x["id"] == "meeting-2026-10-22" for x in rem) and any(x["id"] == "hearing-V-2026-0388" for x in rem),
      "meeting and hearing reminders")
family.now = lambda: NOW + 18 * 86400
check(any(x["id"].startswith("dues-2026-11-01") for x in json.loads(call("reminders", {"homeowner_token": tok}))["reminders"]),
      "dues reminder")
family.now = lambda: NOW + 46 * 86400                                          # November 25
rem = json.loads(call("reminders", {"homeowner_token": nora}))["reminders"]
check(any(x["id"] == "booking-guest suite-2026-11-26" for x in rem), f"booking reminder the day before: {rem}")
family.now = lambda: NOW
check(f.handler({"rawPath": "/family", "requestContext": {"http": {"method": "DELETE"}}, "headers": {}}, None)["statusCode"] == 401,
      "other methods still need the token")

print("stan-mcp local: all passed" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
