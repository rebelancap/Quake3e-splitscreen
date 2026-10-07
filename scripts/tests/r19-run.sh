#!/bin/sh
# usage: scripts/tests/r19-run.sh <cfg> <name> <map|-> [extra args...]
# R19 (Independent mode pad ownership) runner = r13-run.sh (Independent mode, every window inside
# 64,64 1280x720, --noactivate, homepath work/q3test, SDL off) with the R19 test device bus on in
# every process (in_padBus 1: <homepath>/padbus.txt, written by the coordinator's 'padbus'
# commands) and in_padDebug 1.  Player windows list the bus in REVERSE order (in_padBusOrder 1:
# SDL's device order differs between processes) and run r19-child.cfg.  Evidence:
# work/r19-<name>.log, work/r19-<name>-child<N>.log, shots work/r19-<name>-*.jpg, tasklist.
# R19_CHILDCFG replaces r19-child.cfg; the other R13_* variables work as in r13-run.sh.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
H="${R13_HOME:-${Q3HOME:-$REPO/work/q3test}}"
rm -f "$H/padbus.txt" "$H/padbus.txt.tmp"
R13_PREFIX="${R13_PREFIX:-r19}" \
R13_CHILDARGS="${R13_CHILDARGS:-+set developer 1 +set in_padBus 1 +set in_padBusOrder 1 +set in_padDebug 1 +exec ${R19_CHILDCFG:-r19-child.cfg}}" \
  sh "$REPO/scripts/tests/r13-run.sh" "$1" "$2" "$3" +set in_padBus 1 +set in_padDebug 1 $(shift 3; echo "$@")
cp "$H/padbus.txt" "$REPO/work/${R13_PREFIX:-r19}-$2-padbus.txt" 2>/dev/null
rm -f "$H/padbus.txt" "$H/padbus.txt.tmp"
