#!/bin/sh
set -e
cd "$(dirname "$0")"
swiftc -O esp32-say.swift -o esp32-say
echo "built $(pwd)/esp32-say"
