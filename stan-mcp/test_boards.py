"""The boards' dashboard, without AWS. python3 stan-mcp/test_boards.py"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ["BOARDS_TOKEN"] = "t0k"
os.environ.pop("STATE_TABLE", None)
import boards
import lambda_function
import state

fails = 0


def check(c, what):
    global fails
    if not c:
        print("FAIL", what)
        fails += 1


def call(path, method="GET", body=None, token="t0k"):
    ev = {"rawPath": path, "requestContext": {"http": {"method": method, "sourceIp": "192.0.2.7"}},
          "headers": {"Authorization": "Bearer " + token} if token else {},
          "body": json.dumps(body) if body is not None else ""}
    return lambda_function.handler(ev, None)


state.reset()
check(call("/boards", token=None)["statusCode"] == 200, "the page needs no token")
check("<title>Gadgets</title>" in call("/mcp/boards", token=None)["body"], "the page from the /mcp address")
check(call("/boards/data", token=None)["statusCode"] == 401, "data wants the token")
check(call("/boards/data", token="nope")["statusCode"] == 401, "data wants the right token")
check(call("/boards/report", "POST", {"board": "x"}, token="nope")["statusCode"] == 401, "reports want the token")

rep = {"board": "tiny1", "mac": "68:ee:8f:00:00:01", "every": 60,
       "sys": {"uptime_s": 10, "heap_kb": 200.5, "ssid": "home"},
       "stats": {"temp_c": 22.5, "bad key!": 1, "nan_c": float("nan"), "ok": True}}
r = call("/boards/report", "POST", rep)
check(r["statusCode"] == 200 and json.loads(r["body"])["ok"], "a report is taken")
check(call("/boards/report", "POST", {"board": "Bad Name"})["statusCode"] == 400, "a bad name is refused")
r = lambda_function.handler({"rawPath": "/boards/report", "requestContext": {"http": {"method": "POST"}},
                             "headers": {"authorization": "Bearer t0k"}, "body": "{nope"}, None)
check(r["statusCode"] == 400, "not JSON is refused")

d = json.loads(call("/boards/data")["body"])
b = d["boards"][0]
check(b["board"] == "tiny1" and b["ip"] == "192.0.2.7", "the board and where it reported from")
check(b["stats"] == {"temp_c": 22.5, "ok": "yes"}, f"bad keys and NaN dropped: {b['stats']}")
check(b["sys"]["ssid"] == "home" and "sys.ssid" not in b["hist"], "strings shown, not charted")

# History: a point a minute at most, 24 hours kept.
t = 1_000_000
for i in range(5):
    boards.record({"board": "speaker", "stats": {"laf_dba": 40 + i}}, now=t + i * 20)   # 20 s apart
h = boards.all_boards()
sp = [x for x in h if x["board"] == "speaker"][0]
check(len(sp["hist"]["stats.laf_dba"]) == 2, f"close reports share a point: {sp['hist']['stats.laf_dba']}")
check(sp["hist"]["stats.laf_dba"][-1][1] == 44, "the latest value wins")
check(sp["reports"] == 5 and sp["first_seen"] == t, "reports counted, first seen kept")
boards.record({"board": "speaker", "stats": {"laf_dba": 50}}, now=t + 25 * 3600)
sp = [x for x in boards.all_boards() if x["board"] == "speaker"][0]
check(sp["hist"]["stats.laf_dba"] == [[t + 25 * 3600, 50]], "a day old is dropped")

check(len(json.loads(call("/boards/data")["body"])["boards"]) == 2, "two boards, no per-board code")
check(call("/boards/forget", "POST", {"board": "speaker"})["statusCode"] == 200, "forget a board")
check(len(boards.all_boards()) == 1, "and it is gone")

os.environ["BOARDS_TOKEN"] = ""
check(call("/boards/data", token="")["statusCode"] == 401, "no token configured: nothing is open")

print("boards: all passed" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
