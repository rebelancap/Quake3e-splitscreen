#!/bin/sh
# usage: r10-caps-max.sh <log>: per "=== CAPS" section, frames and the largest value of each
# "frame:" field (r_speeds 1 with the R10 renderer), shown as max/capacity.
sed -E 's/\^[0-9]//g' "$1" | awk '
/=== CAPS/ { s = $0; sub(/.*=== CAPS /, "", s); if (!(s in seen)) { seen[s] = 1; order[++ns] = s } next }
/^frame: [0-9]+ scenes/ {
  n[s]++
  line = $0; sub(/^frame: /, "", line)
  k = split(line, f, ", ")
  for (i = 1; i <= k; i++) {
    if (f[i] ~ /scenes/) { split(f[i], a, " "); v = a[1] + 0; name = "scenes"; cap = "" }
    else { split(f[i], a, " "); name = a[1]; split(a[2], b, "/"); v = b[1] + 0; cap = b[2] }
    if (!((s, name) in mx) || v > mx[s, name]) mx[s, name] = v
    capv[s, name] = cap; names[i] = name; nn = k
  }
}
END {
  for (j = 1; j <= ns; j++) { s = order[j]; if (!n[s]) continue
    printf "%-22s frames %4d:", s, n[s]
    for (i = 1; i <= nn; i++) printf " %s %s%s", names[i], mx[s, names[i]], (capv[s, names[i]] != "" ? "/" capv[s, names[i]] : "")
    printf "\n" }
}'
