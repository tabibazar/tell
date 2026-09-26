# watch, speaker and envo on two Macs (home and work)

The boards travel between home and work, so BOTH Macs get the same setup and
keep it: whichever Mac the boards are plugged into serves them, and the other
simply finds nothing (its clock pushes give up quietly, its relay waits).
The firmware is already on the boards; a Mac only has to keep them fed.
Everything identifies boards by MAC (USB serial number), so port names never
matter.

| Board | MAC | What it needs from a Mac |
|---|---|---|
| watch | 80:45:6b:35:11:d4 | the clock (its RTC backup cell is flat) |
| speaker | 28:84:85:56:f6:b0 | the clock (flat RTC cell -- no SD logging until set); the relay for speech |
| envo | 28:84:85:88:1c:3c | nothing: it has its own DS3231 and logs on its own |

## 1. Get the code

    git checkout main
    git pull

(`envio-camera-maze` holds the same commits; main was fast-forwarded to it.)

## 2. Tools the Mac needs (once)

    mac/build.sh                          # builds mac/tell, the BLE sender (Xcode tools)
    python3 -m pip install --user pyserial   # for the relay (skip if ESP-IDF is installed:
                                             # the relay uses its Python automatically)

The first `tell` run asks for Bluetooth permission for the terminal -- allow it.

## 3. Clock pushes (BLE, every 5 min)

    tools/install-agent.sh watch
    tools/install-agent.sh speaker

Check: `tail /tmp/push-clock.watch.log /tmp/push-clock.speaker.log`.
Keep them on both Macs. The relay (below) also pushes a board's clock the
moment it is plugged in, so a board that lost power on the way is right
within seconds rather than at the next 5-minute push.

## 4. The speaker relay (speaker on this Mac's USB)

    tools/install-agent.sh noise-relay

A background agent: it starts at login, runs all the time, and launchd
restarts it if it ever exits; an unplugged speaker is picked up again when it
comes back. It finds speaker by MAC, pushes her clock the moment she is
plugged in, and carries speech and commands to her (section 6).
speaker has her own screen since 2026-09-26 (Sound and Days, swipe between
them), so watch is no longer fed; `--to-watch` brings that back if ever wanted.
Log: `tail -f /tmp/noise-relay.err`. To run it by hand instead (for
debugging), stop the agent first -- both would want the same USB port:
`launchctl bootout gui/$(id -u)/com.tabibazar.noise-relay`, then
`python3 tools/noise-relay.py --verbose`.

## 5. Flashing at work (only if you change firmware)

Needs ESP-IDF 5.5 (`tools/idf-env.sh`). Each board has its own script, and
each refuses a board whose MAC is not its own:

    tools/flash-watch.sh      tools/flash-speaker.sh      tools/flash-envo.sh

Build first: `idf.py -B build-<board> -D SDKCONFIG=sdkconfig.<board>
-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.<board>" build`.
Factory backups of watch and speaker live only on the home Mac
(`firmware-backup/`, git-ignored); their sha256 are in docs/hardware/.

## 6. Speech, calibration and the baseline (through the relay)

    tools/speak "Dinner is ready"          # refused 21:00-06:00 (quiet hours)
    tools/speak --test "testing"           # plays at any hour
    tools/cal-speaker --status             # levels, days and the baseline

speaker's scale is left estimated on purpose: a check on 2026-09-25 with
stepped pink noise put normal conversation at about 56 dBA and the quiet
room at about 34, both about right, while the phone app read ~35 dB high.
To calibrate against a meter you trust (NIOSH SLM, an Apple Watch):
`tools/cal-speaker` (guided, plays pink noise) or `tools/cal-speaker NN`.
The chime is off by default: `./mac/tell --device speaker '!chime on'`.
