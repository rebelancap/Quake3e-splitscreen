#!/bin/sh
# usage: scripts/tests/r13-two.sh [name]   (R13_GL=1: OpenGL exe; name default "two", e.g. "two-gl")
# R13 2-window test: fresh profiles (only "Ada"), then r13-run.sh r13-two.cfg on q3dm7.  Afterwards
# the profile files the run wrote are listed (the child may write only profiles/ada.cfg +
# baseq3/profiles/ada.cfg; _padlast.cfg comes from the coordinator) and the test profiles removed.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAME="${1:-two}"
H="${Q3HOME:-$REPO/work/q3test}"
rm -rf "$H/profiles" "$H/baseq3/profiles"
mkdir -p "$H/profiles"
printf '// test profile\nname "Ada"\n' > "$H/profiles/ada.cfg"
touch -d '2026-01-01 00:00' "$H/profiles/ada.cfg"
sh "$REPO/scripts/tests/r13-run.sh" r13-two.cfg "$NAME" q3dm7
{ echo "# profile files after the run (mtime, path)"; find "$H/profiles" "$H/baseq3/profiles" -type f -printf '%TY-%Tm-%Td %TH:%TM:%TS %p\n' 2>/dev/null; } > "$REPO/work/${R13_PREFIX:-r13}-$NAME-profiles.txt"
rm -rf "$H/profiles" "$H/baseq3/profiles"
