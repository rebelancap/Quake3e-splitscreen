#!/bin/bash
# usage: scripts/tests/linux-run.sh <vk|gl> <cfg> <name> <map|-> [extra args...]
# Linux counterpart of s6-run.sh/r10-run.sh/r12-run.sh: runs the host build
# (build/release-linux-x86_64/, EXE_DIR overrides): vk = quake3e-vulkan-ss.x64,
# gl = quake3e-ss.x64 (renderer built in; no cl_renderer), game data read-only from ~/dev/q3data
# (Q3DATA overrides), homepath work/q3home.  Copies scripts/tests/*.cfg into
# the game dir, runs "+devmap <map> +wait 300 +exec <cfg>" (map "-" = main
# menu), log -> work/<name>.log; every screenshot the run took is moved to
# work/<name>-<x>.jpg (<x> = the shot name without "<name>-", without <name>'s
# part after its first dash (name l6-r12-aim, shot r12-aim-glyph -> glyph), or
# else without the ported cfgs' round prefix "r<N>[a-z]-" (r8-, r17-, r19b-) and
# then <name>'s last word (name l6-r20-cin, shot r17-cin-hint -> hint).
# Adds in_gamepad 0 developer 1 logfile 3 com_hunkMegs 128 cl_splitMaxPlayers 8
# sv_maxclients 16; pass +set in_gamepad 1 etc. after the map to override.
# PRE_ARGS: launch flags before the first +.  Refuses to start if a quake3e
# client is already running (ours or upstream's quake3e.x64 /
# quake3e-vulkan.x64; the maintainer may be playing).
# From SSH the desktop session's display is exported automatically (XWayland :0).
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
R="$1"; CFG="$2"; NAME="$3"; MAP="$4"; shift 4
BASE="${Q3DATA:-$HOME/dev/q3data}"
HOMEDIR="$REPO/work/q3home"; GAMEDIR="baseq3"
case "$R" in vk) EXENAME=quake3e-vulkan-ss.x64;; gl) EXENAME=quake3e-ss.x64;; *) echo "renderer: vk|gl" >&2; exit 2;; esac
EXE="${EXE_DIR:-$REPO/build/release-linux-x86_64}/$EXENAME"
# by executable name (/proc/<pid>/exe): pgrep -x sees only 15 characters of
# "quake3e-vulkan-ss.x64"
q3_running() {
  local p e
  for p in /proc/[0-9]*; do
    e="$(readlink "$p/exe" 2>/dev/null)" || continue
    e="${e##*/}"; e="${e% (deleted)}"
    case "$e" in quake3e-vulkan-ss.x64|quake3e-ss.x64|quake3e.x64|quake3e-vulkan.x64) echo "${p#/proc/} $e"; return 0;; esac
  done
  return 1
}
if R_PID="$(q3_running)"; then echo "a quake3e client is already running ($R_PID); not starting another" >&2; exit 3; fi
if [ -z "$DISPLAY" ] && [ -z "$WAYLAND_DISPLAY" ]; then
  P="$(pgrep -o plasmashell || pgrep -o kwin_wayland)"
  if [ -n "$P" ]; then
    export $(tr '\0' '\n' < /proc/$P/environ | grep -E '^(DISPLAY|WAYLAND_DISPLAY|XAUTHORITY|XDG_RUNTIME_DIR|XDG_SESSION_TYPE)=' | xargs)
  fi
fi
mkdir -p "$HOMEDIR/$GAMEDIR"
[ -f "$HOMEDIR/$GAMEDIR/q3config-ss.cfg" ] || [ -f "$HOMEDIR/$GAMEDIR/q3config.cfg" ] || echo "// linux test config" > "$HOMEDIR/$GAMEDIR/q3config-ss.cfg"
cp "$REPO/scripts/tests/"*.cfg "$HOMEDIR/$GAMEDIR/"
SHOTS="$HOMEDIR/$GAMEDIR/screenshots"
rm -f "$SHOTS"/*.jpg "$SHOTS"/*.tga "$HOMEDIR/$GAMEDIR/qconsole.log"
if [ "$MAP" = "-" ]; then START="+wait 200"; else START="+devmap $MAP +wait 300"; fi
"$EXE" $PRE_ARGS +set fs_basepath "$BASE" +set fs_homepath "$HOMEDIR" \
  +set r_fullscreen 0 +set r_mode -1 +set r_customwidth 1280 +set r_customheight 720 \
  +set in_gamepad 0 +set developer 1 +set logfile 3 +set com_introplayed 1 +set com_hunkMegs 128 \
  +set cl_splitMaxPlayers 8 +set sv_maxclients 16 "$@" \
  $START +exec "$CFG"
RC=$?
cp "$HOMEDIR/$GAMEDIR/qconsole.log" "$REPO/work/$NAME.log" 2>/dev/null
for f in "$SHOTS"/*.jpg; do
  [ -f "$f" ] || continue
  b="$(basename "$f" .jpg)"
  case "$b" in "$NAME"-*) b="${b#$NAME-}";; "${NAME#*-}"-*) b="${b#${NAME#*-}-}";;
    *) b="$(printf '%s' "$b" | sed -E 's/^r[0-9]+[a-z]*-//')"; b="${b#${NAME##*-}-}";; esac
  mv "$f" "$REPO/work/$NAME-$b.jpg"
done
exit $RC
