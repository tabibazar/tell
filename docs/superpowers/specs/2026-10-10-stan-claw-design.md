# stan-claw: a voice assistant board that is also an MCP server

Date: 2026-10-10. Status: design approved in chat; spec awaiting review.

## Purpose

stan-claw is a self-contained voice assistant on a Waveshare
ESP32-S3-Touch-LCD-4B (the board panel1 is). You tap, speak, and it answers
aloud and on screen, using Claude and the tools of an MCP server on the
internet. It is also an MCP server itself, on the local network, so an agent
such as Claude Code on a Mac can use its speaker, microphones and screen.

It is its own ESP-IDF project in `stan-claw/`. Whether it is flashed onto
panel1 (replacing the room-bookings firmware, which stays in the repo) is
decided later. No Mac is needed for it to work.

## Decisions

| Question | Decision |
|---|---|
| Role | Agent (answers questions) **and** MCP server (exposes voice, mic, display) |
| MCP server it uses | On the internet, public `https://` URL, reached through Anthropic's MCP connector |
| Start talking | Tap to talk (screen or BOOT); stops at a pause. No wake word |
| Speech to text | Deepgram |
| Text to speech | ElevenLabs |
| Its own MCP server | Local network only, bearer token, Streamable HTTP |
| WiFi | The networks already known (home, office), joined as panel1 does |

## Architecture

All on the board. One file per job:

| File | Job | Depends on |
|---|---|---|
| `lcd.c`, `touch.c` | the ST7701 panel and GT911 touch | copied from `panel1/main` |
| `net.c`, `config.c`, `console.c` | WiFi over several known networks, settings in NVS, USB console | copied from `panel1/main`, extended with the keys below |
| `audio.c` | ES8311 speaker + ES7210 mics through `esp_codec_dev`; record to a PSRAM buffer, play PCM; amplifier on TCA9554 EXIO3 | pattern from `main/speaker_app.c` |
| `vad.c` | end of speech: energy above the room's noise floor, then 800 ms of quiet; 15 s cap | pure, host-tested |
| `stt.c` | WAV to Deepgram, transcript back | `https.c` |
| `tts.c` | text to ElevenLabs, PCM streamed back and played as it arrives | `https.c`, `audio.c` |
| `agent.c` | one exchange with Claude: transcript in, answer text out, the MCP connector pointed at the internet server; the last 7 turns kept for follow-ups, forgotten after 5 min idle | `https.c`, `claude_msg.c` |
| `claude_msg.c` | builds the Messages request and parses the reply (text, `mcp_tool_use`, `mcp_tool_result`) | pure, host-tested |
| `mcpd.c` | its own MCP server: JSON-RPC over HTTP POST | `mcp_rpc.c`, `esp_http_server` |
| `mcp_rpc.c` | MCP JSON-RPC: initialize, tools/list, tools/call dispatch, errors | pure, host-tested |
| `https.c` | one HTTPS helper (POST, headers, body or stream, timeouts) for all three services | `esp_http_client`, cert bundle |
| `ui.c` | the screens: home (talk button), listening, thinking, speaking, answer text, errors, the LISTENING banner, agent-shown text/image | pure drawing, host-rendered |
| `stanclaw.c` | `app_main`, the one-at-a-time lock, tying it together | everything |

Fonts and drawing reuse `main/aafont.c`, `main/canvas.c` and panel1's faces.

### Hardware (from the board's schematic, as panel1 uses it)

- I2C SDA 47 / SCL 48: TCA9554 0x20, GT911 0x5D, ES8311 0x18, ES7210 0x40, AXP2101 0x34.
- I2S: MCLK 5, BCLK 16, LRCK 7, DOUT (to ES8311) 6, DIN (from ES7210) 15.
- Amplifier enable: TCA9554 EXIO3. Backlight: GPIO4, PWM inverted.
- Audio: 16 kHz, 16-bit mono for both directions.

## The conversation

1. Tap (or BOOT). The red **LISTENING** banner shows; recording starts.
2. `vad.c` ends it at a pause, or at 15 s. Under 0.3 s of speech: "didn't catch that", no calls.
3. `stt.c`: `POST https://api.deepgram.com/v1/listen?model=nova-3&smart_format=true`,
   `Authorization: Token <key>`, body `audio/wav`. Transcript from
   `results.channels[0].alternatives[0].transcript`.
4. `agent.c`: `POST https://api.anthropic.com/v1/messages` with
   `anthropic-version: 2023-06-01`, `anthropic-beta: mcp-client-2025-11-20`, and
   ```json
   {"model": "<model>", "max_tokens": 1024,
    "system": "You are stan-claw, a voice assistant on a small screen. Answer in one to three short spoken sentences.",
    "mcp_servers": [{"type": "url", "url": "<mcp_url>", "name": "remote", "authorization_token": "<mcp_token>"}],
    "tools": [{"type": "mcp_toolset", "mcp_server_name": "remote"}],
    "messages": [ ...the last turns..., {"role": "user", "content": "<transcript>"}]}
   ```
   The answer is the reply's `text` blocks; `mcp_tool_use`/`mcp_tool_result`
   blocks are noted on screen ("asked remote: <tool>") and not spoken. With no
   MCP URL set, the two MCP fields are left out. The default model is
   `claude-sonnet-5`, changeable in settings.
