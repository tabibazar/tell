"""The boards' dashboard: every board that reports over WiFi, on one page.

A board POSTs to /boards/report, now and then (a minute apart, say):

    Authorization: Bearer <BOARDS_TOKEN>
    {"board": "tiny1", "mac": "68:ee:..", "every": 60,
     "sys":   {"uptime_s": 812, "heap_kb": 211.4, "rssi_dbm": -61, "ssid": "..", ...},
     "stats": {"temp_c": 23.1, "pressure_hpa": 1012.6, "photos": 14, ...}}

Nothing here knows any board: a new one shows up the first time it reports,
and its numbers are drawn by their names. A key's last part is its unit
(_c, _pct, _hpa, _dba, _ppm, _ppb, _kb, _dbm, _s, _g, _dps, _mv, _v); a
number gets a line of its recent history, a string is shown as it is.

  pk "boards"  sk "b#<board>"   the latest report, when it came and from where,
                                and each number's history: a point a minute at
                                most, the last HIST_H hours

The page (/boards) holds no data. It asks for BOARDS_TOKEN once, keeps it
in the browser, and fetches /boards/data with it -- the same token the
boards report with (an environment variable; never in git).
"""
import hmac
import json
import os
import re
import time

import state

HIST_H = 24             # hours of history kept per number
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
            "body": json.dumps(d)}


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
    hist = old.get("hist", {})
    for sect, vals in (("sys", sys_), ("stats", stats)):
        for k, v in vals.items():
            if isinstance(v, str):
                continue
            pts = hist.setdefault(sect + "." + k, [])
            if pts and now - pts[-1][0] < HIST_GAP_S:
                pts[-1] = [pts[-1][0], v]           # too soon after the last point: replace it
            else:
                pts.append([now, v])
    cut = now - HIST_H * 3600
    hist = {k: [p for p in pts if p[0] >= cut] for k, pts in hist.items()}
    hist = {k: pts for k, pts in hist.items() if pts}
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
        "hist": hist,
    }
    state.put("boards", "b#" + name, d)
    return True, ""


def all_boards():
    return [d for _, d in state.query("boards", "b#")]


def forget(name):
    state.delete("boards", "b#" + name)


def handle(path, method, headers, body, ip=""):
    if path in ("/boards", "/boards/") and method == "GET":
        return {"statusCode": 200, "headers": {"Content-Type": "text/html; charset=utf-8"}, "body": PAGE}
    if not _token_ok(headers):
        return {"statusCode": 401, "headers": {"WWW-Authenticate": "Bearer"}, "body": ""}
    if path == "/boards/report" and method == "POST":
        try:
            report = json.loads(body or "")
        except ValueError:
            return _json(400, {"ok": False, "why": "not JSON"})
        ok, why = record(report, ip)
        return _json(200 if ok else 400, {"ok": ok, "why": why, "now": int(time.time())})
    if path == "/boards/data" and method == "GET":
        return _json(200, {"now": int(time.time()), "boards": all_boards()})
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
<title>Boards</title>
<style>
:root{--bg:#0f1115;--card:#181b22;--line:#262a33;--text:#e8eaf0;--dim:#8b92a3;--ok:#3ecf8e;--warn:#f5a524;--off:#5b6170;--accent:#6aa9ff}
@media (prefers-color-scheme: light){:root{--bg:#f3f4f7;--card:#fff;--line:#e3e6ec;--text:#161a22;--dim:#687083;--off:#a7adba;--accent:#2f6fde}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.4 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
header{display:flex;align-items:baseline;gap:12px;padding:20px 16px 8px;max-width:1300px;margin:auto}
h1{font-size:22px;margin:0}
#meta{color:var(--dim);font-size:13px}
main{display:grid;grid-template-columns:repeat(auto-fill,minmax(340px,1fr));gap:14px;padding:8px 16px 32px;max-width:1300px;margin:auto}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px 16px}
.top{display:flex;align-items:center;gap:10px}
.dot{width:10px;height:10px;border-radius:50%;background:var(--off);flex:none}
.on .dot{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.late .dot{background:var(--warn)}
.name{font-weight:650;font-size:18px}
.seen{margin-left:auto;color:var(--dim);font-size:12px;text-align:right}
.ids{color:var(--dim);font-size:12px;margin:4px 0 10px;font-family:ui-monospace,Menlo,monospace;word-break:break-all}
h3{font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin:12px 0 6px;font-weight:600}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:8px}
.m{border:1px solid var(--line);border-radius:10px;padding:8px 10px;min-width:0}
.m .k{color:var(--dim);font-size:12px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.m .v{font-size:20px;font-weight:600;font-variant-numeric:tabular-nums;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.m .u{font-size:12px;color:var(--dim);font-weight:400;margin-left:3px}
.m svg{display:block;width:100%;height:30px;margin-top:4px}
.m .r{display:flex;justify-content:space-between;color:var(--dim);font-size:10px}
.m.s .v{font-size:14px;font-weight:500}
.off .m .v{color:var(--dim)}
#login{max-width:420px;margin:60px auto;padding:0 16px}
#login input{width:100%;padding:10px;border-radius:8px;border:1px solid var(--line);background:var(--card);color:var(--text);font-size:15px}
#login button,.btn{margin-top:10px;padding:9px 16px;border-radius:8px;border:0;background:var(--accent);color:#fff;font-size:15px;cursor:pointer}
.btn.small{padding:4px 10px;font-size:12px;background:transparent;color:var(--dim);border:1px solid var(--line)}
#empty{color:var(--dim);padding:16px}
</style></head><body>
<div id="login" hidden><h1>Boards</h1><p>The boards' token, once; this browser keeps it.</p>
<input id="tok" type="password" autocomplete="current-password"><button id="go">Show the boards</button></div>
<header id="hdr" hidden><h1>Boards</h1><span id="meta"></span><span style="flex:1"></span><button class="btn small" id="out">Forget token</button></header>
<main id="cards"></main>
<script>
const UNITS={c:"°C",pct:"%",hpa:"hPa",dba:"dBA",ppm:"ppm",ppb:"ppb",kb:"KB",mb:"MB",dbm:"dBm",s:"",g:"g",dps:"°/s",mv:"mV",v:"V",ms:"ms",hz:"Hz",lux:"lx"};
let TOKEN=null;try{TOKEN=localStorage.getItem("boards_token")}catch(e){}
const $=id=>document.getElementById(id);
function split(k){const i=k.lastIndexOf("_");const u=i>0?k.slice(i+1):"";if(u in UNITS)return [k.slice(0,i),u];return [k,""]}
function label(k){return k.replace(/_/g," ")}
function dur(s){s=Math.round(s);if(s<60)return s+" s";if(s<3600)return Math.floor(s/60)+" min";if(s<86400)return Math.floor(s/3600)+" h "+Math.floor(s%3600/60)+" min";return Math.floor(s/86400)+" d "+Math.floor(s%86400/3600)+" h"}
function num(v){const a=Math.abs(v);return a>=1000?v.toFixed(0):a>=100?v.toFixed(1).replace(/\.0$/,""):a>=10?v.toFixed(1):(Math.round(v*100)/100).toString()}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]))}
function spark(pts){if(!pts||pts.length<2)return "";const xs=pts.map(p=>p[0]),ys=pts.map(p=>p[1]);
 const x0=Math.min(...xs),x1=Math.max(...xs),y0=Math.min(...ys),y1=Math.max(...ys);const W=200,H=30;
 const d=pts.map((p,i)=>(i?"L":"M")+((p[0]-x0)/(x1-x0||1)*W).toFixed(1)+","+(H-2-(p[1]-y0)/(y1-y0||1)*(H-4)).toFixed(1)).join("");
 return `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none"><path d="${d}" fill="none" stroke="var(--accent)" stroke-width="1.6" vector-effect="non-scaling-stroke"/></svg><div class="r"><span>${num(y0)}</span><span>${dur((x1-x0))}</span><span>${num(y1)}</span></div>`}
