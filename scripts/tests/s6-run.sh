#!/bin/sh
# usage: scripts/tests/s6-run.sh <exe> <cfg> <logname> <map|-> [extra args...]
# Like m1-run.sh; map "-" starts at the main menu (no level).  Screenshots
# land in work/q3home/<game>/screenshots; the log is copied to work/<logname>.log.
# PRE_ARGS: launch flags before the first + (R13: PRE_ARGS=--noactivate keeps the window from
# taking the foreground).
# Set GAME=missionpack to read the log from that game dir.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
EXE="$1"; CFG="$2"; LOG="$3"; MAP="$4"; shift 4
GAMEDIR="${GAME:-baseq3}"
HOME_Q3="${Q3HOME:-$REPO/work/q3test}"
mkdir -p "$HOME_Q3/$GAMEDIR"
cp "$REPO/scripts/tests/"*.cfg "$HOME_Q3/$GAMEDIR/"
rm -f "$HOME_Q3/$GAMEDIR/qconsole.log"
if [ "$MAP" = "-" ]; then
  START="+wait 200"
else
  START="+devmap $MAP +wait 300"
fi
"$EXE" $PRE_ARGS +set fs_basepath "${Q3_BASEPATH:?set Q3_BASEPATH to your Quake III folder}" \
  +set fs_homepath "$(cygpath -w "$HOME_Q3")" +set r_fullscreen 0 +set r_mode -1 \
  +set r_customwidth 1280 +set r_customheight 720 +set logfile 2 +set com_introplayed 1 "$@" \
  $START +exec "$CFG"
cp "$HOME_Q3/$GAMEDIR/qconsole.log" "$REPO/work/$LOG.log"
