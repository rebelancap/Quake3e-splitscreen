#!/bin/bash
# usage: scripts/tests/l3-tiles.sh [vk|gl]
# L3 tiling test (Linux r13-tiles.sh): l3-run.sh r13-tiles.cfg at the main menu, children run
# r13-tchild<N>.cfg.  When the coordinator's log shows "=== TEST taskkill P3", P3's process is
# killed with SIGKILL (its pid from "indep: P3 window starting: pid N"); evidence
# work/l3-tiles-kill.txt.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
R="${1:-vk}"
NAME="tiles"; [ "$R" = gl ] && NAME="tiles-gl"
Q3PAT='^quake3e-(vulkan-|ss\.x64$)'   # our clients (process names are cut to 15 characters)
LOG="$REPO/work/q3home/baseq3/qconsole.log"
OUT="$REPO/work/${L3_PREFIX:-l3}-$NAME-kill.txt"
: > "$OUT"
L3_CHILDARGS="+set developer 1 +exec r13-tchild@@.cfg" "$REPO/scripts/tests/l3-run.sh" "$R" r13-tiles.cfg "$NAME" - &
RUN=$!
sleep 5
for i in $(seq 1 480); do
  if grep -q "=== TEST taskkill P3" "$LOG" 2>/dev/null; then
    PID=$(grep "indep: P3 window starting: pid" "$LOG" | tail -1 | sed 's/.*pid \([0-9]*\),.*/\1/')
    { echo "$(date +%T.%N | cut -c1-12) before:"; pgrep -a "$Q3PAT" | cut -c1-80; echo "$(date +%T.%N | cut -c1-12) kill -9 $PID (P3's window)"; } >> "$OUT"
    kill -9 "$PID" >> "$OUT" 2>&1
    sleep 1; { echo "$(date +%T.%N | cut -c1-12) 1 s after:"; pgrep -a "$Q3PAT" | cut -c1-80; } >> "$OUT"
    break
  fi
  sleep 0.5
done
wait $RUN
