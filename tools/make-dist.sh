#!/bin/sh
# Builds a self-contained tell for another Mac and tars it up.
set -e
cd "$(dirname "$0")/.."

OUT=dist
STAGE="$OUT/tell"
rm -rf "$STAGE"
mkdir -p "$STAGE"

# Universal, so it runs on Apple Silicon and Intel alike.
swiftc -O -target arm64-apple-macos11  mac/tell.swift -o "$OUT/.tell-arm64"
swiftc -O -target x86_64-apple-macos11 mac/tell.swift -o "$OUT/.tell-x86_64"
lipo -create "$OUT/.tell-arm64" "$OUT/.tell-x86_64" -output "$STAGE/tell"
rm -f "$OUT/.tell-arm64" "$OUT/.tell-x86_64"

# Ad-hoc signature: not notarised, but avoids some Gatekeeper complaints.
codesign -s - -f "$STAGE/tell" 2>/dev/null || true

cp tools/dist-README.md "$STAGE/README.md"
tar -czf "$OUT/tell.tar.gz" -C "$OUT" tell

echo "built $OUT/tell.tar.gz"
lipo -archs "$STAGE/tell"
