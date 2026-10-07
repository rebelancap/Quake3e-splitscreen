#!/bin/sh
# usage: r10-sound-count.sh <log>: per "=== SOUND" section: sound starts (s_show 1 lines "<time> : <sfx>"),
# "played once" de-dups, and the s_show 2 active channels ("----(N)---- painted") avg / max per frame
awk '
/=== SOUND/ { s = $0; sub(/.*=== SOUND /, "", s); order[++ns] = s; next }
/^[0-9]+ : / { starts[s]++ }
/played once/ { dup[s]++ }
/^----\([0-9]+\)---- painted/ { v = $0; sub(/^----\(/, "", v); sub(/\).*/, "", v); v = v + 0; tot[s] += v; n[s]++; if (v > mx[s]) mx[s] = v }
END { for (j = 1; j <= ns; j++) { s = order[j]; if (s == "end") continue
  printf "%-22s starts %5d  de-dups %5d", s, starts[s], dup[s]
  if (n[s]) printf "  channels avg %.1f max %d (%d frames)", tot[s] / n[s], mx[s], n[s]
  printf "\n" } }' "$1"
