#!/bin/sh
# usage: scripts/tests/r12-table.sh <log>
# Summarises the "AA P1 ..." lines of an r12-aim.cfg run per level and section:
#  A: look-stick yaw rate outside the bubble vs inside (min / mean |yaw| over frames with a target)
#  B: the largest |rot| and |output yaw| while P1 gives no input (both must be 0)
#  C/D: frames with a target and rotation: mean target angular velocity (omega yaw), mean rotational
#       yaw, mean output yaw (stick 1.25 deg/s in C, 0 in D)
#  E: frames marked (flick) and their span; F: traces made without a target (no line of sight)
awk '
function abs(x) { return x < 0 ? -x : x }
function field(name,   i) { for (i = 1; i <= NF; i++) if ($i == name) return $(i+1); return "" }
function flush() {
  if (sec == "") return
  if (sec == "A") printf "%-9s A sweep   stick %6.2f | outside %6.2f | in bubble %2d frames: min %6.2f mean %6.2f deg/s (slow min %.3f)\n", lvl, stick, outside, n, (n ? mn : 0), (n ? sum / n : 0), slowmin
  if (sec == "B") printf "%-9s B idle    frames with target %2d | max |rot| %.2f | max |output yaw| %.2f deg/s\n", lvl, n, maxrot, maxout
  if (sec == "C" || sec == "D") printf "%-9s %s %-6s frames rot>0 %2d | mean omega %7.2f | mean rot %6.2f | mean output %6.2f deg/s (target frames %d, max |omega| %.1f, max |rot| %.2f)\n", lvl, sec, (sec == "C" ? "track" : "strafe"), nr, (nr ? om / nr : 0), (nr ? rs / nr : 0), (nr ? os / nr : 0), n, maxom, maxrot
  if (sec == "E") printf "%-9s E flick   frames (flick) %d over %d ms | rot during flick max %.2f\n", lvl, nf, (nf ? tl - tf : 0), maxrot
  if (sec == "F") printf "%-9s F wall    frames %d, traces made %d, frames with a target %d\n", lvl, nall, tr, n
  sec = ""
}
/=== LEVEL/ { flush(); lvl = ($3 == "0") ? "Off" : ($3 == "1") ? "Low" : "Standard"; next }
/=== TEST/ {
  flush(); s = $3
  if (s ~ /^[ABCDEF]$/) { sec = s; n = nr = nf = nall = tr = 0; sum = om = rs = os = maxrot = maxout = maxom = 0; mn = 1e9; slowmin = 1; outside = ""; stick = 0 }
  if (s == "E") lvl = "Standard"
  next
}
/^AA P1 lvl/ && sec != "" {
  tgt = field("tgt"); st = field("stick"); out = field("yaw"); sl = field("slow")
  r = $0; sub(/.* rot /, "", r); split(r, rr, " "); ry = rr[1]
  o = $0; sub(/.* omega /, "", o); split(o, oo, " "); oy = oo[1]
  nall++; tr += field("traces")
  if (sec == "A" && abs(st) == 0) next
  if (sec == "A") { stick = st; if (tgt == -1 && outside == "") outside = out
    if (tgt != -1) { n++; sum += abs(out); if (abs(out) < mn) mn = abs(out); if (sl < slowmin) slowmin = sl } }
  if (sec == "B") { if (tgt != -1) n++; if (abs(ry) > maxrot) maxrot = abs(ry); if (abs(out) > maxout) maxout = abs(out) }
  if (sec == "C" || sec == "D") { if (tgt != -1) n++; if (abs(ry) > maxrot) maxrot = abs(ry)
    if (tgt != -1 && abs(oy) > maxom) maxom = abs(oy)
    if (tgt != -1 && abs(ry) > 0) { nr++; om += oy; rs += ry; os += out } }
  if (sec == "E") { if ($0 ~ /\(flick\)/) { t = field("t"); if (!nf) tf = t; tl = t; nf++; if (abs(ry) > maxrot) maxrot = abs(ry) } }
  if (sec == "F") { if (tgt != -1) n++ }
}
END { flush() }
' "$1"
