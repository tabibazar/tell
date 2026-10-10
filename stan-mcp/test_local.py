"""Exercise stan-mcp's handler the way a function URL calls it: python3 stan-mcp/test_local.py"""
import json, os, sys
sys.path.insert(0, os.path.dirname(__file__))
os.environ["MCP_TOKEN"] = "test-token"
import lambda_function as f

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
check(len(tools) == 23 and all(t["inputSchema"]["type"] == "object" for t in tools), f"tools/list ({len(tools)})")
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
check("Payment of $295.00 accepted" in call("make_payment", {"homeowner_token": tok, "amount": 295}), "payment")
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

print("stan-mcp local: all passed" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
