#!/bin/bash
# usage: scripts/tests/l3-ckill.sh [vk|gl]
# L3 (Linux r13-ckill.sh): the coordinator (P1's process) is killed with SIGKILL while P2's and P3's
# windows run; every child must be gone within ~1 s (PR_SET_PDEATHSIG -> SIGTERM; they also watch
# the coordinator's pid).  Evidence: work/l3-ckill-kill.txt (processes before / 0.25 s steps after).
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
R="${1:-vk}"
Q3PAT='^quake3e-(vulkan-|ss\.x64$)'   # our clients (process names are cut to 15 characters)
LOG="$REPO/work/q3home/baseq3/qconsole.log"
OUT="$REPO/work/${L3_PREFIX:-l3}-ckill-kill.txt"
: > "$OUT"
L3_CHILDARGS="+set developer 1" "$REPO/scripts/tests/l3-run.sh" "$R" r13-ckill.cfg ckill - &
RUN=$!
sleep 5
for i in $(seq 1 480); do
  if grep -q "=== TEST kill the coordinator now" "$LOG" 2>/dev/null; then
    PID=$(grep "(this process: pid" "$LOG" | tail -1 | sed 's/.*pid \([0-9]*\)).*/\1/')
    { echo "$(date +%T.%N | cut -c1-12) before:"; pgrep -a "$Q3PAT" | cut -c1-80; echo "$(date +%T.%N | cut -c1-12) kill -9 $PID (the coordinator)"; } >> "$OUT"
    kill -9 "$PID" >> "$OUT" 2>&1
    for t in 1 2 3 4 5 6 7 8 9 10 11 12; do
      sleep 0.25; { echo "$(date +%T.%N | cut -c1-12) +$((t*250)) ms: $(pgrep -c "$Q3PAT") process(es)"; pgrep -a "$Q3PAT" | cut -c1-80; } >> "$OUT"
    done
    echo "(end)" >> "$OUT"
    break
  fi
  sleep 0.5
done
wait $RUN
