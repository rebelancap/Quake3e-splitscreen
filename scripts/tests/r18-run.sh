#!/bin/sh
# usage: scripts/tests/r18-run.sh <urt|urtauto|urtdrop|q3> <cfg> <name> <map|-> [extra args...]
# R18 (Urban Terror pass) runner, Vulkan Release exe (R18_GL=1: OpenGL), windowed 1280x720 at 64,64,
# --noactivate, developer 1, logfile 3, in_gamepad 0 (virtual pads via padinject).
#   urt     : UrT install read-only as fs_basepath + fs_basegame q3ut4, homepath work/urthome
#   urtauto : the same without fs_basegame and without com_hunkMegs (q3ut4 auto-detect)
#   urtdrop : exe copied into work/r18-urtdrop/ beside a q3ut4 junction to the UrT install's
#             q3ut4 (no fs_basepath, no fs_basegame: the "drop the exes into UrbanTerror43"
#             case), homepath still work/urthome so nothing is written through the junction
#   q3      : Quake 3 install, homepath work/q3test, hunk 128 (baseq3 regressions)
# work/urthome is a live home: q3config*.cfg and both profiles dirs are backed up before and
# restored after (sha256 lists before/after -> work/r18-<name>-home.txt).
# Log -> work/r18-<name>.log; screenshots "r18-<x>" -> work/r18-<x>.jpg.
# Refuses to start while any quake3e process runs (one game instance at a time).
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
MODE="$1"; CFG="$2"; NAME="$3"; MAP="$4"; shift 4
BIN="${EXE_DIR:-$REPO/build/Release}"
EXEN="quake3e-vulkan-ss.x64.exe"; [ -n "$R18_GL" ] && EXEN="quake3e-ss.x64.exe"
EXE="$BIN/$EXEN"
URT="${URT_BASEPATH:?set URT_BASEPATH to your Urban Terror 4.3 folder}"
if tasklist 2>/dev/null | grep -i "quake3e" >/dev/null; then
  echo "r18-run: a quake3e process is running; not starting" >&2
  tasklist | grep -i quake3e >&2
  exit 1
fi
if [ "$MODE" = "q3" ]; then
  HOMED="$REPO/work/q3test"; GAMEDIR=baseq3
else
  HOMED="${R18_URTHOME:-$REPO/work/urthome}"; GAMEDIR=q3ut4   # R21: R18_URTHOME = a test home instead of the live one
fi
mkdir -p "$HOMED/$GAMEDIR"
BK=""
if [ "$MODE" != "q3" ]; then
  BK="$REPO/work/r18-homebackup-$NAME"
  rm -rf "$BK"; mkdir -p "$BK"
  ( cd "$HOMED" && find profiles q3ut4/profiles q3ut4/q3config.cfg q3ut4/q3config-ss.cfg q3ut4/serversettings -type f 2>/dev/null | sort > "$BK/list.txt"
    tar cf "$BK/home.tar" -T "$BK/list.txt" 2>/dev/null
    xargs -a "$BK/list.txt" sha256sum > "$BK/before.txt" 2>/dev/null )
  # our own (empty) q3config when the home has none, so the maintainer's UrT folder's q3config.cfg is not run
  [ -f "$HOMED/q3ut4/q3config-ss.cfg" ] || [ -f "$HOMED/q3ut4/q3config.cfg" ] || echo "// r18 test config" > "$HOMED/q3ut4/q3config-ss.cfg"
