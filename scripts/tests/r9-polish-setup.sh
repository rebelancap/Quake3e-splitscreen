#!/bin/sh
# R9 polish: clean profiles plus a hand-edited alice.cfg whose name line says "Bob" (run before r9-polish.cfg).
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
H="${Q3HOME:-$REPO/work/q3test}"
rm -rf "$H/profiles" "$H/baseq3/profiles" "$H/missionpack/profiles"
mkdir -p "$H/profiles"
printf '%s\n' 'name "Bob"' 'joy_yawSpeed "333"' > "$H/profiles/alice.cfg"
grep -E '^seta (name|model) ' "$H/baseq3/q3config-ss.cfg"
