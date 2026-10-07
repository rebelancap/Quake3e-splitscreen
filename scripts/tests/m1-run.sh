#!/bin/sh
# usage: scripts/tests/m1-run.sh <exe> <cfg> <logname> [map] [extra args...]
# Runs one windowed test of the splitscreen build with the repo's fs_homepath
# and copies qconsole.log to work/<logname>.log.
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
EXE="$1"; CFG="$2"; LOG="$3"; MAP="${4:-q3dm1}"; shift 4 2>/dev/null
HOME_Q3="${Q3HOME:-$REPO/work/q3test}"
mkdir -p "$HOME_Q3/baseq3"
cp "$REPO/scripts/tests/"*.cfg "$HOME_Q3/baseq3/"
rm -f "$HOME_Q3/baseq3/qconsole.log"
"$EXE" +set fs_basepath "${Q3_BASEPATH:?set Q3_BASEPATH to your Quake III folder}" \
  +set fs_homepath "$(cygpath -w "$HOME_Q3")" +set r_fullscreen 0 +set r_mode -1 \
  +set r_customwidth 1280 +set r_customheight 720 +set logfile 2 "$@" \
  +devmap "$MAP" +wait 300 +exec "$CFG"
cp "$HOME_Q3/baseq3/qconsole.log" "$REPO/work/$LOG.log"
