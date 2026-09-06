#!/bin/sh
set -e
cd "$(dirname "$0")"
swiftc -O tell.swift -o tell
echo "built $(pwd)/tell"
