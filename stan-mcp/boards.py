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
<title>Gadgets</title>
<link rel="preconnect" href="https://fonts.googleapis.com"><link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Instrument+Serif&family=IBM+Plex+Mono:wght@400;500&family=IBM+Plex+Sans:wght@400;500&display=swap" rel="stylesheet">
<style>
:root{--paper:#f3f0e8;--ink:#1d1c1a;--mute:#7a756b;--rule:#d8d2c4;--fill:rgba(29,28,26,.07);--sig:#c8431a;--ok:#2e6a4c}
@media (prefers-color-scheme:dark){:root{--paper:#131312;--ink:#ebe6da;--mute:#8b8579;--rule:#2c2a26;--fill:rgba(235,230,218,.07);--sig:#ff7d4f;--ok:#8fc6a2}}
*{box-sizing:border-box;margin:0}
body{background:var(--paper);color:var(--ink);font:14px/1.45 "IBM Plex Sans",system-ui,sans-serif;-webkit-font-smoothing:antialiased}
.wrap{max-width:1720px;margin:0 auto;padding:0 20px}
.mono{font-family:"IBM Plex Mono",ui-monospace,monospace}
header{display:flex;justify-content:space-between;align-items:flex-end;padding:36px 0 18px;gap:16px;flex-wrap:wrap}
.mark{font:400 52px/0.9 "Instrument Serif",serif;letter-spacing:-.01em}
.head-r{text-align:right;color:var(--mute);font:12px/1.6 "IBM Plex Mono",monospace}
.head-r b{color:var(--ink);font-weight:500}
#boards{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,540px),1fr));gap:0 44px;align-items:start}
.board{border-top:2px solid var(--ink);display:flex;flex-direction:column;gap:16px;padding:16px 0 36px;min-width:0}
.who{display:flex;justify-content:space-between;align-items:flex-start;gap:8px 20px;flex-wrap:wrap}
.who .meta{text-align:right}
.who h2{font:400 46px/1 "Instrument Serif",serif;margin:2px 0 10px;word-break:break-word}
.state{font:500 12px "IBM Plex Mono",monospace}
.state.on{color:var(--ok)} .state.off{color:var(--sig)}
.meta{color:var(--mute);font:11.5px/1.75 "IBM Plex Mono",monospace;word-break:break-all}
.meta span{color:var(--ink)}
.grid{display:grid;grid-auto-flow:dense;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));border-left:1px solid var(--rule);border-top:1px solid var(--rule)}
.cell{border-right:1px solid var(--rule);border-bottom:1px solid var(--rule);padding:11px 13px 9px;cursor:pointer;min-width:0;position:relative}
.cell:hover{background:var(--fill)}
.cell.open{grid-column:1/-1;cursor:zoom-out}
.k{font:500 10.5px "IBM Plex Sans",sans-serif;letter-spacing:.09em;text-transform:uppercase;color:var(--mute);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.v{font:400 27px/1.15 "IBM Plex Mono",monospace;font-variant-numeric:tabular-nums;margin-top:3px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.v small{font-size:12px;color:var(--mute);margin-left:4px}
.cell svg{display:block;width:100%;margin-top:6px;overflow:visible}
.rng{display:flex;justify-content:space-between;font:10px "IBM Plex Mono",monospace;color:var(--mute);margin-top:3px}
.facts{display:flex;flex-wrap:wrap;gap:10px 30px}
.facts .f{font:400 16px "IBM Plex Sans",sans-serif;margin-top:1px}
.sys{color:var(--mute);font:11.5px/1.7 "IBM Plex Mono",monospace;margin-top:-4px}
.sys i{font-style:normal;color:var(--rule);padding:0 7px}
.stale .v,.stale svg,.stale .f{opacity:.38}
footer{color:var(--mute);font:11px "IBM Plex Mono",monospace;padding:4px 0 40px;display:flex;justify-content:space-between;gap:12px;flex-wrap:wrap}
footer button{all:unset;cursor:pointer;text-decoration:underline;text-underline-offset:3px}
#gate{max-width:400px;padding-top:18vh}
#gate p{color:var(--mute);margin:14px 0 26px}
#gate input{width:100%;background:none;border:0;border-bottom:1.5px solid var(--ink);color:var(--ink);font:16px "IBM Plex Mono",monospace;padding:8px 0;outline:none}
#gate button{margin-top:22px;background:var(--ink);color:var(--paper);border:0;padding:10px 18px;font:500 13px "IBM Plex Sans",sans-serif;cursor:pointer}
.none{color:var(--mute);padding:30px 0;border-top:2px solid var(--ink);grid-column:1/-1}
@media (max-width:760px){.who .meta{text-align:left}.who h2{font-size:40px}.mark{font-size:44px}.head-r{text-align:left}}
</style></head><body>
<div class="wrap" id="gate" hidden><div class="mark">Gadgets</div>
<p>Enter the boards' token. This browser keeps it.</p>
<input id="tok" type="password" autocomplete="current-password" spellcheck="false"><br><button id="go">Open</button></div>
<div class="wrap" id="app" hidden>
<header><div class="mark">Gadgets</div><div class="head-r" id="head"></div></header>
<div id="boards"></div>
<footer><span id="upd"></span><button id="out">forget token</button></footer></div>
<script>
const U={c:"°C",pct:"%",hpa:"hPa",dba:"dB(A)",ppm:"ppm",ppb:"ppb",kb:"KB",mb:"MB",dbm:"dBm",deg:"°",min:"min",s:"",g:"g",dps:"°/s",mv:"mV",v:"V",ms:"ms",hz:"Hz",lux:"lx"};
const W={laeq:"LAeq",l90:"L90",tvoc:"TVOC",eco2:"eCO2",co2:"CO2",rssi:"RSSI",pm25:"PM2.5",pm10:"PM10",imu:"IMU",rtc:"RTC",sd:"SD",max:"max",1:"1 s"};
const $=id=>document.getElementById(id);
const store={get(k){try{return localStorage.getItem(k)}catch(e){return null}},set(k,v){try{v==null?localStorage.removeItem(k):localStorage.setItem(k,v)}catch(e){}}};
let TOKEN=store.get("gadgets_token"),DATA=null;const OPEN=new Set(JSON.parse(store.get("gadgets_open")||"[]"));
const esc=s=>String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
function unit(k){const i=k.lastIndexOf("_"),u=i>0?k.slice(i+1):"";return u in U?[k.slice(0,i),u]:[k,""]}
function words(k){return k.split("_").map(w=>W[w]||(w==="1s"?"1 s":w)).join(" ")}
function dur(s){s=Math.max(0,Math.round(s));if(s<60)return s+" s";if(s<3600)return Math.floor(s/60)+" min";
 if(s<86400){const h=Math.floor(s/3600),m=Math.floor(s%3600/60);return h+" h"+(m?" "+m+" min":"")}const d=Math.floor(s/86400),h=Math.floor(s%86400/3600);return d+" d"+(h?" "+h+" h":"")}
function num(v){const a=Math.abs(v);let t=a>=1000?v.toFixed(0):a>=100?v.toFixed(1):a>=10?v.toFixed(1):v.toFixed(2);t=t.replace(/(\.\d*?)0+$/,"$1").replace(/\.$/,"");return t.replace("-","−")}
function hhmm(t){return new Date(t*1000).toLocaleTimeString([], {hour:"2-digit",minute:"2-digit"})}
function thin(pts,n){if(pts.length<=n)return pts;const x0=pts[0][0],span=(pts[pts.length-1][0]-x0)||1,b=[];
 for(const p of pts){const i=Math.min(n-1,Math.floor((p[0]-x0)/span*n));(b[i]=b[i]||[]).push(p)}
 return b.filter(Boolean).map(g=>[g.reduce((a,p)=>a+p[0],0)/g.length,g.reduce((a,p)=>a+p[1],0)/g.length])}
function span(s){return s>=3600?Math.round(s/3600)+" h":dur(s)}
function chart(pts,big){
 if(!pts||pts.length<2)return "";
 const raw=pts;pts=thin(pts,big?240:48);
 const w=big?900:200,h=big?150:34,pad=big?18:2;
 const x0=pts[0][0],x1=pts[pts.length-1][0],ys=pts.map(p=>p[1]);let y0=Math.min(...raw.map(p=>p[1])),y1=Math.max(...raw.map(p=>p[1]));const r0=y0,r1=y1;
 if(y1-y0<1e-9){y0-=1;y1+=1}
 const X=t=>((t-x0)/(x1-x0||1))*w,Y=v=>pad+(1-(v-y0)/(y1-y0))*(h-pad*2);
 let d="",gaps=[];pts.forEach((p,i)=>{const brk=i&&p[0]-pts[i-1][0]>Math.max(900,(x1-x0)/(big?60:12));d+=(i&&!brk?"L":"M")+X(p[0]).toFixed(1)+" "+Y(p[1]).toFixed(1)});
 const area=d.split("M").filter(Boolean).map(seg=>{const c=seg.split("L");const f=c[0].split(" ")[0],l=c[c.length-1].split(" ")[0];return "M"+seg+"L"+l+" "+h+"L"+f+" "+h+"Z"}).join("");
 const last=pts[pts.length-1];let ticks="";
 if(big){const step=(x1-x0)>6*3600?3*3600:(x1-x0)>2*3600?3600:900;for(let t=Math.ceil(x0/step)*step;t<=x1;t+=step){const x=X(t).toFixed(1);if(X(t)<30||X(t)>w-40)continue;ticks+=`<line x1="${x}" x2="${x}" y1="0" y2="${h}" stroke="var(--rule)" stroke-width="1" vector-effect="non-scaling-stroke"/><text x="${x}" y="${h+14}" font-size="10" fill="var(--mute)" text-anchor="middle" font-family="IBM Plex Mono">${hhmm(t)}</text>`}
  ticks+=`<text x="${w}" y="${Y(r1)-4}" font-size="10" fill="var(--mute)" text-anchor="end" font-family="IBM Plex Mono">${num(r1)}</text><text x="${w}" y="${Y(r0)+12}" font-size="10" fill="var(--mute)" text-anchor="end" font-family="IBM Plex Mono">${num(r0)}</text>`}
 return `<svg viewBox="0 0 ${w} ${h+(big?18:0)}" height="${h+(big?18:0)}" preserveAspectRatio="none">${ticks}<path d="${area}" fill="var(--fill)"/><path d="${d}" fill="none" stroke="var(--ink)" stroke-width="${big?1.4:1.2}" vector-effect="non-scaling-stroke" stroke-linejoin="round"/><circle cx="${X(last[0])}" cy="${Y(last[1])}" r="${big?3:2.2}" fill="var(--sig)"/></svg>`+
  (big?"":`<div class="rng"><span>${num(r0)}</span><span>${span(x1-x0)}</span><span>${num(r1)}</span></div>`)}
function cell(b,k,v){const id=b.board+"."+k,[name,u]=unit(k),open=OPEN.has(id);
 const shown=u==="s"?dur(v):num(v)+(U[u]?`<small>${U[u]}</small>`:"");
 return `<div class="cell${open?" open":""}" data-id="${esc(id)}"><div class="k">${esc(words(name))}</div><div class="v">${shown}</div>${chart(b.hist["stats."+k],open)}</div>`}
const SYS={uptime_s:v=>"up "+dur(v),heap_kb:v=>"heap "+num(v)+" KB",heap_min_kb:v=>"low "+num(v)+" KB",psram_free_kb:v=>"psram "+num(v/1024)+" MB free",chip_c:v=>"chip "+num(v)+" °C",last_reset:v=>"last reset: "+v};
function sysline(b){const s=b.sys||{},out=[];
 if(s.rssi_dbm!=null)out.push(num(s.rssi_dbm)+" dBm"+(s.network?" on "+s.network:""));
 for(const k of Object.keys(s)){if(k==="rssi_dbm"||k==="network")continue;const f=SYS[k];if(f)out.push(f(s[k]));else{const[n,u]=unit(k);out.push(words(n)+" "+(typeof s[k]==="number"?(u==="s"?dur(s[k]):num(s[k])+(U[u]?" "+U[u]:"")):s[k]))}}
 return out.map(esc).join("<i>/</i>")}
function board(b,now){const age=now-b.last_seen,ev=b.every||60,on=age<ev*2.5,stale=age>ev*10;
 const st=on?`reporting · ${dur(age)} ago`:`silent for ${dur(age)}`;
 const st_=b.stats||{},ks=Object.keys(st_).filter(k=>typeof st_[k]!=="string"),ws=Object.keys(st_).filter(k=>typeof st_[k]==="string");
 return `<section class="board${stale?" stale":""}"><div class="who"><div><h2>${esc(b.board)}</h2><div class="state ${on?"on":"off"}">${st}</div></div>
 <div class="meta">${b.mac?`mac <span>${esc(b.mac)}</span><br>`:""}${b.fw?`fw <span>${esc(b.fw)}</span><br>`:""}every <span>${dur(ev)}</span> · <span>${b.reports}</span> reports<br>first seen <span>${new Date(b.first_seen*1000).toLocaleDateString([], {day:"numeric",month:"short"})}</span></div></div>
 <div class="grid">${ks.length?ks.map(k=>cell(b,k,b.stats[k])).join(""):`<div class="cell"><div class="k">no readings</div></div>`}</div>
 ${ws.length?`<div class="facts">${ws.map(k=>`<div><div class="k">${esc(words(k))}</div><div class="f">${esc(st_[k])}</div></div>`).join("")}</div>`:""}
 <div class="sys">${sysline(b)}</div></section>`}
function render(){if(!DATA)return;const now=DATA.now+Math.round((Date.now()-DATA.got)/1000);
 const bs=[...DATA.boards].sort((a,b)=>a.board.localeCompare(b.board));
 const live=bs.filter(b=>now-b.last_seen<(b.every||60)*2.5).length;
 $("boards").innerHTML=bs.length?bs.map(b=>board(b,now)).join(""):`<div class="none">Nothing has reported yet. A board shows up here the first time it does.</div>`;
 $("head").innerHTML=`<b>${bs.length}</b> board${bs.length==1?"":"s"} · <b>${live}</b> reporting<br>${new Date().toLocaleDateString([], {weekday:"short",day:"numeric",month:"short"})} ${new Date().toLocaleTimeString([], {hour:"2-digit",minute:"2-digit"})}`;
 $("upd").textContent="updated "+new Date(DATA.got).toLocaleTimeString()+" · every 10 s"}
async function load(){let r;try{r=await fetch(location.pathname.replace(/\/boards\/?$/,"").replace(/\/+$/,"")+"/boards/data",{headers:{Authorization:"Bearer "+TOKEN},cache:"no-store"})}catch(e){$("upd").textContent="can't reach the server, trying again";return}
 if(r.status===401){store.set("gadgets_token",null);TOKEN=null;show();return}
 DATA=await r.json();DATA.got=Date.now();render()}
function show(){$("gate").hidden=!!TOKEN;$("app").hidden=!TOKEN;if(TOKEN)load();else $("tok").focus()}
$("go").onclick=()=>{const t=$("tok").value.trim();if(!t)return;TOKEN=t;store.set("gadgets_token",t);show()};
$("tok").onkeydown=e=>{if(e.key==="Enter")$("go").click()};
$("out").onclick=()=>{store.set("gadgets_token",null);TOKEN=null;show()};
$("boards").onclick=e=>{const c=e.target.closest(".cell[data-id]");if(!c)return;const id=c.dataset.id;OPEN.has(id)?OPEN.delete(id):OPEN.add(id);store.set("gadgets_open",JSON.stringify([...OPEN]));render()};
show();setInterval(()=>{if(TOKEN)load()},10000);setInterval(render,1000);
</script></body></html>"""
