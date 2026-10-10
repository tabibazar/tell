#!/usr/bin/env python3
"""Make stan-claw's three calls from the Mac, exactly as the board will:
speech -> Deepgram -> Claude (+ the remote MCP server) -> ElevenLabs -> speaker.

    tools/stan-claw-live.py "What can you do?"

Keys from secrets/stan-claw.env (gitignored). Standard library only.
"""
import json, os, subprocess, sys, tempfile, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
env = {}
for line in open(os.path.join(ROOT, "secrets", "stan-claw.env")):
    if "=" in line and not line.startswith("#"):
        k, v = line.rstrip("\n").split("=", 1)
        env[k] = v

def post(url, headers, body, timeout):
    req = urllib.request.Request(url, data=body, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()

question = sys.argv[1] if len(sys.argv) > 1 else "What can you do?"
tmp = tempfile.mkdtemp()
wav = os.path.join(tmp, "q.wav")
subprocess.run(["say", "-o", wav, "--data-format=LEI16@16000", question], check=True)

code, body = post("https://api.deepgram.com/v1/listen?model=nova-3&smart_format=true",
                  {"Authorization": "Token " + env["DEEPGRAM_KEY"], "Content-Type": "audio/wav"},
                  open(wav, "rb").read(), 15)
print("deepgram", code)
heard = json.loads(body)["results"]["channels"][0]["alternatives"][0]["transcript"]
print("  heard:", heard)

req = {"model": env.get("MODEL") or "claude-sonnet-5", "max_tokens": 1024,
       "system": "You are stan-claw, a voice assistant on a small screen. Answer in one to three short spoken sentences.",
       "messages": [{"role": "user", "content": heard}]}
headers = {"x-api-key": env["CLAUDE_KEY"], "anthropic-version": "2023-06-01", "content-type": "application/json"}
if env.get("MCP_URL"):
    srv = {"type": "url", "url": env["MCP_URL"], "name": "remote"}
    if env.get("MCP_TOKEN"):
        srv["authorization_token"] = env["MCP_TOKEN"]
    req["mcp_servers"] = [srv]
    req["tools"] = [{"type": "mcp_toolset", "mcp_server_name": "remote"}]
    headers["anthropic-beta"] = "mcp-client-2025-11-20"
code, body = post("https://api.anthropic.com/v1/messages", headers, json.dumps(req).encode(), 60)
print("claude", code)
reply = json.loads(body)
if code != 200:
    sys.exit("  " + json.dumps(reply)[:300])
text = " ".join(b["text"] for b in reply["content"] if b["type"] == "text")
tools = sorted({b["name"] for b in reply["content"] if b["type"] == "mcp_tool_use"})
print("  answer:", text)
print("  remote tools used:", ", ".join(tools) or "none")

code, pcm = post("https://api.elevenlabs.io/v1/text-to-speech/%s/stream?output_format=pcm_16000" % env["ELEVENLABS_VOICE"],
                 {"xi-api-key": env["ELEVENLABS_KEY"], "Content-Type": "application/json"},
                 json.dumps({"text": text, "model_id": "eleven_flash_v2_5"}).encode(), 20)
print("elevenlabs", code, len(pcm), "bytes =", round(len(pcm) / 32000, 1), "s")
if code == 200:
    out = os.path.join(tmp, "a.wav")
    import wave
    with wave.open(out, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(16000); w.writeframes(pcm)
    subprocess.run(["afplay", out])
