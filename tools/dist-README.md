# tell

Writes text to the ESP32 desk display over Bluetooth LE.

The board is already flashed and needs no pairing or setup — this single binary
is everything required. It talks to whichever board is in range advertising the
service, so nothing is tied to a particular Mac.

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

The display is **20 characters wide by 5 lines**. Longer text wraps; past five
lines it is truncated with a visible `...`. Newlines force line breaks.
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

`no ESP32-Screen found within 10s` — the board is unpowered, out of range, or
already connected to another machine. Only one Mac can be connected at a time,
though the connection only lasts a second or so per message.
