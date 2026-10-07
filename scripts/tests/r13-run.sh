#!/bin/sh
# usage: scripts/tests/r13-run.sh <cfg> <name> <map|-> [extra args...]
# R13 (Independent mode) runner: the Vulkan Release exe (R13_GL=1: OpenGL) started as
#   quake3e-vulkan-ss.x64.exe --independent --noactivate +set cl_splitIndepArea "64 64 1280 720" ...
# so every window (coordinator + spawned children) is borderless inside that 1280x720 region,
# never takes the foreground and opens below other windows.  The maintainer's Quake 3 folder read-only,
# homepath work/q3home.  Children get "+set developer 1 +exec r13-child@@.cfg" (their own
# scripts; @@ = player number) unless R13_CHILDARGS overrides.  The q3config-ss.cfg of the
# homepath is polled every 0.5 s while the game runs (work/r13-<name>-q3config.txt: mtime,
# and whether the child-only marker "0 1 0" ever appears).  Logs: work/r13-<name>.log
# (coordinator), work/r13-<name>-child<N>.log; screenshots r13-* -> work/r13-<name>-*.jpg.
# After the run: tasklist for leftover quake3e processes -> work/r13-<name>-tasklist.txt.
# R13_BASE / R13_HOME / R13_GAMEDIR override basepath, homepath, game dir (UrT: pass +set fs_basegame q3ut4 too).
# R13_PREFIX (default r13) replaces the "r13" of the work/ evidence names.
# R13_FLAGS replaces "--independent --noactivate" (e.g. "--noactivate" for a Together start; the
# window is always forced to 1280x720 at 64,64 so a Together start is never fullscreen).
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
PFX="${R13_PREFIX:-r13}"	# evidence prefix: work/<PFX>-<name>*
CFG="$1"; NAME="$2"; MAP="$3"; shift 3
EXE="${EXE_DIR:-$REPO/build/Release}/quake3e-vulkan-ss.x64.exe"
[ -n "$R13_GL" ] && EXE="${EXE_DIR:-$REPO/build/Release}/quake3e-ss.x64.exe"
BASE="${R13_BASE:-${Q3_BASEPATH:?set Q3_BASEPATH to your Quake III folder}}"; HOMEDIR="${R13_HOME:-${Q3HOME:-$REPO/work/q3test}}"; GAMEDIR="${R13_GAMEDIR:-baseq3}"
CHILDARGS="${R13_CHILDARGS:-+set developer 1 +exec r13-child@@.cfg}"
mkdir -p "$HOMEDIR/$GAMEDIR"
[ -f "$HOMEDIR/$GAMEDIR/q3config-ss.cfg" ] || [ -f "$HOMEDIR/$GAMEDIR/q3config.cfg" ] || echo "// r13 test config" > "$HOMEDIR/$GAMEDIR/q3config-ss.cfg"
cp "$REPO/scripts/tests/"*.cfg "$HOMEDIR/$GAMEDIR/"
SHOTS="$HOMEDIR/$GAMEDIR/screenshots"
rm -f "$SHOTS"/r13-*.jpg "$HOMEDIR/$GAMEDIR"/qconsole*.log
if [ "$MAP" = "-" ]; then START="+wait 200"; else START="+devmap $MAP +wait 300"; fi

# q3config watcher
Q3C="$HOMEDIR/$GAMEDIR/q3config-ss.cfg"
WATCH="$REPO/work/$PFX-$NAME-q3config.txt"
echo "# t(s) mtime marker(0 1 0)" > "$WATCH"
( t=0; while [ ! -f "$REPO/work/.r13-stop" ]; do
    m=$(stat -c %Y "$Q3C" 2>/dev/null); k=no; grep -q '"0 1 0"' "$Q3C" 2>/dev/null && k=yes
    echo "$t $m $k" >> "$WATCH"; sleep 0.5; t=$((t+1)); done ) &
rm -f "$REPO/work/.r13-stop"

timeout "${R13_TIMEOUT:-300}" "$EXE" ${R13_FLAGS:---independent --noactivate} +set fs_basepath "$BASE" +set fs_homepath "$(cygpath -w "$HOMEDIR")" \
  +set com_hunkMegs 128 +set cl_splitIndepArea "64 64 1280 720" \
  +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set vid_xpos 64 +set vid_ypos 64 \
  +set in_gamepad 0 +set developer 1 +set logfile 4 +set com_introplayed 1 \
  +set cl_splitChildArgs "$CHILDARGS" "$@" \
  $START +exec "$CFG"

touch "$REPO/work/.r13-stop"; sleep 1; rm -f "$REPO/work/.r13-stop"
sleep 2
tasklist 2>/dev/null | grep -i quake3e > "$REPO/work/$PFX-$NAME-tasklist.txt"
echo "(tasklist after the run: $(wc -l < "$REPO/work/$PFX-$NAME-tasklist.txt") quake3e process(es))" >> "$REPO/work/$PFX-$NAME-tasklist.txt"
# never leave one behind (only our own test processes run on this box during a test: checked before)
grep -qE "quake3e-vulkan-ss|quake3e-ss\.x64" "$REPO/work/$PFX-$NAME-tasklist.txt" && taskkill //F //IM quake3e-vulkan-ss.x64.exe //IM quake3e-ss.x64.exe > /dev/null 2>&1
cp "$HOMEDIR/$GAMEDIR/qconsole.log" "$REPO/work/$PFX-$NAME.log"
for f in "$HOMEDIR/$GAMEDIR"/qconsole-child*.log; do
  [ -f "$f" ] || continue
  b="$(basename "$f" .log)"
  cp "$f" "$REPO/work/$PFX-$NAME-${b#qconsole-}.log"
done
for f in "$SHOTS"/r13-*.jpg; do
  [ -f "$f" ] || continue
  b="$(basename "$f")"
  mv "$f" "$REPO/work/$PFX-$NAME-${b#r13-}"
done
