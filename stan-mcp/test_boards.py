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
    path, _, query = path.partition("?")
    ev = {"rawPath": path, "rawQueryString": query, "requestContext": {"http": {"method": method, "sourceIp": "192.0.2.7"}},
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
check(b["sys"]["ssid"] == "home" and "hist" not in b, "strings shown; history is not in the latest")

# History: by the hour, a point a minute at most.
import time
t = (int(time.time()) // 3600 - 5) * 3600      # an hour's start, five hours ago
for i in range(5):
    boards.record({"board": "speaker", "stats": {"laf_dba": 40 + i}}, now=t + i * 20)   # 20 s apart
h = boards.history("speaker", 24, now=t + 100)["series"]
check(len(h["stats.laf_dba"]) == 2, f"close reports share a point: {h['stats.laf_dba']}")
check(h["stats.laf_dba"][-1][1] == 44, "the latest value wins")
sp = [x for x in boards.all_boards() if x["board"] == "speaker"][0]
check(sp["reports"] == 5 and sp["first_seen"] == t, "reports counted, first seen kept")
for m in range(1, 180):                          # three hours, a report a minute
    boards.record({"board": "speaker", "stats": {"laf_dba": m}}, now=t + 100 + m * 60)
h = boards.history("speaker", 2, now=t + 100 + 179 * 60)["series"]["stats.laf_dba"]
check(len(h) == 120 and h[-1][1] == 179, f"the last two hours across hour buckets: {len(h)}")
r = json.loads(call("/boards/history?board=speaker&hours=24")["body"])
check(r["step"] == 60 and "stats.laf_dba" in r["series"], "history over HTTP")
w = boards.history("speaker", 72, now=t + 100 + 179 * 60)
check(w["step"] == 300 and len(w["series"]["stats.laf_dba"]) <= 40, "beyond two days: five-minute means")
check(call("/boards/history?board=Bad!")["statusCode"] == 400, "a bad board name for history")

# The old way's history (inside the latest report) moves into the buckets.
state.put("boards", "b#old", {"board": "old", "hist": {"stats.x_c": [[t, 1.5], [t + 60, 2.5]]}, "reports": 3})
boards.record({"board": "old", "stats": {"x_c": 3.5}}, now=t + 120)
h = boards.history("old", 24, now=t + 120)["series"]["stats.x_c"]
check([v for _, v in h] == [1.5, 2.5, 3.5], f"old history kept: {h}")
check("hist" not in [x for x in boards.all_boards() if x["board"] == "old"][0], "and gone from the latest")

# The seasons: each finished hour summed up into its day, a year kept.
y0 = (t // 86400 + 1) * 86400                    # a day's start, after the minutes above
for hh in range(0, 50):                          # two days and a bit, a report every 20 min
    for m in (0, 20, 40):
        boards.record({"board": "season", "stats": {"temp_c": 10 + hh % 24, "co2_ppm": 400 + m}}, now=y0 + hh * 3600 + m * 60)
L = boards.history("season", 24 * 30, now=y0 + 50 * 3600)
check(L["step"] == 3600, f"a month: hourly summaries ({L['step']})")
pts = L["series"]["stats.co2_ppm"]
check(len(pts) == 49 and pts[0][1:] == [420.0, 400, 440], f"hours summed (mean, min, max): {len(pts)} {pts[0]}")
Y = boards.history("season", 24 * 365, now=y0 + 50 * 3600)
check(Y["step"] == 86400 and len(Y["series"]["stats.temp_c"]) == 3, f"a year: by the day: {Y['series'].get('stats.temp_c')}")
check(Y["series"]["stats.temp_c"][0][2:] == [10, 33], "a day's min and max")
check(state.get("boards", "b#season")["rolled"] == (y0 + 49 * 3600) // 3600 - 1, "rolled up to the hour before the last report's")

check(len(json.loads(call("/boards/data")["body"])["boards"]) == 4, "boards with no per-board code")
check(call("/boards/forget", "POST", {"board": "speaker"})["statusCode"] == 200, "forget a board")
check(not boards.history("speaker", 24, now=t + 100)["series"], "and its history")

os.environ["BOARDS_TOKEN"] = ""
check(call("/boards/data", token="")["statusCode"] == 401, "no token configured: nothing is open")

print("boards: all passed" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
