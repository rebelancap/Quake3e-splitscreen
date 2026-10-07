#!/bin/sh
# usage: scripts/tests/r11-run.sh <vk|gl> <cfg> <name> <map|-> [extra args...]
# Urban Terror 4.3 runner: the maintainer's UrT install read-only as fs_basepath, our own
# homepath work/urthome (never his folder), basegame q3ut4.  Copies scripts/tests/*.cfg
# into work/urthome/q3ut4, runs "+devmap <map> +wait 300 +exec <cfg>" (map "-" = main
# menu), log -> work/r11-<r>-<name>.log; screenshots named "r11-<x>" are moved to
# work/r11-<r>-<x>.jpg.  EXE_DIR overrides build/Release.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
R="$1"; CFG="$2"; NAME="$3"; MAP="$4"; shift 4
EXE="${EXE_DIR:-$REPO/build/Release}/quake3e-vulkan-ss.x64.exe"
[ "$R" = "gl" ] && EXE="${EXE_DIR:-$REPO/build/Release}/quake3e-ss.x64.exe"
URT="${URT_BASEPATH:?set URT_BASEPATH to your Urban Terror 4.3 folder}"
HOME_UT="$REPO/work/urthome"
GAMEDIR="q3ut4"
mkdir -p "$HOME_UT/$GAMEDIR"
# our own (empty) q3config: otherwise the one in the maintainer's UrT folder (basepath) is executed
[ -f "$HOME_UT/$GAMEDIR/q3config-ss.cfg" ] || [ -f "$HOME_UT/$GAMEDIR/q3config.cfg" ] || echo "// r11 test config" > "$HOME_UT/$GAMEDIR/q3config-ss.cfg"
cp "$REPO/scripts/tests/"*.cfg "$HOME_UT/$GAMEDIR/"
SHOTS="$HOME_UT/$GAMEDIR/screenshots"
rm -f "$SHOTS"/r11-*.jpg "$HOME_UT/$GAMEDIR/qconsole.log"
if [ "$MAP" = "-" ]; then
  START="+wait 200"
else
  START="+devmap $MAP +wait 300"
fi
"$EXE" $PRE_ARGS +set fs_basepath "$URT" +set fs_homepath "$(cygpath -w "$HOME_UT")" +set fs_basegame q3ut4 \
  +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 \
  +set in_gamepad 0 +set developer 1 +set logfile 3 +set com_introplayed 1 \
  +set cl_splitMaxPlayers 8 +set sv_maxclients 16 "$@" \
  $START +exec "$CFG"
cp "$HOME_UT/$GAMEDIR/qconsole.log" "$REPO/work/r11-$R-$NAME.log"
for f in "$SHOTS"/r11-*.jpg; do
  [ -f "$f" ] || continue
  b="$(basename "$f")"
  mv "$f" "$REPO/work/r11-$R-${b#r11-}"
done
