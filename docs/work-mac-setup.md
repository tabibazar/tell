# Moving watch, speaker and envo to the work Mac

The firmware is already on the boards; the work Mac only has to keep them fed.
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

## 2. Clock pushes (BLE, every 5 min)

    tools/install-agent.sh watch
    tools/install-agent.sh speaker

Check: `tail /tmp/push-clock.watch.log /tmp/push-clock.speaker.log`.
Once these run at work, the same agents on the home Mac can be removed:
`launchctl bootout gui/$(id -u)/com.tabibazar.push-clock.watch` (and `.speaker`).

## 3. speaker's level on watch (both on this Mac's USB)

    python3 tools/noise-relay.py      # see its --help and --self-test

It finds both boards by MAC and forwards speaker's level to watch's Sound
page (side button: Face -> Sand -> Sound).

## 4. Flashing at work (only if you change firmware)

Needs ESP-IDF 5.5 (`tools/idf-env.sh`). Each board has its own script, and
each refuses a board whose MAC is not its own:

    tools/flash-watch.sh      tools/flash-speaker.sh      tools/flash-envo.sh

Build first: `idf.py -B build-<board> -D SDKCONFIG=sdkconfig.<board>
-D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.<board>" build`.
Factory backups of watch and speaker live only on the home Mac
(`firmware-backup/`, git-ignored); their sha256 are in docs/hardware/.

## 5. Calibrate speaker once it is in place

With a phone sound-meter app beside it in a steady sound:
`./mac/tell --device speaker '!cal NN'` (NN = the phone's dBA).
The chime is off by default: `./mac/tell --device speaker '!chime on'`.
