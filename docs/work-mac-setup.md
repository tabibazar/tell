# watch, speaker and envo on two Macs (home and work)

The boards travel between home and work, so BOTH Macs get the same setup and
keep it: whichever Mac the boards are plugged into serves them, and the other
simply finds nothing (its clock pushes give up quietly, its relay waits).
The firmware is already on the boards; a Mac only has to keep them fed.
Everything identifies boards by MAC (USB serial number), so port names never
matter.

| Board | MAC | What it needs from a Mac |
|---|---|---|
| watch | 80:45:6b:35:11:d4 | the clock (its RTC backup cell is flat); the noise relay for its Sound page |
| speaker | 28:84:85:56:f6:b0 | the clock (flat RTC cell -- no SD logging until set) |
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

## 4. speaker's level on watch (both on this Mac's USB)

    tools/install-agent.sh noise-relay

A background agent: it starts at login, runs all the time, and launchd
restarts it if it ever exits; unplugged boards are picked up again when they
come back. It finds both boards by MAC and forwards speaker's level to
watch's Sound page (side button: Face -> Sand -> Sound).
Log: `tail -f /tmp/noise-relay.log`. To run it by hand instead (for
debugging), stop the agent first -- both would want the same USB ports:
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

## 6. Calibrate speaker once it is in place

With a phone sound-meter app beside it in a steady sound:
`./mac/tell --device speaker '!cal NN'` (NN = the phone's dBA).
The chime is off by default: `./mac/tell --device speaker '!chime on'`.
