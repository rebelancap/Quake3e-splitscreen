#!/bin/sh
# usage: r9-perf-avg.sh <log>: average com_speeds columns (msec) per "=== PERF" section
sed -E 's/: +/:/g' "$1" | awk '/=== PERF/{s=$0} /^frame:[0-9]+ all:/{ for(i=2;i<=NF;i++){split($i,a,":"); v[s,a[1]]+=a[2]} n[s]++ } END{ for(k in n){ printf "%s frames %d", k, n[k]; split("all sv ev cl gm rf bk",c," "); for(j=1;j<=7;j++) printf " %s %.2f", c[j], v[k,c[j]]/n[k]; printf "\n" } }' | sort
