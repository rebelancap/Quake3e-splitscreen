#!/bin/sh
# usage: r10-perf-avg.sh <log>: com_speeds averages (msec) per "=== PERF" section, leaving out stall
# frames (any column >= 100 ms: a load / hitch, counted separately) -- r9-perf-avg.sh averaged them in.
sed -E 's/: +/:/g' "$1" | awk '
/=== PERF/ { s = $0; sub(/.*=== PERF /, "", s); if (!(s in seen)) { seen[s] = 1; order[++ns] = s } next }
/^frame:[0-9]+ all:/ {
  stall = 0
  for (i = 2; i <= NF; i++) { split($i, a, ":"); x[a[1]] = a[2] + 0; if (x[a[1]] >= 100) stall = 1 }
  if (stall) { st[s]++; next }
  for (k in x) v[s, k] += x[k]
  n[s]++
}
END { for (j = 1; j <= ns; j++) { s = order[j]; if (!n[s]) continue
  printf "%-8s frames %d (+%d stalls)", s, n[s], st[s]
  split("all sv ev cl gm rf bk", c, " ")
  for (i = 1; i <= 7; i++) printf " %s %.2f", c[i], v[s, c[i]] / n[s]
  printf "\n" } }'
