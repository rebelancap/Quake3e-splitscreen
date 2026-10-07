#!/bin/sh
# usage: scripts/tests/r10-run.sh <vk|gl> <cfg> <name> <map|-> [extra args...]
# Runs s6-run.sh with the Vulkan (vk) or OpenGL (gl) Release exe at the default hunk
# size and 8 local players allowed, log -> work/r10-<r>-<name>.log; screenshots the
# script took as "r10-<x>" are moved to work/r10-<r>-<x>.jpg.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
R="$1"; CFG="$2"; NAME="$3"; MAP="$4"; shift 4
EXE="${EXE_DIR:-$REPO/build/Release}/quake3e-vulkan-ss.x64.exe"
[ "$R" = "gl" ] && EXE="${EXE_DIR:-$REPO/build/Release}/quake3e-ss.x64.exe"
SHOTS="${Q3HOME:-$REPO/work/q3test}/baseq3/screenshots"
rm -f "$SHOTS"/r10-*.jpg
"$REPO/scripts/tests/s6-run.sh" "$EXE" "$CFG" "r10-$R-$NAME" "$MAP" +set in_gamepad 0 +set developer 1 \
  +set logfile 3 +set com_hunkMegs 128 +set cl_splitMaxPlayers 8 +set sv_maxclients 16 "$@"
for f in "$SHOTS"/r10-*.jpg; do
  [ -f "$f" ] || continue
  b="$(basename "$f")"
  mv "$f" "$REPO/work/r10-$R-${b#r10-}"
done
