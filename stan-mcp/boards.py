"""The boards' dashboard: every board that reports over WiFi, on one page,
at https://gadgets.avriz.io (CloudFront in front of this Lambda; / is /boards).

A board POSTs to /boards/report, now and then (a minute apart, say):

    Authorization: Bearer <BOARDS_TOKEN>
    {"board": "tiny1", "mac": "68:ee:..", "every": 60,
     "sys":   {"uptime_s": 812, "heap_kb": 211.4, "rssi_dbm": -61, "network": "..", ...},
     "stats": {"temp_c": 23.1, "pressure_hpa": 1012.6, "photos": 14, ...}}

Nothing here knows any board: a new one shows up the first time it reports,
and its numbers are drawn by their names. A key's last part is its unit
(_c, _pct, _hpa, _dba, _ppm, _ppb, _kb, _dbm, _s, _mhz, ...); a number gets
its history charted and summed up, a string is shown as it is.

  pk "boards"  sk "b#<board>"          the latest report, when it came and from where
               sk "h#<board>#<hour>"   that hour's numbers, a point a minute at most,
                                       as {"t0": hour start, "s": {"stats.temp_c":
                                       [[seconds into the hour, value], ...]}};
                                       kept HIST_DAYS days
               sk "d#<board>#<day>"    that day's hours summed up, once each hour is
                                       over: {"t0": day start, "s": {"stats.temp_c":
                                       [[hour 0-23, mean, min, max, n], ...]}};
                                       kept ROLL_DAYS days -- the seasons

History is kept by the hour, and a year by the day, so no item grows past
DynamoDB's 400 KB (a day of a busy board's minutes in one item would), and a
report writes the small items it touches: its minute into the hour's item,
and once an hour the hour before summed into the day's.

The page holds no data. It asks for BOARDS_TOKEN once, keeps it in the
browser, and fetches /boards/data (the latest reports) and /boards/history
(a board's numbers over the last N hours) with it -- the same token the
boards report with (an environment variable; never in git).
"""
import hmac
import json
import os
import re
import time
from urllib.parse import parse_qsl

import state

HIST_DAYS = 8           # minute points live this long (the page shows them up to 7 days)
ROLL_DAYS = 400         # hourly summaries live this long (the page shows up to a year)
MAX_HOURS = 24 * 366
HIST_GAP_S = 55         # at most a point a minute
MAX_KEYS = 48           # per section, per report
MAX_STR = 80
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,23}$")
KEY_RE = re.compile(r"^[a-z0-9_]{1,32}$")


def _token_ok(headers):
    token = os.environ.get("BOARDS_TOKEN", "")
    return bool(token) and hmac.compare_digest(headers.get("authorization", ""), "Bearer " + token)


def _json(status, d):
    return {"statusCode": status, "headers": {"Content-Type": "application/json", "Cache-Control": "no-store"},
            "body": json.dumps(d, separators=(",", ":"))}


def _clean(section):
    """A section of a report, kept to short keys and plain values."""
    out = {}
    if not isinstance(section, dict):
        return out
    for k, v in list(section.items())[:MAX_KEYS]:
        if not isinstance(k, str) or not KEY_RE.match(k):
            continue
        if isinstance(v, bool):
            out[k] = "yes" if v else "no"
        elif isinstance(v, (int, float)) and v == v and abs(v) < 1e12:   # not NaN, not absurd
            out[k] = round(float(v), 3) if isinstance(v, float) else v
        elif isinstance(v, str):
            out[k] = v[:MAX_STR]
    return out


def _hour_sk(name, hour):
    return "h#%s#%08d" % (name, hour)


