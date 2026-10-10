# stan-claw

A tap-to-talk assistant on a Waveshare ESP32-S3-Touch-LCD-4B: you speak,
Deepgram transcribes, Claude answers (with a remote MCP server's tools when
one is set), ElevenLabs speaks. It is also an MCP server on the local
network for its speaker, mics and screen. Design:
`docs/superpowers/specs/2026-10-10-stan-claw-design.md`.

## Build

    . tools/idf-env.sh && cd stan-claw && idf.py build

## Put it on panel1's board (when decided)

1. Back up panel1's settings: `esptool read_flash 0x9000 0x6000 secrets/panel1-nvs.bin`.
2. `tools/flash-stan-claw.sh --full`. stan-claw takes panel1's WiFi from the NVS left in place.
3. Keys: `tools/stan-claw.py key claude ...`, `key deepgram ...`, `key elevenlabs ...`, `voice <id>`,
   `mcp url https://...`, `mcp token ...`, `serve token <long random>`; check with `tools/stan-claw.py status`.
4. Hardware check: `tools/stan-claw.py beep`, then `tools/stan-claw.py rec` while speaking.

Back to room bookings: `cd panel1 && idf.py build && tools/flash-panel1.sh --full`
(its settings come back from NVS, or `esptool write_flash 0x9000 secrets/panel1-nvs.bin`).

## Use it from Claude Code

    claude mcp add --transport http stan-claw http://stan-claw.local/mcp \
        --header "Authorization: Bearer <serve token>"

Tools: speak, listen, ask, show_text, show_image, clear_screen, set_volume, status.
The red LISTENING banner shows whenever the mics are open.

## Check the services from the Mac

    python3 tools/stan-claw-live.py "What can you do?"
