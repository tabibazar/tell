# tell

Writes text to the ESP32 desk display over Bluetooth LE.

The boards are already flashed and need no pairing or setup — this single binary
is everything required, on any Mac. Nothing is tied to a particular machine.

## Install

```sh
xattr -dr com.apple.quarantine tell     # only if copied via download/AirDrop
sudo mv tell /usr/local/bin/
```

macOS will ask for Bluetooth permission the first time you run it. Grant it to
your terminal app — the request comes from Terminal or iTerm, not from
`tell` itself. Without it you get:

```
tell: Bluetooth permission denied -- grant it to your terminal in
System Settings > Privacy & Security > Bluetooth
```

## Use

```sh
tell "hello"                 # show text
echo "hello" | tell          # from stdin, so you can pipe anything
date | tell                  # e.g. pipe a command's output
tell ""                      # clear, returning to the clock
tell --sync                  # sync the clock, leave the display alone
```

Longer text wraps at word boundaries; past the last line it is truncated with a
visible `...`, so nothing is silently dropped. Newlines force line breaks.
Non-ASCII characters show as `?`.

When no message is showing, the board displays a clock. A message holds the
screen for 30 seconds, then the clock returns.

## The clock

The board has no battery-backed clock, so it starts at `--:--:--` after every
power cycle and drifts a few seconds a day. Every command re-syncs it, so in
normal use this is invisible. `tell --sync` fixes it without changing what
is on screen.

Time is sent as seconds since *your* local midnight, so the board never needs to
know a timezone — it simply shows whatever your Mac's clock says.

## Requirements

macOS 11 or later, Apple Silicon or Intel. No runtime dependencies: it links
only against system frameworks and the Swift runtime that ships with macOS.

## Troubleshooting

`no device named X found within 10s` — check the spelling with `tell --list`.
Otherwise the board is unpowered, out of range, or already connected to another
machine. Only one Mac can hold a connection at a time, though each message
holds it for barely a second, so several Macs can share the boards in practice.
