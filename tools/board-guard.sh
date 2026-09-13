#!/bin/sh
# Refuse to write one board's firmware to another.
#
# lilly and wave are both ESP32-S3 with native USB-Serial-JTAG, so both show up
# as /dev/cu.usbmodem* and whichever was plugged in first takes the lower
# number. The port is therefore no evidence at all about which board is on the
# other end of it, and the firmwares are not interchangeable: different panel
# driver, different pins, different partition table. It has already happened
# once in this project.
#
# So: read the MAC, and compare it against tools/boards.tsv.
#
#   the expected board                  flash
#   anything else, when we know the
#     expected board's MAC               REFUSE, naming it if we know it
#   any board, when the expected one
#     has no MAC on record               proceed, and say how to record it
#   a different KNOWN board, always      REFUSE
#   unreadable                           REFUSE, unless --any
#
# --any covers exactly one case: esptool would not tell us what is there. It
# deliberately does NOT override a MAC that was read and did not match, because
# there is no good reason to write one board's firmware to another and an
# override that allows it is a foot-gun that will eventually be used by
# accident -- as it was, within a minute of being written, onto lilly. A board
# that has genuinely been replaced gets a new line in boards.tsv, which is the
# durable fix rather than a flag on one command.
#
# Usage:  board_guard <name> <port> [--any]
# Sourced, not run: it needs to exit the calling script.

board_mac() {           # port -> MAC on stdout, empty if it could not be read
    _port="$1"
    # esptool occasionally fails to sync when called twice in quick
    # succession, so give it a few goes before believing the board is mute.
    _i=0
    while [ "$_i" -lt 3 ]; do
        _mac=$("$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python" \
               "$HOME/esp/esp-idf/components/esptool_py/esptool/esptool.py" \
               --port "$_port" read_mac 2>/dev/null \
               | grep -m1 '^MAC:' | awk '{print $2}')
        [ -n "$_mac" ] && { echo "$_mac"; return 0; }
        _i=$((_i + 1))
        sleep 2
    done
    return 1
}

board_guard() {         # name, port, [--any]
    _want_name="$1"
    _port="$2"
    _any="$3"

    # The flash scripts cd to the repository root before sourcing this.
    _registry="tools/boards.tsv"
    _want_mac=$(grep -v '^#' "$_registry" 2>/dev/null \
                | awk -F'\t' -v n="$_want_name" '$1 == n { print $2 }')

    _mac=$(board_mac "$_port")
    if [ -z "$_mac" ]; then
        if [ "$_any" = "--any" ]; then
            echo "warning: could not read the MAC on $_port; flashing anyway (--any)"
            return 0
        fi
        echo "cannot read the MAC on $_port, so cannot tell which board this is." >&2
        echo "Unplug it, plug it back in, and try again -- or pass --any to skip" >&2
        echo "this check if you are certain it is $_want_name." >&2
        exit 1
    fi

    if [ -n "$_want_mac" ] && [ "$_mac" = "$_want_mac" ]; then
        echo "$_want_name confirmed on $_port ($_mac)"
        return 0
    fi

    # Whatever is there, is it something we know?
    _is=$(grep -v '^#' "$_registry" 2>/dev/null \
          | awk -F'\t' -v m="$_mac" '$2 == m { print $1 }')
    if [ -n "$_is" ]; then
        echo "REFUSING: $_port has $_is on it ($_mac), not $_want_name." >&2
        echo "These firmwares are not interchangeable -- different panel," >&2
        echo "different pins, different partition table. Plug in $_want_name," >&2
        echo "or run tools/flash-$_is.sh to flash the board that is actually there." >&2
        exit 1
    fi

    if [ -z "$_want_mac" ]; then
        # Nothing on record for this board, so there is nothing to contradict.
        echo "note: $_want_name has no MAC on record, and $_port has $_mac."
        echo "      Add this line to tools/boards.tsv to have it checked next time:"
        printf '        %s\t%s\twhat it is\n' "$_want_name" "$_mac"
        return 0
    fi

    # We know what this board should be, and this is not it.
    echo "REFUSING: $_want_name is $_want_mac, but $_port has $_mac on it," >&2
    echo "which is not a board this repository knows. If $_want_name has been" >&2
    echo "replaced, add its new MAC to tools/boards.tsv -- there is no flag for" >&2
    echo "this, because the registry is the thing that should be right." >&2
    exit 1
}
