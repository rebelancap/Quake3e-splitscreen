#!/bin/sh
# usage: r10-hunk-sum.sh <log>: per "=== HUNK" marker the meminfo low permanent and total hunk in use
# (of the total), plus any refused join / hunk shortage lines
awk '
/=== HUNK/ { s = $0; sub(/.*=== HUNK /, "", s); next }
/bytes total hunk/ { tot = $1 }
/low permanent/ { lp = $1 }
/total hunk in use/ { if (s != "") { printf "%-22s low permanent %10d  in use %10d of %d (free %.1f MB)\n", s, lp, $1, tot, (tot - $1) / 1048576; s = "" } }
/not enough hunk|refused|Hunk_Alloc failed/ { print "   !! " $0 }' "$1"