function metric(sect,k,v,b){const [name,u]=split(k);
 if(typeof v==="string")return `<div class="m s"><div class="k">${esc(label(k))}</div><div class="v" title="${esc(v)}">${esc(v)}</div></div>`;
 const shown=u==="s"?dur(v):num(v);
 return `<div class="m"><div class="k">${esc(label(name))}</div><div class="v">${shown}<span class="u">${UNITS[u]||""}</span></div>${spark(b.hist[sect+"."+k])}</div>`}
function section(title,sect,b){const o=b[sect]||{};const ks=Object.keys(o);if(!ks.length)return "";
 return `<h3>${title}</h3><div class="grid">${ks.map(k=>metric(sect,k,o[k],b)).join("")}</div>`}
function card(b,now){const age=now-b.last_seen,ev=b.every||60;const st=age<ev*2.5?"on":age<ev*10?"late":"off";
 const ids=[b.mac,b.ip,b.fw].filter(Boolean).map(esc).join(" · ");
 return `<div class="card ${st}"><div class="top"><span class="dot"></span><span class="name">${esc(b.board)}</span>
 <span class="seen">${st==="on"?"live":"last seen "+dur(age)+" ago"}<br>every ${dur(ev)} · ${b.reports} reports</span></div>
 <div class="ids">${ids}</div>${section("Sensors","stats",b)}${section("System","sys",b)}</div>`}
async function load(){
 let r;try{r=await fetch(location.pathname.replace(/\/boards\/?$/,"").replace(/\/+$/,"")+"/boards/data",{headers:{Authorization:"Bearer "+TOKEN},cache:"no-store"})}catch(e){$("meta").textContent="offline, retrying";return}
 if(r.status===401){try{localStorage.removeItem("boards_token")}catch(e){}TOKEN=null;show();return}
 const d=await r.json();const bs=d.boards.sort((a,b)=>b.last_seen-a.last_seen);
 $("cards").innerHTML=bs.length?bs.map(b=>card(b,d.now)).join(""):`<div id="empty">No board has reported yet.</div>`;
 const live=bs.filter(b=>d.now-b.last_seen<(b.every||60)*2.5).length;
 $("meta").textContent=`${live} of ${bs.length} live · updated ${new Date().toLocaleTimeString()}`}
function show(){const has=!!TOKEN;$("login").hidden=has;$("hdr").hidden=!has;$("cards").hidden=!has;if(has)load()}
$("go").onclick=()=>{TOKEN=$("tok").value.trim();try{localStorage.setItem("boards_token",TOKEN)}catch(e){}show()};
$("tok").onkeydown=e=>{if(e.key==="Enter")$("go").click()};
$("out").onclick=()=>{try{localStorage.removeItem("boards_token")}catch(e){}TOKEN=null;show()};
show();setInterval(()=>{if(TOKEN)load()},10000);
</script></body></html>"""
