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
    if not c: print("FAIL", what); fails += 1

check(post({"jsonrpc": "2.0", "id": 1, "method": "ping"}, token="wrong")["statusCode"] == 401, "wrong token refused")
r = post({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-06-18"}})
check(r["statusCode"] == 200 and "Mcp-Session-Id" in r["headers"] and json.loads(r["body"])["result"]["serverInfo"]["name"] == "stan-mcp", "initialize")
check(post({"jsonrpc": "2.0", "method": "notifications/initialized"})["statusCode"] == 202, "notification 202")
names = [t["name"] for t in json.loads(post({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})["body"])["result"]["tools"]]
check(names == ["sign_in", "lookup_rule", "my_account", "violations", "file_request", "next_board_meeting", "contact_management"], "tools/list")
def call(name, args):
    return json.loads(post({"jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": {"name": name, "arguments": args}})["body"])
check("6 feet" in call("lookup_rule", {"topic": "Can I build a fence?"})["result"]["content"][0]["text"], "fence rule")
check("Section 7.9" in call("lookup_rule", {"topic": "solar panels"})["result"]["content"][0]["text"], "solar rule")
check("Section 11.4" in call("lookup_rule", {"topic": "my dog barks a lot"})["result"]["content"][0]["text"], "dog -> pets")
check("Section 12.3" in call("lookup_rule", {"topic": "can I airbnb my house"})["result"]["content"][0]["text"], "airbnb")
check("No rule" in call("lookup_rule", {"topic": "a b c"})["result"]["content"][0]["text"], "nothing matches short words")
text = lambda r: r["result"]["content"][0]["text"]
check("Not signed in" in text(call("my_account", {})), "account needs sign-in")
check("Not signed in" in text(call("violations", {"homeowner_token": "hw_forged"})), "forged token refused")
check("don't know a community" in text(call("sign_in", {"community": "Stan Meadows", "email": "info@avriz.io"})), "unknown community")
check("No homeowner" in text(call("sign_in", {"community": "Sheppard Center", "email": "someone@else.com"})), "unknown email")
r = text(call("sign_in", {"community": "the Shepherd Centre HOA", "email": "info at avris dot io"}))   # as speech might hear it
check(r.startswith("Signed in: info@avriz.io") and "homeowner_token: hw_" in r, "sign_in tolerant")
tok = r.split("homeowner_token: ")[1].strip()
check("595" in text(call("my_account", {"homeowner_token": tok})), "account")
check("V-2026-0412" in text(call("violations", {"homeowner_token": tok})), "violations")
t = text(call("file_request", {"homeowner_token": tok, "kind": "architectural", "details": "a 12 by 16 foot cedar deck"}))
check(t.startswith("Filed architectural request ARC-2026-"), "file_request")
check("6 feet" in text(call("lookup_rule", {"topic": "fence"})), "rules stay public")
check(call("fly", {}).get("error", {}).get("code") == -32602, "unknown tool")
print("stan-mcp local: all passed" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