fi
cp "$REPO/scripts/tests/"*.cfg "$HOMED/$GAMEDIR/"
# R18_PRE: a shell command run after the backup (e.g. to plant a test profile file; removed by the restore)
[ -n "$R18_PRE" ] && ( cd "$HOMED" && eval "$R18_PRE" )
SHOTS="$HOMED/$GAMEDIR/screenshots"
rm -f "$SHOTS"/r18-*.jpg "$HOMED/$GAMEDIR/qconsole.log"
[ "$MODE" = "q3" ] && rm -f "$SHOTS"/*.jpg   # the test home: old shots of the same name would not be told apart
mkdir -p "$SHOTS"; ls "$SHOTS" > "$REPO/work/.r18-shots-before" 2>/dev/null
if [ "$MAP" = "-" ]; then START="+wait 200"; else START="+devmap $MAP +wait 300"; fi
COMMON="+set fs_homepath $(cygpath -w "$HOMED") +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 +set vid_xpos 64 +set vid_ypos 64 +set in_gamepad 0 +set developer 1 +set logfile 3 +set com_introplayed 1 +set cl_splitMaxPlayers 8 +set sv_maxclients 16"
case "$MODE" in
  urt)     "$EXE" --noactivate +set fs_basepath "$URT" +set fs_basegame q3ut4 +set com_hunkMegs 1024 $COMMON "$@" $START +exec "$CFG" ;;
  urtauto) "$EXE" --noactivate +set fs_basepath "$URT" $COMMON "$@" $START +exec "$CFG" ;;
  urtdrop)
    DROP="$REPO/work/r18-urtdrop"
    rm -rf "$DROP"; mkdir -p "$DROP"
    cp "$EXE" "$DROP/"
    cmd //c mklink //J "$(cygpath -w "$DROP/q3ut4")" "$URT\q3ut4" > /dev/null
    ls -la "$DROP" > "$REPO/work/r18-$NAME-dropdir.txt"
    "$DROP/$EXEN" --noactivate $COMMON "$@" $START +exec "$CFG"
    cmd //c rmdir "$(cygpath -w "$DROP/q3ut4")"     # removes the junction only, never its target
    rm -rf "$DROP" ;;
  q3)      "$EXE" --noactivate +set fs_basepath "${Q3_BASEPATH:?set Q3_BASEPATH to your Quake III folder}" +set com_hunkMegs 128 $COMMON "$@" $START +exec "$CFG" ;;
esac
cp "$HOMED/$GAMEDIR/qconsole.log" "$REPO/work/r18-$NAME.log"
# R18_POST: a shell command run in the home before the restore (e.g. to copy a written profile out)
[ -n "$R18_POST" ] && ( cd "$HOMED" && eval "$R18_POST" )
for f in "$SHOTS"/*.jpg; do
  [ -f "$f" ] || continue
  b="$(basename "$f")"
  case "$b" in
    r18-*) mv "$f" "$REPO/work/$b" ;;
    *) grep -qx "$b" "$REPO/work/.r18-shots-before" || mv "$f" "$REPO/work/r18-$NAME-$b" ;;   # other rounds' test cfgs
  esac
done
rm -f "$REPO/work/.r18-shots-before"
if [ -n "$BK" ]; then
  ( cd "$HOMED"
    find profiles q3ut4/profiles q3ut4/q3config.cfg q3ut4/q3config-ss.cfg q3ut4/serversettings -type f 2>/dev/null | sort > "$BK/after-list.txt"
    xargs -a "$BK/after-list.txt" rm -f
    tar xf "$BK/home.tar"
    xargs -a "$BK/list.txt" sha256sum > "$BK/restored.txt" 2>/dev/null )
  if cmp -s "$BK/before.txt" "$BK/restored.txt"; then echo "home restored byte-identical ($(wc -l < "$BK/list.txt") files)" > "$REPO/work/r18-$NAME-home.txt"
  else echo "HOME RESTORE MISMATCH" > "$REPO/work/r18-$NAME-home.txt"; diff "$BK/before.txt" "$BK/restored.txt" >> "$REPO/work/r18-$NAME-home.txt"; fi
  cat "$REPO/work/r18-$NAME-home.txt"
  rm -rf "$BK"
fi
tasklist 2>/dev/null | grep -i quake3e && echo "r18-run: WARNING quake3e still running" >&2
exit 0
