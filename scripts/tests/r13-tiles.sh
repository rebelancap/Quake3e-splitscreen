#!/bin/sh
# usage: scripts/tests/r13-tiles.sh
# R13 tiling test: r13-run.sh r13-tiles.cfg at the main menu, children run r13-tchild<N>.cfg.  When the
# coordinator's log (logfile 4: flushed) shows "=== TEST taskkill P3", P3's process is killed with
# taskkill /F (its pid from "indep: P3 window starting: pid N"); evidence work/r13-tiles-taskkill.txt.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
LOG="${Q3HOME:-$REPO/work/q3test}/baseq3/qconsole.log"
OUT="$REPO/work/r13-tiles-taskkill.txt"
: > "$OUT"
R13_CHILDARGS="+set developer 1 +exec r13-tchild@@.cfg" sh "$REPO/scripts/tests/r13-run.sh" r13-tiles.cfg tiles - &
RUN=$!
sleep 5
for i in $(seq 1 240); do
  if grep -q "=== TEST taskkill P3" "$LOG" 2>/dev/null; then
    PID=$(grep "indep: P3 window starting: pid" "$LOG" | tail -1 | sed 's/.*pid \([0-9]*\),.*/\1/')
    echo "$(date +%T) taskkill /F /PID $PID (P3's window)" >> "$OUT"
    taskkill //F //PID "$PID" >> "$OUT" 2>&1
    break
  fi
  sleep 0.5
done
wait $RUN
