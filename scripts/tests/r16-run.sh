#!/bin/sh
# usage: scripts/tests/r16-run.sh <cfg> <name> <map|-> [extra args...]
# R16 (Server options rows, instagib, bots 20, FOV): s6-run.sh with the Vulkan Release exe (R16_GL=1: OpenGL), test home
# work/q3test (Q3HOME overrides), developer, logfile 3, hunk 128, 8 players, sv_maxclients 16.
# Log -> work/r16-<name>.log; screenshots the script took as "r16-<x>" -> work/r16-<x>.jpg.
# Refuses to start while any quake3e process runs (one game instance at a time).
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
CFG="$1"; NAME="$2"; MAP="$3"; shift 3
EXE="${EXE_DIR:-$REPO/build/Release}/quake3e-vulkan-ss.x64.exe"
[ -n "$R16_GL" ] && EXE="${EXE_DIR:-$REPO/build/Release}/quake3e-ss.x64.exe"
if tasklist 2>/dev/null | grep -i "quake3e" >/dev/null; then
  echo "r16-run: a quake3e process is running; not starting" >&2
  tasklist | grep -i quake3e >&2
  exit 1
fi
SHOTS="${Q3HOME:-$REPO/work/q3test}/${GAME:-baseq3}/screenshots"
rm -f "$SHOTS"/r16-*.jpg
PRE_ARGS="${PRE_ARGS:---noactivate}" "$REPO/scripts/tests/s6-run.sh" "$EXE" "$CFG" "r16-$NAME" "$MAP" +set in_gamepad 0 \
  +set developer 1 +set logfile 3 +set com_hunkMegs 128 +set cl_splitMaxPlayers 8 +set sv_maxclients 16 \
  +set vid_xpos 64 +set vid_ypos 64 "$@"
for f in "$SHOTS"/r16-*.jpg; do
  [ -f "$f" ] || continue
  mv "$f" "$REPO/work/$(basename "$f")"
done
tasklist 2>/dev/null | grep -i quake3e && echo "r16-run: WARNING quake3e still running" >&2
exit 0
