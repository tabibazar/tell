#!/bin/sh
# Serialise access to the board: it accepts one BLE connection at a time, and
# three agents plus hand-run commands otherwise collide and silently drop
# updates ("no device named ... found within 10s").
#
# Takes the same arguments as mac/tell and passes stdin through.
cd "$(dirname "$0")/.."

LOCK="/tmp/tell-ble.lock"
STALE=60

# Break a lock left behind by a killed process.
if [ -d "$LOCK" ]; then
    age=$(( $(date +%s) - $(stat -f %m "$LOCK" 2>/dev/null || echo 0) ))
    [ "$age" -gt "$STALE" ] && rmdir "$LOCK" 2>/dev/null
fi

# Wait for our turn. A send takes ~2s, so 30 tries at 1s is ample.
i=0
while ! mkdir "$LOCK" 2>/dev/null; do
    i=$((i + 1))
    if [ "$i" -ge 30 ]; then
        echo "tell-locked: board busy, giving up" >&2
        exit 0        # a dropped update is not an error; the next tick retries
    fi
    sleep 1
done
trap 'rmdir "$LOCK" 2>/dev/null' EXIT

./mac/tell "$@"
