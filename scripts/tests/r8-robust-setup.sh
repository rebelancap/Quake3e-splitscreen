#!/bin/sh
# R8 robustness: hand-corrupted profile files for r8-robust.cfg (run before it).
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
H="${Q3HOME:-$REPO/work/q3test}"
rm -rf "$H/profiles" "$H/baseq3/profiles"
mkdir -p "$H/profiles" "$H/baseq3/profiles"
LONG=$(printf 'x%.0s' $(seq 1 700))
printf '%s\n' '// hand-corrupted' 'name "Junk"' 'joy_yawSpeed "abc"' 'joy_pitchSpeed "99999"' \
  'joy_aimCurve "wobbly"' 'joy_invertPitch "1"' "joy_turnBoost \"$LONG\"" "$LONG" 'garbage line with "unbalanced quote' \
  '=====' 'sv_cheats "1"' 'joy_deadzone' > "$H/profiles/junk.cfg"
printf '\001\002\377binary\r\n' >> "$H/profiles/junk.cfg"
printf '%s\n' 'bind PAD_NOPE "+attack"' 'bind PAD_X' 'bind PAD_A "+attack"' 'userinfo model' \
  'userinfo bad\key "x"' 'userinfo model "doom;quit"' 'userinfo color1 "7"' 'quit' "$LONG" > "$H/baseq3/profiles/junk.cfg"
printf '%s\n' 'name "Odd"' > "$H/profiles/Odd Name!.cfg"
printf '%s\n' 'pad "guid-pad1" "junk"' 'pad' 'nonsense "a" "b"' > "$H/profiles/_padlast.cfg"
