#!/bin/sh
# usage: scripts/tests/r12-run.sh <q3|urt> <cfg> <name> <map|-> [extra args...]
# R12 (aim assist) runner, Vulkan Release exe (GL with R12_GL=1).  q3: the maintainer's Quake 3 folder
# read-only, homepath work/q3home; urt: his UrT 4.3 folder read-only, homepath work/urthome,
# basegame q3ut4, com_hunkMegs 1024.  Copies scripts/tests/*.cfg into the game dir, runs
# "+devmap <map> +wait 300 +exec <cfg>", log -> work/r12-<g>-<name>.log; screenshots named
# "r12-<x>" are moved to work/r12-<g>-<x>.jpg.  EXE_DIR overrides build/Release.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
G="$1"; CFG="$2"; NAME="$3"; MAP="$4"; shift 4
EXE="${EXE_DIR:-$REPO/build/Release}/quake3e-vulkan-ss.x64.exe"
[ -n "$R12_GL" ] && EXE="${EXE_DIR:-$REPO/build/Release}/quake3e-ss.x64.exe"
if [ "$G" = "urt" ]; then
  BASE="${URT_BASEPATH:?set URT_BASEPATH to your Urban Terror 4.3 folder}"; HOMEDIR="$REPO/work/urthome"; GAMEDIR="q3ut4"
  GAMEARGS="+set fs_basegame q3ut4 +set com_hunkMegs 1024"
else
  BASE="${Q3_BASEPATH:?set Q3_BASEPATH to your Quake III folder}"; HOMEDIR="${Q3HOME:-$REPO/work/q3test}"; GAMEDIR="baseq3"
  GAMEARGS="+set com_hunkMegs 128"
fi
mkdir -p "$HOMEDIR/$GAMEDIR"
[ -f "$HOMEDIR/$GAMEDIR/q3config-ss.cfg" ] || [ -f "$HOMEDIR/$GAMEDIR/q3config.cfg" ] || echo "// r12 test config" > "$HOMEDIR/$GAMEDIR/q3config-ss.cfg"
cp "$REPO/scripts/tests/"*.cfg "$HOMEDIR/$GAMEDIR/"
SHOTS="$HOMEDIR/$GAMEDIR/screenshots"
rm -f "$SHOTS"/r12-*.jpg "$HOMEDIR/$GAMEDIR/qconsole.log"
if [ "$MAP" = "-" ]; then START="+wait 200"; else START="+devmap $MAP +wait 300"; fi
"$EXE" $PRE_ARGS +set fs_basepath "$BASE" +set fs_homepath "$(cygpath -w "$HOMEDIR")" $GAMEARGS \
  +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 \
  +set in_gamepad 0 +set developer 1 +set logfile 3 +set com_introplayed 1 \
  +set cl_splitMaxPlayers 8 +set sv_maxclients 16 "$@" \
  $START +exec "$CFG"
cp "$HOMEDIR/$GAMEDIR/qconsole.log" "$REPO/work/r12-$G-$NAME.log"
for f in "$SHOTS"/r12-*.jpg; do
  [ -f "$f" ] || continue
  b="$(basename "$f")"
  mv "$f" "$REPO/work/r12-$G-${b#r12-}"
done
