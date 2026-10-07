#!/bin/bash
# usage: scripts/tests/l3-two.sh [vk|gl]
# L3 2-window test (Linux r13-two.sh): fresh profiles (only "Ada"), then l3-run.sh r13-two.cfg on q3dm7;
# P2's window runs l3-child2.cfg (CD KEY page accepted, then r13-child2.cfg).  Afterwards the profile
# files the run wrote are listed (work/l3-two[-gl]-profiles.txt) and the test profiles removed.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
R="${1:-vk}"; NAME="two"; [ "$R" = gl ] && NAME="two-gl"
H="$REPO/work/q3home"
rm -rf "$H/profiles" "$H/baseq3/profiles"
mkdir -p "$H/profiles"
printf '// test profile\nname "Ada"\n' > "$H/profiles/ada.cfg"
touch -d '2026-01-01 00:00' "$H/profiles/ada.cfg"
L3_CHILDARGS="+set developer 1 +exec l3-child@@.cfg" "$REPO/scripts/tests/l3-run.sh" "$R" r13-two.cfg "$NAME" q3dm7
{ echo "# profile files after the run (mtime, path)"; find "$H/profiles" "$H/baseq3/profiles" -type f -printf '%TY-%Tm-%Td %TH:%TM:%TS %p\n' 2>/dev/null; } > "$REPO/work/${L3_PREFIX:-l3}-$NAME-profiles.txt"
rm -rf "$H/profiles" "$H/baseq3/profiles"
