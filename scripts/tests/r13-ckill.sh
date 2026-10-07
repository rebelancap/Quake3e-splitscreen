#!/bin/sh
# usage: scripts/tests/r13-ckill.sh
# R13: the coordinator (P1's process) is killed with taskkill /F while P2's and P3's windows run; every
# child must be gone within a few seconds (kill-on-close job; the children also watch the coordinator's
# pid).  Evidence: work/r13-ckill-taskkill.txt (tasklist before / 1 s / 3 s after the kill).
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
LOG="${Q3HOME:-$REPO/work/q3test}/baseq3/qconsole.log"
OUT="$REPO/work/${R13_PREFIX:-r13}-ckill-taskkill.txt"
: > "$OUT"
R13_CHILDARGS="+set developer 1" sh "$REPO/scripts/tests/r13-run.sh" r13-ckill.cfg ckill - &
RUN=$!
sleep 5
for i in $(seq 1 240); do
  if grep -q "=== TEST kill the coordinator now" "$LOG" 2>/dev/null; then
    PID=$(grep "(this process: pid" "$LOG" | tail -1 | sed 's/.*pid \([0-9]*\)).*/\1/')
    { echo "$(date +%T) before:"; tasklist | grep -i quake3e; echo "$(date +%T) taskkill /F /PID $PID (the coordinator)"; } >> "$OUT"
    taskkill //F //PID "$PID" >> "$OUT" 2>&1
    sleep 1; { echo "$(date +%T) 1 s after:"; tasklist | grep -i quake3e; } >> "$OUT"
    sleep 2; { echo "$(date +%T) 3 s after:"; tasklist | grep -i quake3e; echo "(end)"; } >> "$OUT"
    break
  fi
  sleep 0.5
done
wait $RUN
