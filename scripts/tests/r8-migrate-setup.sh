#!/bin/sh
# R8: an R7 interim store (splitpads.cfg) for r8-migrate.cfg.  Argument "keep": leave existing profiles alone.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
H="${Q3HOME:-$REPO/work/q3test}"
[ "$1" = "keep" ] || rm -rf "$H/profiles" "$H/baseq3/profiles"
cat > "$H/baseq3/splitpads.cfg" <<'EOS'
// splitscreen pad binds and controls per player (interim store until profiles); written by the engine
pfeel 1
pset 1 joy_yawSpeed "410"
pset 1 joy_aimCurve "dynamic"
pset 1 joy_crouchToggle "1"
pslot 1
pbind 1 PAD_A "+moveup"
pbind 1 PAD_Y "+attack"
pbind 1 PAD_RT "+attack"
pfeel 2
pset 2 joy_yawSpeed "150"
pslot 2
pbind 2 PAD_A "+attack"
EOS