5. `tts.c`: `POST https://api.elevenlabs.io/v1/text-to-speech/<voice_id>/stream?output_format=pcm_16000`,
   `xi-api-key: <key>`, `{"text": "...", "model_id": "eleven_flash_v2_5"}`;
   PCM is played as it arrives while the text shows on screen.
6. Back home after 30 s, or at once on a tap.

## Its own MCP server

`http://stan-claw.local/mcp` (mDNS name `stan-claw`), Streamable HTTP:
each JSON-RPC request is a POST, and each answer is a single
`application/json` response (no SSE stream needed). Protocol version
`2025-06-18`; an `Mcp-Session-Id` is issued at initialize and accepted
thereafter. Every request needs `Authorization: Bearer <serve token>`;
otherwise 401.

| Tool | Arguments | Result |
|---|---|---|
| `speak` | `text` (string, up to 1000 chars) | after playback: "spoken" |
| `listen` | `max_seconds` (1-30, default 10) | the transcript, or "" for silence |
| `ask` | `question` (string) | speaks it, listens, returns the transcript |
| `show_text` | `text`, optional `title` | "shown" |
| `show_image` | `jpeg_base64` (baseline JPEG, up to 480x480, up to 200 KB) | "shown" |
| `clear_screen` | none | "cleared" |
| `set_volume` | `level` (0-100) | the new level |
| `status` | none | WiFi network, IP, volume, busy or idle |

Rules:
- **The LISTENING banner** shows whenever the mics are open, whoever opened
  them. Nothing can hide it.
- **One at a time.** A single lock covers the speaker, mics and screen.
  Calls arriving while it is held wait up to 30 s, then return a tool error
  "busy".
- Text and images an agent shows stay up until a tap or `clear_screen`.
- Connecting from Claude Code:
  `claude mcp add --transport http stan-claw http://stan-claw.local/mcp --header "Authorization: Bearer <token>"`.

Plain HTTP on the local network means the token can be sniffed by someone on
the same WiFi. That is accepted for now; HTTPS on the board can be added later.

## Settings

Kept in NVS, set over the USB console (`tools/stan-claw.py`, like
`tools/panel1.py`). They survive restarts and app reflashes and never go into
git. A freshly erased board is seeded from the gitignored
`secrets/stan-claw.env` (and its WiFi from the same `WIFI_`, `WIFI2_` ... lines
as `secrets/panel1.env`).

| Console command | Setting |
|---|---|
| `wifi add/forget/list` | as panel1 |
| `key claude <k>` / `key deepgram <k>` / `key elevenlabs <k>` | API keys (never echoed back) |
| `voice <id>` | ElevenLabs voice ID |
| `model <id>` | Claude model |
| `mcp url <https-url>` / `mcp token <t>` | the internet MCP server |
| `serve token <t>` | the token clients need for its own MCP server |
| `status` | network, which keys are set (yes/no only), MCP URL |

## Failures

| Situation | Behaviour |
|---|---|
| No WiFi | Rejoins by itself; the talk button reads "no WiFi" |
| Key missing, or 401/403 | "Deepgram key missing" / "Claude key rejected", on screen; no retry loop |
| 402/429 (credit, rate) | ElevenLabs: the answer shows as text without speech. Claude: one retry after 2 s, then a message |
| Timeouts | Deepgram 15 s, Claude 60 s, ElevenLabs 20 s; then a message, never a hang |
| Internet MCP server failing | Claude answers without it; the screen notes "MCP server unreachable" |
| Silence | "didn't catch that"; no calls |
| Bad token or bad JSON-RPC on its own server | 401, or a JSON-RPC error; logged on the console |
| Hang | Task watchdog (20 s, panic) restarts it |

## Testing

- **Host tests** (`host_tests/`, as for panel1):
  - `mcp_rpc.c`: initialize, tools/list, every tool's argument checking, unknown method, bad JSON, missing token.
  - `claude_msg.c`: request JSON (with and without MCP), parsing of text, `mcp_tool_use`, `mcp_tool_result` and error replies.
  - Deepgram reply parsing.
  - `vad.c` on recorded clips: speech then pause, silence, noise, the 15 s cap.
  - `ui.c` renders to BMP/PNG for review.
- **Live check from the Mac**: a script makes the same three calls with the
  real keys, to confirm request and reply formats before anything is flashed.
- **On hardware**: built for the 4B. Before stan-claw first goes onto
  panel1, panel1's settings are backed up (as done on 2026-10-10:
  `secrets/panel1-nvs.bin`).

## Not in this version

A wake word; streaming speech to text while talking; HTTPS on its own MCP
server; an MCP client on the board for servers only the local network can
reach; music or arbitrary audio playback through MCP; Telegram or other chat
channels.
