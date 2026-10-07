#!/bin/bash
# usage: scripts/tests/l6-r19-run.sh <vk|gl> <cfg> <name> <map|-> [extra args...]
# Linux counterpart of r19-run.sh (R19 Independent-mode pad ownership on the test device bus):
# l3-run.sh (Independent mode on the desktop session, x11 driver, --independent --noactivate,
# homepath work/q3home, SDL off) with the R19 bus on in every process (in_padBus 1:
# work/q3home/padbus.txt, written by the coordinator's 'padbus' commands) and in_padDebug 1.
# Player windows list the bus in REVERSE order (in_padBusOrder 1) and run r19-child.cfg
# (L6_R19_CHILDCFG replaces it).  Evidence (prefix L3_PREFIX, default l6): work/<pfx>-<name>.log,
# -child<N>.log, -padbus.txt (the bus file at the end), and l3-run.sh's -x11/-procs/-q3config files.
#   scripts/tests/l6-r19-run.sh vk r19-pads.cfg r19-pads -
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
H="$REPO/work/q3home"
PFX="${L3_PREFIX:-l6}"
rm -f "$H/padbus.txt" "$H/padbus.txt.tmp"
R="$1"; CFG="$2"; NAME="$3"; MAP="$4"; shift 4
L3_PREFIX="$PFX" \
L3_CHILDARGS="${L3_CHILDARGS:-+set developer 1 +set in_padBus 1 +set in_padBusOrder 1 +set in_padDebug 1 +exec ${L6_R19_CHILDCFG:-r19-child.cfg}}" \
  "$REPO/scripts/tests/l3-run.sh" "$R" "$CFG" "$NAME" "$MAP" +set in_padBus 1 +set in_padDebug 1 "$@"
RC=$?
cp "$H/padbus.txt" "$REPO/work/$PFX-$NAME-padbus.txt" 2>/dev/null
rm -f "$H/padbus.txt" "$H/padbus.txt.tmp"
exit $RC