def _add_points(name, points):
    """points: [(t, series, value)], into their hours' buckets."""
    by_hour = {}
    for t, k, v in points:
        by_hour.setdefault(t // 3600, []).append((t, k, v))
    for hour, pts in by_hour.items():
        sk = _hour_sk(name, hour)
        b = state.get("boards", sk) or {"t0": hour * 3600, "s": {}}
        for t, k, v in pts:
            off = t - hour * 3600
            ser = b["s"].setdefault(k, [])
            if ser and off - ser[-1][0] < HIST_GAP_S:
                ser[-1] = [ser[-1][0], v]              # too soon after the last point: replace it
            else:
                ser.append([off, v])
        state.put("boards", sk, b, keep_days=HIST_DAYS)


def _day_sk(name, day):
    return "d#%s#%06d" % (name, day)


def _roll_up(name, upto_hour, since_hour):
    """Sum up each finished hour in (since_hour, upto_hour) into its day's
    item. Returns the last hour looked at."""
    lo, hi = since_hour + 1, upto_hour - 1
    if hi < lo:
        return since_hour
    days = {}
    for sk, b in state.query_range("boards", _hour_sk(name, lo), _hour_sk(name, hi)):
        hour = b["t0"] // 3600
        days.setdefault(hour // 24, []).append((hour, b))
    for day, hours in days.items():
        sk = _day_sk(name, day)
        d = state.get("boards", sk) or {"t0": day * 86400, "s": {}}
        for hour, b in hours:
            for k, ser in b.get("s", {}).items():
                vs = [v for _, v in ser]
                if not vs:
                    continue
                row = [hour % 24, round(sum(vs) / len(vs), 3), min(vs), max(vs), len(vs)]
                rows = [r for r in d["s"].get(k, []) if r[0] != row[0]] + [row]
                d["s"][k] = sorted(rows)
        state.put("boards", sk, d, keep_days=ROLL_DAYS)
    return hi


def record(report, ip="", now=None):
    """Keep one report. Returns (ok, why)."""
    now = int(now if now is not None else time.time())
    if not isinstance(report, dict):
        return False, "not an object"
    name = str(report.get("board", "")).lower()
    if not NAME_RE.match(name):
        return False, "bad board name"
    old = state.get("boards", "b#" + name) or {}
    sys_, stats = _clean(report.get("sys")), _clean(report.get("stats"))
    points = [(t, k, v) for k, pts in old.get("hist", {}).items() for t, v in pts]   # history kept the old way
    points += [(now, sect + "." + k, v) for sect, vals in (("sys", sys_), ("stats", stats))
               for k, v in vals.items() if not isinstance(v, str)]
    _add_points(name, points)
    hour = now // 3600
    rolled = old.get("rolled", hour - HIST_DAYS * 24)
    if rolled < hour - 1:                          # a new hour: sum up the ones before it
        rolled = _roll_up(name, hour, rolled)
    every = report.get("every")
    d = {
        "board": name,
        "mac": str(report.get("mac", ""))[:17],
        "fw": str(report.get("fw", ""))[:40],
        "every": int(every) if isinstance(every, (int, float)) and 5 <= every <= 86400 else 60,
        "first_seen": old.get("first_seen", now),
        "last_seen": now,
        "reports": old.get("reports", 0) + 1,
        "ip": ip[:45],
        "sys": sys_,
        "stats": stats,
        "rolled": rolled,
    }
    state.put("boards", "b#" + name, d)
    return True, ""


def all_boards():
    return [d for _, d in state.query("boards", "b#")]


def _long_history(name, hours, now):
    """Beyond a week, from the days' summaries: [t, mean, min, max] an hour
    apart up to a month, a day apart beyond (a year is 366 points a series)."""
    cut = now - hours * 3600
    by_day = hours > 31 * 24
    out = {}
    for _, d in state.query_range("boards", _day_sk(name, cut // 86400), _day_sk(name, now // 86400)):
        for k, rows in d.get("s", {}).items():
            rows = [r for r in rows if d["t0"] + r[0] * 3600 + 3600 > cut]
            if not rows:
                continue
            if by_day:
                n = sum(r[4] for r in rows)
                mean = sum(r[1] * r[4] for r in rows) / n
                out.setdefault(k, []).append([d["t0"] + 43200, round(mean, 3),
                                              min(r[2] for r in rows), max(r[3] for r in rows)])
            else:
                out.setdefault(k, []).extend([d["t0"] + r[0] * 3600 + 1800, r[1], r[2], r[3]] for r in rows)
    return {"board": name, "hours": hours, "step": 86400 if by_day else 3600, "series": out}


def history(name, hours, now=None):
    """{series: [[t, value], ...]} for the last `hours` hours: a point a minute
    up to two days, five-minute means up to a week (~2000 points a series),
    and beyond that the summaries, [t, mean, min, max]."""
    now = int(now if now is not None else time.time())
    hours = max(1, min(MAX_HOURS, int(hours)))
    if hours > HIST_DAYS * 24 - 24:
        return _long_history(name, hours, now)
    cut = now - hours * 3600
    step = 60 if hours <= 48 else 300
    out = {}
    for _, b in state.query_range("boards", _hour_sk(name, cut // 3600), _hour_sk(name, now // 3600)):
        for k, ser in b.get("s", {}).items():
            out.setdefault(k, []).extend([b["t0"] + off, v] for off, v in ser if b["t0"] + off > cut)
    if step > 60:
        for k, pts in out.items():
            slots = {}
            for t, v in pts:
                slots.setdefault(t // step, []).append(v)
            out[k] = [[s * step + step // 2, round(sum(vs) / len(vs), 3)] for s, vs in sorted(slots.items())]
    return {"board": name, "hours": hours, "step": step, "series": out}


def forget(name):
    state.delete("boards", "b#" + name)
    for prefix in ("h#%s#" % name, "d#%s#" % name):
        for sk, _ in state.query("boards", prefix):
            state.delete("boards", sk)


def handle(path, method, headers, body, ip="", query=""):
    if path in ("/boards", "/boards/") and method == "GET":
        return {"statusCode": 200, "headers": {"Content-Type": "text/html; charset=utf-8"}, "body": PAGE}
    if not _token_ok(headers):
        return {"statusCode": 401, "headers": {"WWW-Authenticate": "Bearer"}, "body": ""}
    q = dict(parse_qsl(query or ""))
    if path == "/boards/report" and method == "POST":
        try:
            report = json.loads(body or "")
        except ValueError:
            return _json(400, {"ok": False, "why": "not JSON"})
        ok, why = record(report, ip)
        return _json(200 if ok else 400, {"ok": ok, "why": why, "now": int(time.time())})
    if path == "/boards/data" and method == "GET":
        return _json(200, {"now": int(time.time()), "boards": all_boards()})
    if path == "/boards/history" and method == "GET":
        name = q.get("board", "")
        if not NAME_RE.match(name):
            return _json(400, {"ok": False})
        try:
            hours = int(q.get("hours", "24"))
        except ValueError:
            hours = 24
        return _json(200, history(name, hours))
    if path == "/boards/forget" and method == "POST":
        try:
            name = json.loads(body or "{}").get("board", "")
        except ValueError:
            name = ""
        if not NAME_RE.match(str(name)):
            return _json(400, {"ok": False})
        forget(name)
        return _json(200, {"ok": True})
    return {"statusCode": 404, "body": ""}


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Gadgets</title>
<link rel="preconnect" href="https://fonts.googleapis.com"><link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Instrument+Serif:ital@0;1&family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;500&display=swap" rel="stylesheet">
<style>
:root{--paper:#f3f0e8;--ink:#1d1c1a;--mute:#7a756b;--rule:#d8d2c4;--fill:rgba(29,28,26,.07);--band:rgba(29,28,26,.06);--sig:#c8431a;--ok:#2e6a4c;
 --s0:#1d1c1a;--s1:#c8431a;--s2:#2e6a4c;--s3:#3d5a80;--s4:#b07d18;--s5:#7b4b94}
@media (prefers-color-scheme:dark){:root{--paper:#131312;--ink:#ebe6da;--mute:#8b8579;--rule:#2c2a26;--fill:rgba(235,230,218,.07);--band:rgba(235,230,218,.07);--sig:#ff7d4f;--ok:#8fc6a2;
 --s0:#ebe6da;--s1:#ff7d4f;--s2:#8fc6a2;--s3:#8fb3e0;--s4:#e3b55a;--s5:#c49be0}}
*{box-sizing:border-box;margin:0}
body{background:var(--paper);color:var(--ink);font:14px/1.45 "IBM Plex Sans",system-ui,sans-serif;-webkit-font-smoothing:antialiased}
.wrap{max-width:1720px;margin:0 auto;padding:0 20px}
header{display:flex;justify-content:space-between;align-items:flex-end;padding:34px 0 16px;gap:16px;flex-wrap:wrap}
.mark{font:400 52px/0.9 "Instrument Serif",serif;letter-spacing:-.01em}
.head-r{display:flex;align-items:flex-end;gap:26px;flex-wrap:wrap}
.clock{text-align:right;color:var(--mute);font:12px/1.6 "IBM Plex Mono",monospace}
.clock b{color:var(--ink);font-weight:500}
.ranges{display:flex;border:1px solid var(--ink)}
.ranges button{all:unset;cursor:pointer;font:500 12px "IBM Plex Mono",monospace;padding:6px 11px;border-right:1px solid var(--ink)}
.ranges button:last-child{border-right:0}
.ranges button.on{background:var(--ink);color:var(--paper)}
h4{font:400 30px/1 "Instrument Serif",serif;margin:0 0 4px}
.sub{color:var(--mute);font:11.5px "IBM Plex Mono",monospace}
#compare{border-top:2px solid var(--ink);padding:16px 0 30px;display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,520px),1fr));gap:22px 44px}
#compare:empty{display:none}
#compare .cmp-h{grid-column:1/-1;display:flex;justify-content:space-between;align-items:baseline;gap:12px;flex-wrap:wrap}
.cmp .k{margin-bottom:2px}
.legend{display:flex;flex-wrap:wrap;gap:4px 16px;font:12px "IBM Plex Mono",monospace;margin:4px 0 6px}
.legend i{display:inline-block;width:14px;height:2px;vertical-align:middle;margin-right:6px}
#boards{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,540px),1fr));gap:0 44px;align-items:start}
.board{border-top:2px solid var(--ink);display:flex;flex-direction:column;gap:16px;padding:16px 0 36px;min-width:0}
.who{display:flex;justify-content:space-between;align-items:flex-start;gap:8px 20px;flex-wrap:wrap}
.who h2{font:400 46px/1 "Instrument Serif",serif;margin:2px 0 10px;word-break:break-word}
.who .meta{text-align:right}
.state{font:500 12px "IBM Plex Mono",monospace}
.state.on{color:var(--ok)} .state.off{color:var(--sig)}
.meta{color:var(--mute);font:11.5px/1.75 "IBM Plex Mono",monospace;word-break:break-all}
.meta span{color:var(--ink)}
.grid{display:grid;grid-auto-flow:dense;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));border-left:1px solid var(--rule);border-top:1px solid var(--rule)}
.grid.small{grid-template-columns:repeat(auto-fill,minmax(122px,1fr))}
.cell{border-right:1px solid var(--rule);border-bottom:1px solid var(--rule);padding:11px 13px 9px;cursor:zoom-in;min-width:0;position:relative}
.cell:hover{background:var(--fill)}
.cell.open{grid-column:1/-1;cursor:default;padding:14px 16px 16px}
.cell.open:hover{background:none}
.k{font:500 10.5px "IBM Plex Sans",sans-serif;letter-spacing:.09em;text-transform:uppercase;color:var(--mute);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.v{font:400 27px/1.15 "IBM Plex Mono",monospace;font-variant-numeric:tabular-nums;margin-top:3px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.small .v{font-size:20px}
.v small{font-size:12px;color:var(--mute);margin-left:4px}
.d{font:10.5px "IBM Plex Mono",monospace;color:var(--mute);margin-top:3px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.d b{font-weight:500;color:var(--ink)}
.up{color:var(--sig)} .dn{color:var(--s3)}
.spark{display:block;width:100%;height:34px;margin-top:6px}
.small .spark{height:24px}
.chart{position:relative;margin-top:8px}
.chart svg{display:block;width:100%}
.chart .lab{position:absolute;font:10px "IBM Plex Mono",monospace;color:var(--mute);white-space:nowrap;transform:translateX(-50%)}
.chart .ylab{position:absolute;right:0;font:10px "IBM Plex Mono",monospace;color:var(--mute);transform:translateY(-110%);background:var(--paper);padding-left:4px}
.chart .dot{position:absolute;width:7px;height:7px;border-radius:50%;transform:translate(-50%,-50%);background:var(--sig)}
.chart .ann{position:absolute;font:500 10.5px "IBM Plex Mono",monospace;white-space:nowrap;transform:translate(-50%,-140%)}
.chart .ann.lo{transform:translate(-50%,40%)}
.detail{display:grid;grid-template-columns:minmax(0,1fr) 250px;gap:18px 26px;margin-top:6px}
.stats{font:12px/1.95 "IBM Plex Mono",monospace;border-top:1px solid var(--rule)}
.stats div{display:flex;justify-content:space-between;border-bottom:1px solid var(--rule)}
.stats span{color:var(--mute)}
.panels{grid-column:1/-1;display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:18px 26px}
.panel .k{margin-bottom:6px}
.bars{display:flex;align-items:flex-end;gap:2px;height:70px}
.bars div{flex:1;background:var(--ink);opacity:.75;min-height:1px;position:relative}
.bars div.med{background:var(--sig);opacity:1}
.bars div.none{background:var(--rule);opacity:1}
.axis{display:flex;justify-content:space-between;font:10px "IBM Plex Mono",monospace;color:var(--mute);margin-top:3px}
.cal{display:grid;grid-auto-flow:column;grid-template-rows:repeat(7,11px);gap:2px;overflow-x:auto;padding-bottom:2px}
.cal div{width:11px;height:11px;background:var(--rule)}
.close{position:absolute;top:10px;right:12px;font:12px "IBM Plex Mono",monospace;color:var(--mute);cursor:pointer;background:none;border:0}
.facts{display:flex;flex-wrap:wrap;gap:10px 30px}
.facts .f{font:400 16px "IBM Plex Sans",sans-serif;margin-top:1px}
.sys{color:var(--mute);font:11.5px/1.7 "IBM Plex Mono",monospace;margin-top:-4px;cursor:pointer}
.sys i{font-style:normal;color:var(--rule);padding:0 7px}
.sys u{text-decoration:none;color:var(--ink)}
.stale .v,.stale .spark,.stale .f{opacity:.38}
footer{color:var(--mute);font:11px "IBM Plex Mono",monospace;padding:4px 0 40px;display:flex;justify-content:space-between;gap:12px;flex-wrap:wrap}
footer button{all:unset;cursor:pointer;text-decoration:underline;text-underline-offset:3px}
#gate{max-width:400px;padding-top:18vh}
#gate p{color:var(--mute);margin:14px 0 26px}
#gate input{width:100%;background:none;border:0;border-bottom:1.5px solid var(--ink);color:var(--ink);font:16px "IBM Plex Mono",monospace;padding:8px 0;outline:none}
#gate button{margin-top:22px;background:var(--ink);color:var(--paper);border:0;padding:10px 18px;font:500 13px "IBM Plex Sans",sans-serif;cursor:pointer}
.none{color:var(--mute);padding:30px 0;border-top:2px solid var(--ink);grid-column:1/-1}
@media (max-width:760px){.who .meta{text-align:left}.who h2{font-size:40px}.mark{font-size:44px}.detail{grid-template-columns:1fr}.clock{text-align:left}}
</style></head><body>
<div class="wrap" id="gate" hidden><div class="mark">Gadgets</div>
<p>Enter the boards' token. This browser keeps it.</p>
<input id="tok" type="password" autocomplete="current-password" spellcheck="false"><br><button id="go">Open</button></div>
<div class="wrap" id="app" hidden>
<header><div class="mark">Gadgets</div><div class="head-r"><div class="ranges" id="ranges"></div><div class="clock" id="head"></div></div></header>
<section id="compare"></section>
<div id="boards"></div>
<footer><span id="upd"></span><button id="out">forget token</button></footer></div>
<script>
"use strict";
const U={c:"°C",pct:"%",hpa:"hPa",dba:"dB(A)",ppm:"ppm",ppb:"ppb",kb:"KB",mb:"MB",dbm:"dBm",deg:"°",min:"min",s:"",g:"g",dps:"°/s",mv:"mV",v:"V",ms:"ms",hz:"Hz",mhz:"MHz",lux:"lx"};
const W={laeq:"LAeq",l90:"L90",tvoc:"TVOC",eco2:"eCO2",co2:"CO2",rssi:"RSSI",pm25:"PM2.5",imu:"IMU",rtc:"RTC",aqi:"AQI",cpu:"CPU",tx:"TX",ch:"channel",wifi:"WiFi",big:"largest block",heap:"heap",psram:"PSRAM"};
const RANGES=[["1h",1],["6h",6],["24h",24],["7d",168],["30d",720],["1y",8760]];
const DAYS=["Sun","Mon","Tue","Wed","Thu","Fri","Sat"],MONTHS=["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];
const $=id=>document.getElementById(id);
const store={get(k){try{return localStorage.getItem(k)}catch(e){return null}},set(k,v){try{v==null?localStorage.removeItem(k):localStorage.setItem(k,v)}catch(e){}}};
let TOKEN=store.get("gadgets_token"),DATA=null,HOURS=+(store.get("gadgets_hours")||24),SYSOPEN=new Set(JSON.parse(store.get("gadgets_sys")||"[]"));
const OPEN=new Set(JSON.parse(store.get("gadgets_open")||"[]"));
const HIST={};          // board -> {hours, got, step, series}
const esc=s=>String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
function unit(k){const i=k.lastIndexOf("_"),u=i>0?k.slice(i+1):"";return u in U?[k.slice(0,i),u]:[k,""]}
function words(k){return k.split("_").map(w=>W[w]||(w==="1s"?"1 s":w)).join(" ")}
function dur(s){s=Math.max(0,Math.round(s));if(s<60)return s+" s";if(s<3600)return Math.floor(s/60)+" min";
 if(s<86400){const h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h+" h"+(m?" "+m+" min":"")}const d=Math.floor(s/86400),h=Math.floor(s%86400/3600);return d+" d"+(h?" "+h+" h":"")}
function num(v){if(v==null||!isFinite(v))return "–";const a=Math.abs(v);let t=a>=1000?v.toFixed(0):a>=100?v.toFixed(1):a>=10?v.toFixed(1):v.toFixed(2);t=t.replace(/(\.\d*?)0+$/,"$1").replace(/\.$/,"");return t.replace("-","−")}
function fmt(v,u){return u==="s"?dur(v):num(v)+(U[u]?" "+U[u]:"")}
const D=t=>new Date(t*1000);
function when(t){const d=D(t),now=new Date(),same=d.toDateString()===now.toDateString();
 const hm=d.toLocaleTimeString([], {hour:"2-digit",minute:"2-digit"});return same?hm:HOURS>24*40?d.toLocaleDateString([], {day:"numeric",month:"short",year:"2-digit"}):DAYS[d.getDay()]+" "+d.getDate()+" "+hm}
function tick(t,span){const d=D(t);if(span<=2*86400)return d.toLocaleTimeString([], {hour:"2-digit",minute:"2-digit"});if(span<=40*86400)return DAYS[d.getDay()]+" "+d.getDate();return MONTHS[d.getMonth()]+(d.getMonth()===0?" "+d.getFullYear():"")}

/* ---- statistics ---- */
function q(sorted,p){if(!sorted.length)return NaN;const i=(sorted.length-1)*p,lo=Math.floor(i),hi=Math.ceil(i);return sorted[lo]+(sorted[hi]-sorted[lo])*(i-lo)}
function stats(pts){if(!pts||!pts.length)return null;const vs=pts.map(p=>p[1]),s=[...vs].sort((a,b)=>a-b),n=vs.length;
 const mean=vs.reduce((a,b)=>a+b,0)/n,sd=Math.sqrt(vs.reduce((a,b)=>a+(b-mean)**2,0)/n);
 let lo=pts[0],hi=pts[0];for(const p of pts){const pl=p.length>2?p[2]:p[1],ph=p.length>2?p[3]:p[1];if(pl<(lo.length>2?lo[2]:lo[1]))lo=p;if(ph>(hi.length>2?hi[3]:hi[1]))hi=p}
 const t0=pts[0][0],tm=pts.reduce((a,p)=>a+p[0],0)/n;let sxy=0,sxx=0;for(const p of pts){sxy+=(p[0]-tm)*(p[1]-mean);sxx+=(p[0]-tm)**2}
 return {n,mean,sd,med:q(s,.5),p10:q(s,.1),p90:q(s,.9),min:lo.length>2?lo[2]:lo[1],tmin:lo[0],max:hi.length>2?hi[3]:hi[1],tmax:hi[0],
  slope:sxx?sxy/sxx*3600:0,first:pts[0][1],last:pts[n-1][1],t0,t1:pts[n-1][0]}}
function thin(pts,n){if(pts.length<=n)return pts;const x0=pts[0][0],span=(pts[pts.length-1][0]-x0)||1,b=[];
 for(const p of pts){const i=Math.min(n-1,Math.floor((p[0]-x0)/span*n));(b[i]=b[i]||[]).push(p)}
 return b.filter(Boolean).map(g=>[g.reduce((a,p)=>a+p[0],0)/g.length,g.reduce((a,p)=>a+p[1],0)/g.length,Math.min(...g.map(p=>p.length>2?p[2]:p[1])),Math.max(...g.map(p=>p.length>2?p[3]:p[1]))])}
function nice(lo,hi,n){const span=hi-lo||Math.abs(hi)||1,step0=span/n,mag=10**Math.floor(Math.log10(step0)),f=step0/mag,step=(f<1.5?1:f<3?2:f<7?5:10)*mag;
 const out=[];for(let v=Math.ceil(lo/step)*step;v<=hi+step*1e-9;v+=step)out.push(+v.toFixed(10));return out}

/* ---- charts: SVG for the lines (stretched), HTML for every label (crisp) ---- */
function path(pts,X,Y,gap){let d="";pts.forEach((p,i)=>{const brk=i&&p[0]-pts[i-1][0]>gap;d+=(i&&!brk?"L":"M")+X(p[0]).toFixed(1)+" "+Y(p[1]).toFixed(1)});return d}
function spark(pts,st){if(!pts||pts.length<2)return "";const t=thin(pts,60),x0=t[0][0],x1=t[t.length-1][0];
 let y0=Math.min(...t.map(p=>p[1])),y1=Math.max(...t.map(p=>p[1]));if(y1-y0<1e-9){y0-=1;y1+=1}
 const X=x=>(x-x0)/(x1-x0||1)*200,Y=v=>32-(v-y0)/(y1-y0)*30,d=path(t,X,Y,(x1-x0)/8);
 const area=d.split("M").filter(Boolean).map(seg=>{const c=seg.split("L"),f=c[0].split(" ")[0],l=c[c.length-1].split(" ")[0];return "M"+seg+"L"+l+" 34L"+f+" 34Z"}).join("");
 const med=st?`<line x1="0" x2="200" y1="${Y(st.med)}" y2="${Y(st.med)}" stroke="var(--mute)" stroke-dasharray="2 3" stroke-width="1" vector-effect="non-scaling-stroke"/>`:"";
 return `<svg class="spark" viewBox="0 0 200 34" preserveAspectRatio="none"><path d="${area}" fill="var(--fill)"/>${med}<path d="${d}" fill="none" stroke="var(--ink)" stroke-width="1.2" vector-effect="non-scaling-stroke" stroke-linejoin="round"/></svg>`}
function bigChart(series,o){
 /* series: [{pts, color}] -- pts [t,v] or [t,mean,min,max]; o: {h, st, unit, ann} */
 const H=o.h||180,all=series.flatMap(s=>s.pts);if(all.length<2)return `<div class="sub" style="padding:30px 0">Not enough history for this range yet.</div>`;
 const now=Date.now()/1000,x1=Math.max(...all.map(p=>p[0]),now-60),x0=Math.min(x1-HOURS*3600*0.98,Math.max(Math.min(...all.map(p=>p[0])),now-HOURS*3600));
 let y0=Math.min(...all.map(p=>p.length>2?p[2]:p[1])),y1=Math.max(...all.map(p=>p.length>2?p[3]:p[1]));const pad=(y1-y0)*.08||1;y0-=pad;y1+=pad;
 const X=t=>(t-x0)/(x1-x0)*1000,Y=v=>(1-(v-y0)/(y1-y0))*H,P=v=>(Y(v)/H*100).toFixed(2)+"%",L=t=>(X(t)/10).toFixed(2)+"%";
 let svg="",html="";
 for(const v of nice(y0,y1,4)){svg+=`<line x1="0" x2="1000" y1="${Y(v)}" y2="${Y(v)}" stroke="var(--rule)" stroke-width="1" vector-effect="non-scaling-stroke"/>`;html+=`<div class="ylab" style="top:${Y(v)}px">${num(v)}</div>`}
 const span=x1-x0,steps=[900,1800,3600,3*3600,6*3600,12*3600,86400,2*86400,7*86400,14*86400,30.44*86400,61*86400];const step=steps.find(s=>span/s<=7)||91*86400;
 if(step>=30*86400){const d=D(x0);d.setDate(1);d.setHours(0,0,0,0);for(let m=0;m<14;m++){const t=new Date(d.getFullYear(),d.getMonth()+m,1).getTime()/1000;if(t<x0||t>x1)continue;
  if(step>40*86400&&new Date(t*1000).getMonth()%2)continue;svg+=`<line x1="${X(t)}" x2="${X(t)}" y1="0" y2="${H}" stroke="var(--rule)" stroke-width="1" vector-effect="non-scaling-stroke"/>`;html+=`<div class="lab" style="left:${L(t)};top:${H+4}px">${tick(t,span)}</div>`}}
 else{const off=new Date().getTimezoneOffset()*60;for(let t=Math.ceil((x0-off)/step)*step+off;t<=x1;t+=step){if(X(t)<30||X(t)>960)continue;
  svg+=`<line x1="${X(t)}" x2="${X(t)}" y1="0" y2="${H}" stroke="var(--rule)" stroke-width="1" vector-effect="non-scaling-stroke"/>`;html+=`<div class="lab" style="left:${L(t)};top:${H+4}px">${tick(t,span)}</div>`}}
 if(o.st&&series.length===1){const st=o.st;
  svg+=`<rect x="0" width="1000" y="${Y(st.p90)}" height="${Math.max(0,Y(st.p10)-Y(st.p90))}" fill="var(--band)"/>`;
  svg+=`<line x1="0" x2="1000" y1="${Y(st.med)}" y2="${Y(st.med)}" stroke="var(--mute)" stroke-dasharray="4 4" stroke-width="1" vector-effect="non-scaling-stroke"/>`;
  html+=`<div class="ylab" style="top:${Y(st.med)}px;left:0;right:auto;color:var(--ink);padding:0 4px 0 0">median ${num(st.med)}</div>`}
 for(const s of series){const pts=s.pts.length>700?thin(s.pts,700):s.pts,gap=Math.max(900,(x1-x0)/120);
  if(pts[0].length>2){const up=pts.map(p=>[p[0],p[3]]),dn=pts.map(p=>[p[0],p[2]]).reverse();svg+=`<path d="${path(up,X,Y,1e12)}L${path(dn,X,Y,1e12).slice(1)}Z" fill="${s.color}" opacity=".12"/>`}
  svg+=`<path d="${path(pts,X,Y,gap)}" fill="none" stroke="${s.color}" stroke-width="${series.length>1?1.6:1.4}" vector-effect="non-scaling-stroke" stroke-linejoin="round"/>`;
  const l=s.pts[s.pts.length-1];html+=`<div class="dot" style="left:${L(l[0])};top:${Y(l[1])}px;background:${s.color}"></div>`}
 if(o.st&&series.length===1&&o.ann){const st=o.st;
  const side=t=>X(t)<120?";transform:translate(0,-140%)":X(t)>880?";transform:translate(-100%,-140%)":"",sideLo=t=>side(t).replace("-140%","40%");
  html+=`<div class="ann" style="left:${L(st.tmax)};top:${Y(st.max)}px${side(st.tmax)}">▲ ${num(st.max)} <span style="color:var(--mute)">${when(st.tmax)}</span></div>`;
  html+=`<div class="ann lo" style="left:${L(st.tmin)};top:${Y(st.min)}px${sideLo(st.tmin)}">▼ ${num(st.min)} <span style="color:var(--mute)">${when(st.tmin)}</span></div>`}
 return `<div class="chart" style="height:${H+18}px"><svg viewBox="0 0 1000 ${H}" height="${H}" preserveAspectRatio="none">${svg}</svg>${html}</div>`}
function bars(vals,labels,hilite,fmtv){const ok=vals.filter(v=>v!=null&&isFinite(v));if(!ok.length)return `<div class="sub">–</div>`;
 const lo=Math.min(...ok),hi=Math.max(...ok),base=lo-(hi-lo)*.25;
 return `<div class="bars">${vals.map((v,i)=>v==null||!isFinite(v)?`<div class="none" style="height:2px" title="${labels[i]}: no data"></div>`:`<div class="${i===hilite?"med":""}" style="height:${Math.max(3,(v-base)/((hi-base)||1)*100)}%" title="${labels[i]}: ${fmtv(v)}"></div>`).join("")}</div>`}

/* ---- one reading, opened ---- */
function profile(pts,keyOf,n){const sum=new Array(n).fill(0),cnt=new Array(n).fill(0);for(const p of pts){const k=keyOf(D(p[0]));sum[k]+=p[1];cnt[k]++}return sum.map((s,i)=>cnt[i]?s/cnt[i]:null)}
function detail(b,full,k,pts,u){const st=stats(pts);if(!st)return "";
 const f=v=>fmt(v,u),span=st.t1-st.t0,ch=st.last-st.first,cover=pts.length*(HIST[b.board]?.step||60)/Math.max(1,HOURS*3600);
 const rows=[["now",f(st.last)],["median",f(st.med)],["mean",f(st.mean)],["σ",num(st.sd)],["p10 – p90",num(st.p10)+" – "+num(st.p90)],
  ["min",f(st.min)+" <span>"+when(st.tmin)+"</span>"],["max",f(st.max)+" <span>"+when(st.tmax)+"</span>"],["range",num(st.max-st.min)],
  ["change",(ch>0?"+":"")+num(ch)+" in "+dur(span)],["trend",(st.slope>0?"+":"")+num(st.slope)+" / h"],["samples",st.n+(cover<0.98?" · "+Math.round(Math.min(1,cover)*100)+"% covered":"")]];
 let panels="";
 /* distribution */
 const NB=24,lo=st.min,hi=st.max,w=(hi-lo)/NB||1,hist=new Array(NB).fill(0);for(const p of pts)hist[Math.min(NB-1,Math.floor((p[1]-lo)/w))]++;
 panels+=`<div class="panel"><div class="k">distribution</div>${bars(hist,hist.map((_,i)=>num(lo+i*w)+"–"+num(lo+(i+1)*w)),Math.min(NB-1,Math.floor((st.med-lo)/w)),v=>v+" samples")}<div class="axis"><span>${num(lo)}</span><span>median ${num(st.med)}</span><span>${num(hi)}</span></div></div>`;
 /* by hour of day */
 if(span>6*3600&&(HIST[b.board]?.step||60)<=3600){const hp=profile(pts,d=>d.getHours(),24),ok=hp.filter(v=>v!=null),pk=hp.indexOf(Math.max(...ok));
  panels+=`<div class="panel"><div class="k">by hour of day · peak ${String(pk).padStart(2,"0")}:00</div>${bars(hp,hp.map((_,i)=>String(i).padStart(2,"0")+":00"),pk,f)}<div class="axis"><span>00</span><span>06</span><span>12</span><span>18</span><span>23</span></div></div>`}
 /* by weekday */
 if(span>=6*86400){const wp=profile(pts,d=>(d.getDay()+6)%7,7),ok=wp.filter(v=>v!=null),pk=wp.indexOf(Math.max(...ok));const nm=["Mon","Tue","Wed","Thu","Fri","Sat","Sun"];
  panels+=`<div class="panel"><div class="k">by weekday · highest ${nm[pk]}</div>${bars(wp,nm,pk,f)}<div class="axis">${nm.map(n=>`<span>${n[0]}</span>`).join("")}</div></div>`}
 /* the seasons: by month, and a calendar of daily means */
 if(span>=45*86400){const mp=profile(pts,d=>d.getMonth(),12),ok=mp.filter(v=>v!=null),pk=mp.indexOf(Math.max(...ok));
  panels+=`<div class="panel"><div class="k">by month · highest ${MONTHS[pk]}</div>${bars(mp,MONTHS,pk,f)}<div class="axis">${MONTHS.map(m=>`<span>${m[0]}</span>`).join("")}</div></div>`}
 if(span>=20*86400){const day={};for(const p of pts){const d=D(p[0]);const key=new Date(d.getFullYear(),d.getMonth(),d.getDate()).getTime();(day[key]=day[key]||[]).push(p[1])}
  const keys=Object.keys(day).map(Number).sort((a,b)=>a-b),means=keys.map(k=>day[k].reduce((a,b)=>a+b,0)/day[k].length),mlo=Math.min(...means),mhi=Math.max(...means);
  const start=new Date(keys[0]);start.setDate(start.getDate()-((start.getDay()+6)%7));let cells="";
  for(let d=new Date(start);d.getTime()<=keys[keys.length-1];d.setDate(d.getDate()+1)){const i=keys.indexOf(d.getTime());
   cells+=i<0?`<div title="${d.toDateString()}: no data"></div>`:`<div style="background:var(--sig);opacity:${(.12+.88*(means[i]-mlo)/((mhi-mlo)||1)).toFixed(2)}" title="${d.toDateString()}: ${f(means[i])}"></div>`}
  panels+=`<div class="panel" style="grid-column:span 2;min-width:0"><div class="k">daily mean · ${num(mlo)} → ${num(mhi)}</div><div class="cal">${cells}</div><div class="axis"><span>${new Date(keys[0]).toLocaleDateString([], {month:"short",day:"numeric"})}</span><span>${new Date(keys[keys.length-1]).toLocaleDateString([], {month:"short",day:"numeric"})}</span></div></div>`}
 return `<div class="detail"><div>${bigChart([{pts,color:"var(--ink)"}],{h:200,st,unit:u,ann:true})}</div>
 <div class="stats">${rows.map(r=>`<div><span>${r[0]}</span><b style="font-weight:500">${r[1]}</b></div>`).join("")}</div><div class="panels">${panels}</div></div>`}

/* ---- boards ---- */
function seriesOf(b,full){const h=HIST[b.board];return h&&h.series[full]||[]}
function cell(b,sect,k,v,small){const full=sect+"."+k,id=b.board+"."+full,[name,u]=unit(k),open=OPEN.has(id),pts=seriesOf(b,full),st=stats(pts);
 const shown=u==="s"?dur(v):num(v)+(U[u]?`<small>${U[u]}</small>`:"");
 let d="";if(st&&st.n>2){const tr=st.slope,rel=Math.abs(tr)*Math.min(24,HOURS)>(st.sd||1)*0.5;d=`<div class="d">med <b>${num(st.med)}</b> · σ ${num(st.sd)}${rel?` · <span class="${tr>0?"up":"dn"}">${tr>0?"↑":"↓"}</span>`:""}</div>`}
 if(open)return `<div class="cell open" data-id="${esc(id)}"><button class="close" data-close="${esc(id)}">close ×</button><div class="k">${esc(b.board)} · ${esc(words(name))}</div><div class="v">${shown}</div>${detail(b,full,k,pts,u)}</div>`;
 return `<div class="cell" data-id="${esc(id)}"><div class="k">${esc(words(name))}</div><div class="v">${shown}</div>${small?"":d}${spark(pts,small?null:st)}</div>`}
const SYS={uptime_s:v=>"up "+dur(v),heap_kb:v=>"heap "+num(v)+" KB",psram_free_kb:v=>"PSRAM "+num(v/1024)+" MB free",chip_c:v=>"chip "+num(v)+" °C",cpu_mhz:v=>num(v)+" MHz",wifi_mhz:v=>num(v)+" MHz",last_reset:v=>"reset: "+v};
function sysline(b){const s=b.sys||{},out=[];
 if(s.rssi_dbm!=null)out.push(num(s.rssi_dbm)+" dBm"+(s.network?" on "+s.network:"")+(s.wifi_ch?" ch "+s.wifi_ch:""));
 for(const k of ["uptime_s","cpu_mhz","heap_kb","psram_free_kb","chip_c","last_reset"])if(s[k]!=null)out.push(SYS[k](s[k]));
 return out.map(esc).join("<i>/</i>")+` <u>${SYSOPEN.has(b.board)?"less ▴":"more ▾"}</u>`}
function board(b,now){const age=now-b.last_seen,ev=b.every||60,on=age<ev*2.5,stale=age>ev*10;
 const st=on?`reporting · ${dur(age)} ago`:`silent for ${dur(age)}`,s=b.stats||{},nums=Object.keys(s).filter(k=>typeof s[k]!=="string"),words_=Object.keys(s).filter(k=>typeof s[k]==="string");
 const sy=b.sys||{},sysn=Object.keys(sy).filter(k=>typeof sy[k]==="number"),syss=Object.keys(sy).filter(k=>typeof sy[k]==="string");
 return `<section class="board${stale?" stale":""}"><div class="who"><div><h2>${esc(b.board)}</h2><div class="state ${on?"on":"off"}">${st}</div></div>
 <div class="meta">${b.mac?`mac <span>${esc(b.mac)}</span><br>`:""}${b.fw?`fw <span>${esc(b.fw)}</span><br>`:""}every <span>${dur(ev)}</span> · <span>${b.reports}</span> reports<br>first seen <span>${D(b.first_seen).toLocaleDateString([], {day:"numeric",month:"short"})}</span></div></div>
 <div class="grid">${nums.length?nums.map(k=>cell(b,"stats",k,s[k])).join(""):`<div class="cell"><div class="k">no readings</div></div>`}</div>
 ${words_.length?`<div class="facts">${words_.map(k=>`<div><div class="k">${esc(words(k))}</div><div class="f">${esc(s[k])}</div></div>`).join("")}</div>`:""}
 <div class="sys" data-sys="${esc(b.board)}">${sysline(b)}</div>
 ${SYSOPEN.has(b.board)?`<div class="grid small">${sysn.map(k=>cell(b,"sys",k,sy[k],true)).join("")}</div>
 <div class="facts">${syss.filter(k=>k!=="network").map(k=>`<div><div class="k">${esc(words(k))}</div><div class="f" style="font-size:13px">${esc(sy[k])}</div></div>`).join("")}</div>`:""}</section>`}

/* ---- across the boards: any reading two or more share ---- */
function compare(bs){const by={};for(const b of bs)for(const k of Object.keys(b.stats||{}))if(typeof b.stats[k]==="number")(by[k]=by[k]||[]).push(b);
 const ks=Object.keys(by).filter(k=>by[k].length>1&&unit(k)[1]);if(!ks.length)return "";
 const cols=["var(--s0)","var(--s1)","var(--s2)","var(--s3)","var(--s4)","var(--s5)"],ci={};bs.forEach((b,i)=>ci[b.board]=cols[i%cols.length]);
 return `<div class="cmp-h"><div><h4>Across the boards</h4><div class="sub">every reading two or more boards share, over the last ${RANGES.find(r=>r[1]===HOURS)[0]}</div></div></div>`+
 ks.map(k=>{const[name,u]=unit(k);const ser=by[k].map(b=>({b,pts:seriesOf(b,"stats."+k),color:ci[b.board]})).filter(s=>s.pts.length>1);
  const legend=by[k].map(b=>{const st=stats(seriesOf(b,"stats."+k));return `<span><i style="background:${ci[b.board]}"></i>${esc(b.board)} <b style="font-weight:500">${fmt(b.stats[k],u)}</b>${st?` <span style="color:var(--mute)">med ${num(st.med)}</span>`:""}</span>`}).join("");
  return `<div class="cmp"><div class="k">${esc(words(name))}${U[u]?" · "+U[u]:""}</div><div class="legend">${legend}</div>${bigChart(ser,{h:150})}</div>`}).join("")}

function render(){if(!DATA)return;const now=DATA.now+Math.round((Date.now()-DATA.got)/1000);
 const bs=[...DATA.boards].sort((a,b)=>a.board.localeCompare(b.board)),live=bs.filter(b=>now-b.last_seen<(b.every||60)*2.5).length;
 $("compare").innerHTML=compare(bs);
 $("boards").innerHTML=bs.length?bs.map(b=>board(b,now)).join(""):`<div class="none">Nothing has reported yet. A board shows up here the first time it does.</div>`;
 $("head").innerHTML=`<b>${bs.length}</b> board${bs.length==1?"":"s"} · <b>${live}</b> reporting<br>${new Date().toLocaleDateString([], {weekday:"short",day:"numeric",month:"short"})} ${new Date().toLocaleTimeString([], {hour:"2-digit",minute:"2-digit"})}`;
 $("ranges").innerHTML=RANGES.map(r=>`<button data-h="${r[1]}" class="${r[1]===HOURS?"on":""}">${r[0]}</button>`).join("");
 $("upd").textContent="updated "+new Date(DATA.got).toLocaleTimeString()+" · readings every 10 s · history from "+(HOURS<=48?"each minute":HOURS<=168?"5-minute means":HOURS<=744?"hourly summaries":"daily summaries")}

const base=()=>location.pathname.replace(/\/boards\/?$/,"").replace(/\/+$/,"");
async function get(path){const r=await fetch(base()+path,{headers:{Authorization:"Bearer "+TOKEN},cache:"no-store"});
 if(r.status===401){store.set("gadgets_token",null);TOKEN=null;show();throw new Error("token")}return r.json()}
async function loadHistory(force){if(!DATA)return;const fresh=HOURS<=48?60e3:300e3;
 await Promise.all(DATA.boards.map(async b=>{const h=HIST[b.board];if(!force&&h&&h.hours===HOURS&&Date.now()-h.got<fresh)return;
  try{const r=await get(`/boards/history?board=${encodeURIComponent(b.board)}&hours=${HOURS}`);HIST[b.board]={hours:HOURS,got:Date.now(),step:r.step,series:r.series}}catch(e){}}));render()}
function appendLatest(){/* the newest report onto the minute history, between history fetches */
 for(const b of DATA.boards){const h=HIST[b.board];if(!h||h.step!==60)continue;
  for(const[sect,vals]of[["stats",b.stats||{}],["sys",b.sys||{}]])for(const k of Object.keys(vals)){if(typeof vals[k]!=="number")continue;const s=h.series[sect+"."+k]=h.series[sect+"."+k]||[];
   if(!s.length||b.last_seen-s[s.length-1][0]>=50)s.push([b.last_seen,vals[k]])}}}
async function load(){try{DATA=await get("/boards/data");DATA.got=Date.now();appendLatest();render();loadHistory(false)}catch(e){if(e.message!=="token")$("upd").textContent="can't reach the server, trying again"}}
function show(){$("gate").hidden=!!TOKEN;$("app").hidden=!TOKEN;if(TOKEN)load();else $("tok").focus()}
$("go").onclick=()=>{const t=$("tok").value.trim();if(!t)return;TOKEN=t;store.set("gadgets_token",t);show()};
$("tok").onkeydown=e=>{if(e.key==="Enter")$("go").click()};
$("out").onclick=()=>{store.set("gadgets_token",null);TOKEN=null;show()};
$("ranges").onclick=e=>{const h=e.target.dataset&&e.target.dataset.h;if(!h)return;HOURS=+h;store.set("gadgets_hours",h);render();loadHistory(true)};
document.addEventListener("click",e=>{const x=e.target.closest("[data-close]");if(x){OPEN.delete(x.dataset.close);store.set("gadgets_open",JSON.stringify([...OPEN]));render();return}
 const sy=e.target.closest("[data-sys]");if(sy){const n=sy.dataset.sys;SYSOPEN.has(n)?SYSOPEN.delete(n):SYSOPEN.add(n);store.set("gadgets_sys",JSON.stringify([...SYSOPEN]));render();return}
 const c=e.target.closest(".cell[data-id]");if(!c||c.classList.contains("open"))return;OPEN.add(c.dataset.id);store.set("gadgets_open",JSON.stringify([...OPEN]));render()});
show();setInterval(()=>{if(TOKEN)load()},10000);setInterval(()=>{if(DATA&&!document.hidden)render()},30000);
</script></body></html>"""
